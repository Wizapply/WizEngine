#pragma once

// Scene の実装ファイル（Scene*.cpp）だけが使う私的ヘッダ。実装を担当ごとに
// 分けたので、共通の下準備（インクルード・エイリアス・小さな変換関数）を
// ここに 1 か所で持つ。公開 API は Scene.h。他のファイルからは含めないこと。
//
//   Scene.cpp           : 構築・ステップ・組み込みコンポーネント・描画への反映
//   SceneEdit.cpp       : エディタ操作（PHYSICS スレッドで Op を実行）
//   SceneEvents.cpp     : マウスの掴みとイベントグラフの実行
//   SceneSerialize.cpp  : シーン文書（XML）との相互変換とブラウザ向け JSON

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
#include <cmath>
#include <cstdio>
#include <cstddef>

#include "core/AssetError.h"
#include "components/GizmoComponent.h"
#include "physics/PhysicsWorld.h"
#include "render/Renderer.h"
#include "scene/MathBridge.h"
#include "scene/PrefabDefaults.h"
#include "scene/RenderBridge.h"
#include "scene/PrefabFrame.h"
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
    return o;
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
