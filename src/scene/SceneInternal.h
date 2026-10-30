#pragma once

// Scene の実装ファイル（Scene*.cpp）だけが使う私的ヘッダ。実装を担当ごとに
// 分けたので、共通の下準備（インクルード・エイリアス・小さな変換関数）を
// ここに 1 か所で持つ。公開 API は Scene.h。他のファイルからは含めないこと。
//
//   Scene.cpp           : 構築・ステップ・組み込みコンポーネント・描画への反映
//   SceneEdit.cpp       : エディタ操作（PHYSICS スレッドで Op を実行）
//   SceneEvents.cpp     : マウスの掴みとイベントグラフの実行
//   SceneSerialize.cpp  : シーン文書（XML）との相互変換とブラウザ向け JSON
//   SceneTerrain.cpp    : テレインの実体（格子・当たり判定・描画）の同期
//   SceneTerrainEdit.cpp: ブラウザの edit.terrain の受け口と操作
//   SceneTerrainFile.cpp: テレインと文書・.terrain・ブラウザの JSON との出入り
//   SceneTerrainPaint.cpp: 塗り分けの道具（自動塗り）

#include "scene/Scene.h"
#include "scene/SceneConfig.h"
#include "physics/MeshCollision.h"
#include "core/Log.h"

#include "scene/SceneMath.h"

#include <chrono/core/ChQuaternion.h>
#include <chrono/core/ChVector3.h>

#include <math/mat4.h>
#include <math/quat.h>
#include <math/vec3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <limits>

#include "core/AssetError.h"
#include "core/Clock.h"
#include "components/GizmoComponent.h"
#include "physics/ModelImport.h"
#include "physics/PhysicsWorld.h"
#include "render/Renderer.h"
#include "scene/MathBridge.h"
#include "scene/PrefabDefaults.h"
#include "scene/RenderBridge.h"
#include "scene/PrefabFrame.h"
#include "terrain/HeightField.h"
#include "terrain/TerrainAutoPaint.h"
#include "terrain/TerrainFile.h"
#include "terrain/TerrainFrame.h"
#include "terrain/TerrainLibrary.h"
#include "terrain/TerrainUndo.h"
#include "vehicle/TireFormula.h"

namespace ed = wizengine::editor;

// Scene*.cpp が共有する小さな変換関数。Chrono の型は完全修飾で書く
// （このヘッダは using namespace chrono; を置かない）。
namespace scene_detail {

// Scene parameters (cameras, lights, physics, ...) live in SceneConfig.h;
// below are only derived helpers and the implementation. シーンの中身
// （オブジェクト・ジョイント・アセット）はここではなく文書（XML）が持つ。

// 既定のマウス操作スクリプト（イベントアセット）の名前。エンジンは「掴んだら
// 動く」を持たず、この名前のアセットがシーンに付いていて初めて物が動く。
// 節を持たない文書と「全消し」はこれを作って付ける（defaultPickupAsset /
// Scene::resetEventsToDefaults）。既定シーン assets/scenes/default.xml にも
// 同じ名前で書いてある。
inline constexpr const char* kMouseEventAsset = "pickup";

// エディタの回転（オイラー角・度）を Chrono の四元数へ。順序の定義は
// SceneMath.h に 1 か所だけ置いてある（インスペクタの数字・ギズモの回転・
// 物理に渡す姿勢が食い違わないように）。
inline chrono::ChQuaternion<> quatFromEuler(const ed::Vec3d& degrees) {
    const scenemath::Quat q =
        scenemath::quatFromEulerDegrees(degrees.x, degrees.y, degrees.z);
    return chrono::ChQuaternion<>(q.w(), q.x(), q.y(), q.z());
}

// エディタの JointKind を PhysicsWorld の enum へ。
inline JointType toJointType(ed::JointKind k) {
    switch (k) {
        case ed::JointKind::Fixed: return JointType::Fixed;
        case ed::JointKind::Spherical: return JointType::Spherical;
        case ed::JointKind::Prismatic: return JointType::Prismatic;
        case ed::JointKind::Distance: return JointType::Distance;
        case ed::JointKind::Universal: return JointType::Universal;
        case ed::JointKind::Cylindrical: return JointType::Cylindrical;
        case ed::JointKind::Planar: return JointType::Planar;
        case ed::JointKind::PointLine: return JointType::PointLine;
        case ed::JointKind::PointPlane: return JointType::PointPlane;
        case ed::JointKind::Gear: return JointType::Gear;
        case ed::JointKind::Screw: return JointType::Screw;
        case ed::JointKind::Spring: return JointType::Spring;
        case ed::JointKind::Bushing: return JointType::Bushing;
        case ed::JointKind::Revolute: break;
    }
    return JointType::Revolute;
}
inline MotorType toMotorType(ed::MotorMode m) {
    switch (m) {
        case ed::MotorMode::Speed: return MotorType::Speed;
        case ed::MotorMode::Position: return MotorType::Position;
        case ed::MotorMode::Force: return MotorType::Force;
        case ed::MotorMode::None: break;
    }
    return MotorType::None;
}

// 文書のジョイント（度・m・N）を PhysicsWorld の指定（rad）へ。角度で持つ
// 欄がどれかは種類とモータで決まるので、変換はここ 1 か所に置く。
// bodyA / bodyB（physId）は呼び出し側が入れる。
inline JointSpec toJointSpec(const ed::JointDesc& j) {
    JointSpec s;
    s.type = toJointType(j.kind);
    s.anchor = chrono::ChVector3d(j.anchor.x, j.anchor.y, j.anchor.z);
    s.axis = chrono::ChVector3d(j.axis.x, j.axis.y, j.axis.z);
    s.distance = j.distance;
    const bool angular = j.kind == ed::JointKind::Revolute ||
                         j.kind == ed::JointKind::Cylindrical ||
                         j.kind == ed::JointKind::Universal;
    const double toRad = scenemath::kPi / 180.0;
    s.limited = j.limited;
    s.limitLo = angular ? j.limitLo * toRad : j.limitLo;
    s.limitHi = angular ? j.limitHi * toRad : j.limitHi;
    s.motor = toMotorType(j.motor);
    // 回転モータの速度と角度は度で書かれている（力 / トルクはそのまま）。
    const bool motorAngular =
        j.kind == ed::JointKind::Revolute && j.motor != ed::MotorMode::Force;
    s.motorTarget = motorAngular ? j.motorTarget * toRad : j.motorTarget;
    s.stiffness = j.stiffness;
    s.damping = j.damping;
    s.breakForce = j.breakForce;
    s.ratio = j.ratio;
    s.pitch = j.pitch;
    s.hasAnchor2 = j.anchor2.x != 0.0 || j.anchor2.y != 0.0 || j.anchor2.z != 0.0;
    s.hasAxis2 = j.axis2.x != 0.0 || j.axis2.y != 0.0 || j.axis2.z != 0.0;
    s.anchor2 = chrono::ChVector3d(j.anchor2.x, j.anchor2.y, j.anchor2.z);
    s.axis2 = chrono::ChVector3d(j.axis2.x, j.axis2.y, j.axis2.z);
    s.rotStiffness = j.rotStiffness;
    s.rotDamping = j.rotDamping;
    return s;
}

// 文書のケーブルを PhysicsWorld の指定へ（端の種類と材質。端のボディ番号
// = physId は呼び出し側が入れる）。
inline CableSpec toCableSpec(const ed::CableDesc& c) {
    CableSpec s;
    s.a = chrono::ChVector3d(c.anchorA.x, c.anchorA.y, c.anchorA.z);
    s.b = chrono::ChVector3d(c.anchorB.x, c.anchorB.y, c.anchorB.z);
    auto end = [](int body) {
        if (body == ed::kCableFreeEnd) return CableSpec::End::Free;
        return body < 0 ? CableSpec::End::Fixed : CableSpec::End::Body;
    };
    s.endA = end(c.bodyA);
    s.endB = end(c.bodyB);
    s.segments = c.segments;
    s.diameter = c.diameter;
    s.density = c.density;
    s.young = c.young;
    s.damping = c.damping;
    s.collide = c.collide;
    return s;
}
// イベントの setMotor が書く目標値も同じ換算（度 → rad）。
inline double motorTargetToPhysics(const ed::JointDesc& j, double value) {
    const bool motorAngular =
        j.kind == ed::JointKind::Revolute && j.motor != ed::MotorMode::Force;
    return motorAngular ? value * scenemath::kPi / 180.0 : value;
}

// ボディの物性・レイヤ・重力を PhysicsWorld の指定へ。
inline BodyOptions toBodyOptions(const ed::BodyDesc& d) {
    BodyOptions o;
    o.friction = d.surface.friction;
    o.restitution = d.surface.restitution;
    o.rolling = d.surface.rolling;
    o.cohesion = d.surface.cohesion;
    o.layer = d.layer;
    o.nocollide = 0;
    for (const int L : d.nocollide) {
        if (L >= 0 && L < ed::kCollisionLayers) o.nocollide |= (1u << L);
    }
    o.gravity = d.gravity;
    o.young = d.surface.young;
    o.poisson = d.surface.poisson;
    o.force = chrono::ChVector3d(d.force.x, d.force.y, d.force.z);
    o.torque = chrono::ChVector3d(d.torque.x, d.torque.y, d.torque.z);
    return o;
}
// 定常荷重を持つか（ChLoad = Core 専用。バックエンドの判定に使う）。
inline bool hasBodyLoads(const ed::BodyDesc& d) {
    return d.force.x != 0.0 || d.force.y != 0.0 || d.force.z != 0.0 ||
           d.torque.x != 0.0 || d.torque.y != 0.0 || d.torque.z != 0.0;
}

// ジョイントの種類ごとの線の色（リニア RGB）。ビューを見ただけで何の拘束か
// 分かるようにしておく。
inline filament::math::float3 jointColor(ed::JointKind k) {
    switch (k) {
        case ed::JointKind::Fixed: return {0.95f, 0.85f, 0.20f};      // 黄
        case ed::JointKind::Spherical: return {0.95f, 0.45f, 0.85f};  // 桃
        case ed::JointKind::Prismatic: return {0.35f, 0.90f, 0.90f};  // 水
        case ed::JointKind::Distance: return {0.60f, 0.95f, 0.40f};   // 黄緑
        case ed::JointKind::Universal: return {0.95f, 0.30f, 0.30f};  // 赤
        case ed::JointKind::Cylindrical: return {0.30f, 0.60f, 0.95f};  // 青
        case ed::JointKind::Planar: return {0.70f, 0.70f, 0.95f};     // 藤
        case ed::JointKind::PointLine: return {0.95f, 0.95f, 0.60f};  // 淡黄
        case ed::JointKind::PointPlane: return {0.60f, 0.95f, 0.95f}; // 淡水
        case ed::JointKind::Gear: return {0.85f, 0.85f, 0.85f};       // 銀
        case ed::JointKind::Screw: return {0.75f, 0.55f, 0.35f};      // 銅
        case ed::JointKind::Spring: return {0.40f, 0.95f, 0.60f};     // 緑
        case ed::JointKind::Bushing: return {0.95f, 0.60f, 0.40f};    // 肌色
        case ed::JointKind::Revolute: break;
    }
    return {1.0f, 0.55f, 0.15f};  // 橙
}

// ---- ライト -----------------------------------------------------------------
inline constexpr double kRadToDeg = 180.0 / scenemath::kPi;
inline constexpr double kDegToRad = scenemath::kPi / 180.0;

// エディタの設計値をレンダラの語彙へ（向きはここでベクトルに戻す）。
inline wizengine::LightDesc toRendererLight(const ed::LightDesc& d) {
    wizengine::LightDesc r;
    switch (d.kind) {
        case ed::LightKind::Sun:
            r.type = wizengine::LightDesc::Type::Directional;
            break;
        case ed::LightKind::Spot:
            r.type = wizengine::LightDesc::Type::Spot;
            break;
        case ed::LightKind::Point:
            r.type = wizengine::LightDesc::Type::Point;
            break;
    }
    r.color = {d.color.r, d.color.g, d.color.b};
    r.intensity = float(d.intensity);
    const scenemath::Vec3 dir =
        scenemath::lightDirection(d.rotation.x, d.rotation.y, d.rotation.z);
    r.direction = {float(dir.x()), float(dir.y()), float(dir.z())};
    r.position = {float(d.position.x), float(d.position.y),
                  float(d.position.z)};
    r.castShadows = d.shadows;
    r.falloffRadius = float(d.falloff);
    r.spotInnerRadians = float(d.spotInnerDeg * kDegToRad);
    r.spotOuterRadians = float(d.spotOuterDeg * kDegToRad);
    return r;
}

// ---- カメラの姿勢表現 -------------------------------------------------------
// CameraObject はオービット（方位角・仰角・距離・注視点）。UI とギズモは
// 「位置（視点）と向き（pitch/yaw 度）」で扱うので、ここで相互変換する。
// 視線 f = target - eye の仰角は -elevation、水平角は azimuth + 180°。
inline double cameraPitchDeg(const CameraObject& c) {
    return -c.elevation() * kRadToDeg;
}
inline double cameraYawDeg(const CameraObject& c) {
    double yaw = c.azimuth() * kRadToDeg + 180.0;
    while (yaw > 180.0) yaw -= 360.0;
    while (yaw < -180.0) yaw += 360.0;
    return yaw;
}
// 方位角・仰角から「注視点 → 視点」の単位ベクトル（eye = target + radius*v）。
inline scenemath::Vec3 orbitVector(double azimuthRad, double elevationRad) {
    return scenemath::Vec3(std::cos(elevationRad) * std::sin(azimuthRad),
                           std::sin(elevationRad),
                           std::cos(elevationRad) * std::cos(azimuthRad));
}

// ---- 計時（重い作業のログ用）-----------------------------------------------
// テレインの生成・メッシュ化の所要時間を出すのに使う（定義は core/Clock.h）。
using wizengine::elapsedMs;

// ---- 地面とテレインの置き場所 -----------------------------------------------
// 位置 + オイラー角（度）→ 姿勢（四元数は SceneMath の 1 か所の定義）。
inline BodyTransform placementPose(const ed::Vec3d& pos, const ed::Vec3d& eulerDeg) {
    const scenemath::Quat q =
        scenemath::quatFromEulerDegrees(eulerDeg.x, eulerDeg.y, eulerDeg.z);
    return BodyTransform{pos.x, pos.y, pos.z, q.w(), q.x(), q.y(), q.z()};
}
inline bool samePose(const BodyTransform& a, const BodyTransform& b) {
    return a.px == b.px && a.py == b.py && a.pz == b.pz && a.qw == b.qw && a.qx == b.qx &&
           a.qy == b.qy && a.qz == b.qz;
}

// ---- テレイン -----------------------------------------------------------------
// 文書の値 → 高さ格子の生成パラメータ（src/terrain は文書の型を知らない）。
// 旧形式の互換読込（SceneTerrainFile.cpp）とノイズ生成（SceneTerrain.cpp）が使う。
inline wizengine::terrain::TerrainParams toTerrainParams(const ed::TerrainDesc& d,
                                                         const ed::TerrainNoiseParams& n) {
    wizengine::terrain::TerrainParams p;
    p.size = float(d.size);
    p.resolution = d.resolution;
    p.height = float(d.height);
    // 格子は原点基準（置き場所はテレインのフレーム = terrain/TerrainFrame.h）。
    p.posX = 0.0f;
    p.posY = 0.0f;
    p.posZ = 0.0f;
    p.seed = n.seed;
    p.octaves = n.octaves;
    p.gain = float(n.gain);
    p.lacunarity = float(n.lacunarity);
    p.scale = float(n.scale);
    return p;
}

// ---- 塗り分けの層（.terrain の LAYR）-----------------------------------------
// 層の値の検査と JSON の唯一の定義（型は terrain::TerrainLayer 1 つ。文書の型には
// 写さない）。ファイルの読込（readTerrainFile）・ブラウザの変更（editTerrainLayer）・
// 旧属性の取り込み（adoptLegacySurface）が同じ検査を通る。

// 層の表示名（index = 0 始まりの層の番号、表示は 1 始まり）。ステータスの文と
// /scene の terrain.layers[i].label（ブラウザの層の欄・自動塗り・筆の選択）が
// 同じ名前を出すための 1 か所。
inline std::string terrainLayerLabel(std::size_t index) {
    return "層 " + std::to_string(index + 1);
}

// 層の値を常識的な範囲へ: 3 つの画像（色 texture・法線 normal・マスク mask）は
// assets/ 相対のパスで、長さは terrain::kMaxLayerPathBytes 以下だけ（外れたら空に
// して LOGW。where = どこから来た値か、英語。長すぎるパスを残すと保存のたびに
// encode が失敗する）、tile は地面と同じ範囲（ed::kSurfaceTileMin〜Max。NaN・無限は
// 既定値へ倒してから丸める - std::clamp は NaN を素通しするため）。roughness /
// metallic は NaN・負なら「テレイン全体に従う」(kLayerFollowsTerrain)、ほかは
// 粗さ ed::kRoughnessMin〜1・金属 0〜1。normalScale は 0〜4（NaN は既定の 1）。
inline void sanitizeTerrainLayers(wizengine::terrain::TerrainLayers& layers,
                                  const char* where) {
    using wizengine::terrain::layerFollowsTerrain;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        wizengine::terrain::TerrainLayer& l = layers[i];
        auto sanitizePath = [&](std::string& path, const char* what) {
            if (path.size() > wizengine::terrain::kMaxLayerPathBytes) {
                LOGW("scene", "terrain layer %zu (%s): %s path is longer than %zu bytes "
                              "- cleared",
                     i, where, what, std::size_t(wizengine::terrain::kMaxLayerPathBytes));
                path.clear();
            } else if (!path.empty() && !ed::assetFileAllowed(path)) {
                LOGW("scene", "terrain layer %zu (%s): %s '%s' is not allowed "
                              "(assets-relative paths only) - cleared",
                     i, where, what, path.c_str());
                path.clear();
            }
        };
        for (const wizengine::terrain::LayerImage image : wizengine::terrain::kLayerImages)
            sanitizePath(wizengine::terrain::layerImagePath(l, image),
                         wizengine::terrain::layerImageName(image));
        if (!std::isfinite(l.tile)) l.tile = wizengine::terrain::TerrainLayer{}.tile;
        l.tile = std::clamp(l.tile, float(ed::kSurfaceTileMin), float(ed::kSurfaceTileMax));
        // 負（と NaN）は「全体に従う」の値にそろえる。
        auto followOr = [](float v, float lo, float hi) {
            return (std::isnan(v) || layerFollowsTerrain(v))
                       ? wizengine::terrain::kLayerFollowsTerrain
                       : std::clamp(v, lo, hi);
        };
        l.roughness = followOr(l.roughness, float(ed::kRoughnessMin), 1.0f);
        l.metallic = followOr(l.metallic, 0.0f, 1.0f);
        if (std::isnan(l.normalScale))
            l.normalScale = wizengine::terrain::TerrainLayer{}.normalScale;
        l.normalScale = std::clamp(l.normalScale, 0.0f, 4.0f);
    }
}

// /scene の terrain.layers: [{texture, tile, normal, mask, roughness, metallic,
// normalScale, label}, ...]（層の数ぶん、順は層の番号。roughness / metallic は
// テレイン全体に従うなら null。label は表示名 = terrainLayerLabel で、読み込み側は
// 見ない）。
inline nlohmann::json layersToJson(const wizengine::terrain::TerrainLayers& layers) {
    using wizengine::terrain::layerFollowsTerrain;
    auto orNull = [](float v) {
        return layerFollowsTerrain(v) ? nlohmann::json(nullptr) : nlohmann::json(v);
    };
    nlohmann::json a = nlohmann::json::array();
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const wizengine::terrain::TerrainLayer& l = layers[i];
        a.push_back(nlohmann::json{{"texture", l.texture},
                                   {"tile", l.tile},
                                   {"normal", l.normal},
                                   {"mask", l.mask},
                                   {"roughness", orNull(l.roughness)},
                                   {"metallic", orNull(l.metallic)},
                                   {"normalScale", l.normalScale},
                                   {"label", terrainLayerLabel(i)}});
    }
    return a;
}

// edit.terrain の layer の部分更新: 送られたキー（texture / normal / mask / tile /
// roughness / metallic / normalScale）だけ base から変える。型違いのキーは無視。
// roughness / metallic は null を送ると「テレイン全体に従う」
// (kLayerFollowsTerrain)。検査（範囲を含む）は呼び手が sanitizeTerrainLayers で
// 掛ける。数値は float に入る範囲へ寄せてから float にするだけ（1e39 などをそのまま
// float にすると無限大になり、検査で上限ではなく既定値へ倒れるため）。NaN は
// base のまま。
inline wizengine::terrain::TerrainLayer layerPatchFromJson(
    const nlohmann::json& j, const wizengine::terrain::TerrainLayer& base) {
    wizengine::terrain::TerrainLayer l = base;
    auto text = [&](const char* key, std::string& field) {
        if (const auto it = j.find(key); it != j.end() && it->is_string())
            field = it->get<std::string>();
    };
    auto number = [&](const char* key, float& field) {
        const double v = ed::jsonNumber(j, key, double(field));
        if (!std::isnan(v)) {
            constexpr double kFloatMax = double(std::numeric_limits<float>::max());
            field = float(std::clamp(v, -kFloatMax, kFloatMax));
        }
    };
    // null = 全体に従う。数なら number と同じ。
    auto numberOrFollow = [&](const char* key, float& field) {
        if (const auto it = j.find(key); it != j.end() && it->is_null()) {
            field = wizengine::terrain::kLayerFollowsTerrain;
            return;
        }
        number(key, field);
    };
    text("texture", l.texture);
    text("normal", l.normal);
    text("mask", l.mask);
    number("tile", l.tile);
    number("normalScale", l.normalScale);
    numberOrFollow("roughness", l.roughness);
    numberOrFollow("metallic", l.metallic);
    return l;
}

// ---- 自動塗りの規則（SceneTerrainPaint.cpp）-----------------------------------
// 規則の JSON の唯一の定義: {softness, layers:[{enabled, slopeMin, slopeMax,
// heightMin, heightMax}, ...]}（層の数ぶん、順は層の番号。傾きは度、高さは
// テレインの置き場所の高さ = 格子の基準 baseY からの m。最低点ではない）。/scene の terrainTools.autoPaintDefaults（terrainJson）と
// edit.terrain の autopaint（autoPaintTerrain）が使う。
nlohmann::json autoPaintRulesToJson(const wizengine::terrain::AutoPaintRules& rules);
// 送られたキーだけ base から変え、clampAutoPaintRules を通して返す。型違いの
// キー・配列でない layers・余分な要素は無視（投げない）。
wizengine::terrain::AutoPaintRules autoPaintRulesFromJson(
    const nlohmann::json& j, const wizengine::terrain::AutoPaintRules& base);

// d を書き換える: 形（size / resolution / height）を新規テレインの既定値
// （ed::TerrainDesc{}）へ戻し、その大きさの平らな格子（原点基準）を返す。
// 位置・回転と表面はそのまま。
// 新規（SceneTerrainEdit.cpp の newTerrain）が使う。
inline std::shared_ptr<const wizengine::terrain::HeightField> flatDefaultTerrain(
    ed::TerrainDesc& d) {
    const ed::TerrainDesc def;
    d.size = def.size;
    d.resolution = def.resolution;
    d.height = def.height;
    return std::make_shared<const wizengine::terrain::HeightField>(
        d.resolution, float(d.size), 0.0f, 0.0f, 0.0f);
}

// ---- シーンファイルの読み込み -----------------------------------------------
// 保存名から assets/scenes の文書を読む。今の形式は XML（*.xml）で、同じ名前の
// XML が無ければ旧形式（*.json、version 1〜3）を探す。どちらも読めなければ
// false と理由を返す。読み取りの警告（打ち間違い・未対応の節など。
// SceneDocument.h の「読み取りの約束」）は全文をログに出し、件数を
// warningCount へ返す - ステータス行に「警告 N 件」と出すため。
inline bool readSceneDocument(const std::string& name, ed::SceneDocument& doc,
                       std::string& reason,
                       std::size_t* warningCount = nullptr) {
    if (warningCount) *warningCount = 0;
    const std::string xmlPath = EditorState::scenePath(name);
    if (xmlPath.empty()) {
        reason = "invalid name (letters, digits, _ - only)";
        return false;
    }
    std::string text;
    std::string xmlReason;
    if (EditorState::readText(xmlPath, text, xmlReason)) {
        std::string error;
        std::vector<std::string> warnings;
        if (!ed::parseXml(text, doc, error, &warnings)) {
            reason = "unreadable XML (" + error + "): " + xmlPath;
            return false;
        }
        for (const auto& w : warnings) {
            LOGW("editor", "scene '%s': %s", name.c_str(), w.c_str());
        }
        if (warningCount) *warningCount = warnings.size();
        return true;
    }
    // 旧形式へのフォールバック。以後の保存は XML になる（JSON は上書きしない）。
    const std::string jsonPath = EditorState::legacyScenePath(name);
    nlohmann::json legacy;
    std::string jsonReason;
    if (!jsonPath.empty() && EditorState::readJson(jsonPath, legacy, jsonReason)) {
        doc = ed::fromLegacyJson(legacy);
        LOGI("editor", "loaded legacy json scene '%s' (saving writes xml)",
             name.c_str());
        return true;
    }
    reason = jsonReason.empty() ? xmlReason : jsonReason;
    return false;
}

}  // namespace scene_detail
