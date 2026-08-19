#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

// エディタモードが編集する「シーン文書」の型。
//
// ここにあるのは値だけ（Chrono も Filament も出てこない）ので、物理・描画・
// HTTP のどのスレッドからも安全にコピーできる。実体の生成は Scene が行い、
// この記述子はその「設計図」にあたる:
//
//   BodyDesc  … 1個の剛体（形・大きさ・置いた場所・質量・色）
//   JointDesc … 2個の剛体をつなぐ拘束
//   SimSettings … シミュレート側の設定（重力・摩擦・レート等）
//
// 保存/読み込みは全部この型の JSON 化で済ませる（assets/scenes/*.json）。
namespace wizengine {
namespace editor {

// エディタ（配置・設計）とシミュレート（実行）の2モード。
enum class AppMode { Editor, Simulate };

inline const char* modeName(AppMode m) {
    return m == AppMode::Editor ? "editor" : "simulate";
}
inline AppMode modeFromName(const std::string& s, AppMode fallback) {
    if (s == "editor") return AppMode::Editor;
    if (s == "simulate" || s == "sim" || s == "play") return AppMode::Simulate;
    return fallback;
}

// 剛体の形。Model は glTF モデル（シーン文書の <asset> 節で相対パスを宣言し、
// BodyDesc::mesh が名前で参照する）。Box / Sphere は組み込みメッシュ。
enum class ShapeKind { Box, Sphere, Model };

inline const char* shapeName(ShapeKind s) {
    switch (s) {
        case ShapeKind::Sphere: return "sphere";
        case ShapeKind::Model: return "model";
        case ShapeKind::Box: break;
    }
    return "box";
}
inline ShapeKind shapeFromName(const std::string& s, ShapeKind fallback) {
    if (s == "box") return ShapeKind::Box;
    if (s == "sphere") return ShapeKind::Sphere;
    if (s == "model") return ShapeKind::Model;
    return fallback;
}

// 拘束の種類。Chrono の対応クラスは PhysicsWorld::addJoint を参照。
enum class JointKind { Fixed, Revolute, Spherical, Prismatic, Distance };

inline const char* jointName(JointKind k) {
    switch (k) {
        case JointKind::Fixed: return "fixed";
        case JointKind::Spherical: return "spherical";
        case JointKind::Prismatic: return "prismatic";
        case JointKind::Distance: return "distance";
        case JointKind::Revolute: break;
    }
    return "revolute";
}
inline JointKind jointFromName(const std::string& s, JointKind fallback) {
    if (s == "fixed") return JointKind::Fixed;
    if (s == "revolute" || s == "hinge") return JointKind::Revolute;
    if (s == "spherical" || s == "ball") return JointKind::Spherical;
    if (s == "prismatic" || s == "slider") return JointKind::Prismatic;
    if (s == "distance" || s == "rod") return JointKind::Distance;
    return fallback;
}

// 選択中のオブジェクトに出すギズモ（Unity の W / E / R に相当）。
enum class GizmoMode { Translate, Rotate, Scale };
// 軸の向きをワールドに合わせるか、オブジェクト自身の向きに合わせるか。
enum class GizmoSpace { World, Local };

inline const char* gizmoModeName(GizmoMode m) {
    switch (m) {
        case GizmoMode::Rotate: return "rotate";
        case GizmoMode::Scale: return "scale";
        case GizmoMode::Translate: break;
    }
    return "translate";
}
inline GizmoMode gizmoModeFromName(const std::string& s, GizmoMode fallback) {
    if (s == "translate" || s == "move") return GizmoMode::Translate;
    if (s == "rotate") return GizmoMode::Rotate;
    if (s == "scale") return GizmoMode::Scale;
    return fallback;
}
inline const char* gizmoSpaceName(GizmoSpace s) {
    return s == GizmoSpace::Local ? "local" : "world";
}
inline GizmoSpace gizmoSpaceFromName(const std::string& s, GizmoSpace fallback) {
    if (s == "world" || s == "global") return GizmoSpace::World;
    if (s == "local" || s == "self") return GizmoSpace::Local;
    return fallback;
}

// ギズモの設定。ブラウザの Inspector タブから変える。
struct GizmoSettings {
    GizmoMode mode = GizmoMode::Translate;
    GizmoSpace space = GizmoSpace::World;
    bool snap = false;
    double moveStep = 0.25;   // m
    double rotateStep = 15.0; // 度
    double scaleStep = 0.1;   // 倍率ではなく寸法の刻み (m)
    // Y=0 の格子グリッド（エディタ中の置き場の目印）。
    bool grid = true;
    double gridStep = 1.0;    // 格子の間隔 (m)
};

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct Color3 {
    float r = 0.80f, g = 0.36f, b = 0.18f;
};

// glTF モデルのアセット宣言（シーン文書の <asset><mesh .../>）。file は
// assets/ からの相対パス（".." や絶対パスは読み込みで弾く）。scale はモデル
// 単位からメートルへの素の倍率で、当たり判定の寸法（BodyDesc::size）とは
// 独立 - 見た目はアーティストの出力そのまま、当たりはエディタで決める。
struct MeshAssetDesc {
    std::string name;
    std::string file;
    double scale = 1.0;
};

// 1個の剛体の設計値。position/rotation は「エディタで置いた姿勢」で、
// シミュレートを止めるとここに戻る（＝オーサリング状態は壊れない）。
struct BodyDesc {
    std::string name;
    ShapeKind shape = ShapeKind::Box;
    // shape=Model のとき、どの glTF モデルで描くか（シーン文書の <asset> 節に
    // ある <mesh> の名前）。空や未知の名前は球へフォールバックする。
    std::string mesh;
    // 当たり判定の形。ふつうは shape と同じだが、既存シーンのように
    //「見た目は glTF モデル・当たりは球」という組み合わせがあるので分けて
    // 持つ。collision=Model は「モデルの凸包」の意味で、読めなければ球。
    ShapeKind collision = ShapeKind::Box;
    // Box は各辺の長さ、Sphere と Model は size.x を直径として使う。
    Vec3d size{0.5, 0.5, 0.5};
    Vec3d position{0.0, 1.0, 0.0};
    Vec3d rotation{0.0, 0.0, 0.0};  // オイラー角（度, X→Y→Z の順）
    double mass = 1.0;              // kg。密度は体積から逆算する
    bool fixed = false;             // true = 動かない土台
    Color3 color;

    // 形状から体積を出す。密度 = mass / volume を Chrono に渡すので、
    // 形や大きさを変えても質量は指定どおりに保たれる。見た目ではなく
    // **当たり判定の形**で計算する（質量は物理側の量なので）。凸包
    // （collision=Model）は外接する箱の体積を見積もりに使う。
    double volume() const {
        if (collision == ShapeKind::Sphere) {
            const double r = size.x * 0.5;
            return (4.0 / 3.0) * 3.14159265358979323846 * r * r * r;
        }
        const double v = size.x * size.y * size.z;
        return v > 1e-9 ? v : 1e-9;
    }
    double density() const {
        const double v = volume();
        return (mass > 0.0 && v > 1e-9) ? mass / v : 1000.0;
    }
    // 選択判定・描画で使う代表半径（Sphere/Model）と半サイズ（Box）。
    double radius() const { return size.x * 0.5; }
};

// 2つの剛体（または剛体と地面）をつなぐ拘束。anchor / axis はワールド座標で
// 持つ: エディタ上で「ここを軸に回す」と指定した位置と向きそのもの。
// bodyA / bodyB は オブジェクト番号。-1 は「地面（ワールド）」。
struct JointDesc {
    std::string name;
    JointKind kind = JointKind::Revolute;
    int bodyA = -1;
    int bodyB = -1;
    Vec3d anchor{0.0, 1.0, 0.0};
    Vec3d axis{0.0, 1.0, 0.0};
    double distance = 0.0;  // Distance のみ。0 = 現在の間隔を維持
};

// ---- ライト -----------------------------------------------------------------
// Sun は平行光（位置は届く光に影響せず、アイコンの置き場でしかない）、Point は
// 全方向、Spot は向いた先への円錐。向きは rotation（オイラー角・度）で持ち、
// 実際の方向ベクトルは「回転ゼロ = 真下 (0,-1,0)」を回したもの
// （scenemath::lightDirection）。kind / shadows / falloff / 円錐角は Filament の
// ライト実体を決めるので、変更は実体の作り直し（rebuild）として扱う。
enum class LightKind { Sun, Point, Spot };

inline const char* lightKindName(LightKind k) {
    switch (k) {
        case LightKind::Sun: return "sun";
        case LightKind::Spot: return "spot";
        case LightKind::Point: break;
    }
    return "point";
}
inline LightKind lightKindFromName(const std::string& s, LightKind fallback) {
    if (s == "sun" || s == "directional") return LightKind::Sun;
    if (s == "point") return LightKind::Point;
    if (s == "spot") return LightKind::Spot;
    return fallback;
}

// 1 灯の設計値。intensity の単位は Sun がルクス（太陽 ~10万）、Point / Spot が
// ルーメン（60W 電球 ~800 lm。太陽下のシーンでは驚くほど大きな値が要る）。
struct LightDesc {
    std::string name;
    LightKind kind = LightKind::Point;
    Vec3d position{0.0, 3.0, 0.0};
    Vec3d rotation{0.0, 0.0, 0.0};  // オイラー角（度）。ゼロ = 真下を向く
    Color3 color{1.0f, 1.0f, 1.0f};
    double intensity = 300000.0;
    double falloff = 25.0;          // Point/Spot の届く距離 (m)
    double spotInnerDeg = 25.0;     // Spot: 全力の円錐（半頂角・度）
    double spotOuterDeg = 35.0;     // Spot: 減衰しきる円錐（半頂角・度）
    bool shadows = false;
};

// ---- 地面と環境光 -----------------------------------------------------------
// どちらもシーンの一部（文書の <worldbody> の <ground/> と <environment/>）。
// 節を書かない文書はこの既定値で開く（ライト・カメラと同じ扱い）。

// 地面。物理の床（当たり判定の箱）と見える地面（テクスチャ付きの板）は
// 半分の広さを別々に持つ - 作業の目安になる床は見えている範囲より広く
// 効いていてほしいため。texture は assets/ からの相対パス（空 = 市松模様）。
struct GroundDesc {
    double half = 10.0;        // 物理の床の半分の広さ (m)。MuJoCo と同じ半寸法
    double visualHalf = 8.0;   // 見える地面の半分の広さ (m)
    std::string texture = "textures/ground.png";
    double tile = 2.0;         // テクスチャ 1 リピートが覆うメートル
    Color3 tint{1.0f, 1.0f, 1.0f};  // テクスチャに乗す色（白 = 画像のまま）
};

// 環境光（IBL）。assets/ の Radiance .hdr を GPU 上でキューブマップ化して
// 使う。空 = 環境マップ無し（一様な弱いアンビエントのみ）。
struct EnvironmentDesc {
    std::string hdr = "studio.hdr";
    double intensity = 30000.0;
};

// ---- カメラ -----------------------------------------------------------------
// 1 台ぶんの姿勢。実体（CameraObject）と同じオービット表現のまま保存する。
// UI が見せる「位置・向き」へは scene.cpp が変換する（eye = target + radius *
// 軌道ベクトル、pitch/yaw ⇄ elevation/azimuth）。active=false のスロットは
// 「削除された」カメラ: エンドポイントは起動時に kMaxCameras ぶん作られるので、
// 実行中の追加・削除はこのフラグの上げ下げになる。
struct CameraPose {
    double azimuth = 0.66;    // ラジアン（Y 軸まわり）
    double elevation = 0.34;  // ラジアン（水平から上向き）
    double radius = 12.0;     // 注視点までの距離 (m)
    Vec3d target{0.0, 1.0, 0.0};
    bool active = true;
};

// ---- イベントグラフ（ノードベースのイベント設計）---------------------------
// Node-RED 風の「トリガー → アクション」グラフ。ノードは値だけの設計図で、
// 実行（トリガー判定とアクション適用）はシミュレート中に物理スレッドが行う
// （Scene::runEventGraph）。アクションが変えた色や強さは「実行時の上書き」で、
// desc（設計値）は書き換えない - シミュレートを止めると全部元に戻る。
// 姿勢が desc へ戻るのと同じ原則。
enum class NodeKind {
    // トリガー（右の出力ポートから発火）
    OnCollision,  // 対象オブジェクトが何かに「新しく」触れた（接触の立ち上がり）
    OnSimStart,   // シミュレート開始の最初のステップ
    OnTimer,      // seconds ごとに繰り返し
    // アクション（左の入力ポートで受ける）
    SetColor,        // オブジェクトの色を color へ（実行時のみ）
    ApplyImpulse,    // オブジェクトに速度変化 vec (m/s) を与える
    SetFixed,        // value != 0 で固定、0 で解除（実行時のみ）
    SetLightColor,   // ライトの色を color へ（実行時のみ）
    SetLightIntensity,  // ライトの強さを value へ（実行時のみ）
    CameraLookAt,    // カメラ target の注視点をオブジェクト other へ向ける
};

inline const char* nodeKindName(NodeKind k) {
    switch (k) {
        case NodeKind::OnSimStart: return "onStart";
        case NodeKind::OnTimer: return "onTimer";
        case NodeKind::SetColor: return "setColor";
        case NodeKind::ApplyImpulse: return "impulse";
        case NodeKind::SetFixed: return "setFixed";
        case NodeKind::SetLightColor: return "lightColor";
        case NodeKind::SetLightIntensity: return "lightIntensity";
        case NodeKind::CameraLookAt: return "cameraLookAt";
        case NodeKind::OnCollision: break;
    }
    return "onCollision";
}
inline NodeKind nodeKindFromName(const std::string& s, NodeKind fallback) {
    if (s == "onCollision" || s == "collision") return NodeKind::OnCollision;
    if (s == "onStart" || s == "start") return NodeKind::OnSimStart;
    if (s == "onTimer" || s == "timer") return NodeKind::OnTimer;
    if (s == "setColor" || s == "color") return NodeKind::SetColor;
    if (s == "impulse" || s == "push") return NodeKind::ApplyImpulse;
    if (s == "setFixed" || s == "fixed") return NodeKind::SetFixed;
    if (s == "lightColor") return NodeKind::SetLightColor;
    if (s == "lightIntensity") return NodeKind::SetLightIntensity;
    if (s == "cameraLookAt" || s == "lookAt") return NodeKind::CameraLookAt;
    return fallback;
}

// トリガーかアクションか。ワイヤーは「トリガー → アクション」の向きだけ。
inline bool nodeIsTrigger(NodeKind k) {
    return k == NodeKind::OnCollision || k == NodeKind::OnSimStart ||
           k == NodeKind::OnTimer;
}

// ノードの target 欄が指す種別。番号の検証・削除時の掃除・保存時の詰め替えは
// 全部これで分岐する（object と light は保存で番号が詰まるため）。
enum class NodeTargetKind { None, Object, Light, Camera };

inline NodeTargetKind nodeTargetKind(NodeKind k) {
    switch (k) {
        case NodeKind::OnCollision:
        case NodeKind::SetColor:
        case NodeKind::ApplyImpulse:
        case NodeKind::SetFixed: return NodeTargetKind::Object;
        case NodeKind::SetLightColor:
        case NodeKind::SetLightIntensity: return NodeTargetKind::Light;
        case NodeKind::CameraLookAt: return NodeTargetKind::Camera;
        case NodeKind::OnSimStart:
        case NodeKind::OnTimer: break;
    }
    return NodeTargetKind::None;
}

// other 欄がオブジェクト番号を指すか（OnCollision の相手フィルタと
// CameraLookAt の注視先）。削除時の掃除と保存時の詰め替えに使う。
inline bool nodeOtherIsObject(NodeKind k) {
    return k == NodeKind::OnCollision || k == NodeKind::CameraLookAt;
}

// 1 個のノード。id はグラフ内で一意（削除しても再利用しない - ワイヤーが
// 別のノードを指し直してしまうため）。使わない欄は既定値のまま持つ:
// 種類ごとに構造体を分けるより、UI・JSON・実行の全部が単純になる。
struct NodeDesc {
    int id = 0;
    NodeKind kind = NodeKind::OnCollision;
    double x = 40.0, y = 40.0;  // ノードエディタのキャンバス座標 (px)
    // 対象番号。nodeTargetKind(kind) の種別を指す。-1 は OnCollision では
    // 「どのオブジェクトでも」、アクションでは「未設定（何もしない）」。
    int target = -1;
    // OnCollision: 相手のフィルタ（-2 = 何でも, -1 = 地面, n = オブジェクト）。
    // CameraLookAt: 注視するオブジェクト番号。
    int other = -2;
    double seconds = 1.0;                // OnTimer の間隔
    Color3 color{0.0f, 0.0f, 0.0f};      // SetColor / SetLightColor（既定 = 黒）
    Vec3d vec{0.0, 5.0, 0.0};            // ApplyImpulse の速度変化 (m/s)
    double value = 0.0;                  // SetFixed(0/1) / SetLightIntensity
};

// トリガーの出力からアクションの入力へ 1 本。多対多を許す（1 トリガーで
// 複数アクション、複数トリガーから同じアクション）。
struct WireDesc {
    int from = -1;  // トリガーノードの id
    int to = -1;    // アクションノードの id
};

// シミュレート側の設定。ここの値はエディタで編集し、シミュレート開始時に
// PhysicsWorld へ流し込む（実行中の変更も反映される）。
struct SimSettings {
    double gravity = -9.81;  // m/s^2（-Y 方向）
    int hz = 60;             // 物理更新レート
    int substeps = 2;
    int iterations = 60;
    double envelope = 0.002;  // 接触エンベロープ (m)
    double recovery = 0.2;    // めり込み解消速度 (m/s)
    float friction = 0.6f;
    float restitution = 0.0f;
    double linearDamping = 0.15;   // 1/s
    double angularDamping = 0.60;  // 1/s
    bool sleeping = true;
};

// ---- JSON 変換 ------------------------------------------------------------
// nlohmann の ADL 版（to_json/from_json）ではなく明示的な関数にしてある。
// 「どのキーが出るか」がそのまま保存フォーマットとブラウザ API になるので、
// 一箇所で読めるほうがよい。

// 型が違っても投げない数値・整数の取り出し。/input は誰でも叩けるので、
// 文字列などを混ぜたリクエストで nlohmann の value() が type_error を投げ、
// 処理スレッドごと落ちる - それを既定値へ落として続行するための口。
inline double jsonNumber(const nlohmann::json& j, const char* key,
                         double fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}
inline int jsonInt(const nlohmann::json& j, const char* key, int fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? int(it->get<double>())
                                              : fallback;
}

inline nlohmann::json toJson(const Vec3d& v) {
    return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
}
inline Vec3d vec3FromJson(const nlohmann::json& j, const Vec3d& fallback) {
    if (!j.is_object()) return fallback;
    Vec3d v;
    v.x = jsonNumber(j, "x", fallback.x);
    v.y = jsonNumber(j, "y", fallback.y);
    v.z = jsonNumber(j, "z", fallback.z);
    return v;
}

// 色は UI 側の <input type="color"> に合わせて "#rrggbb"。sRGB ではなく
// リニア値をそのまま 0-255 に写す（マテリアルがリニアを受け取るため）。
inline std::string colorToHex(const Color3& c) {
    auto ch = [](float v) {
        const int i = int(v * 255.0f + 0.5f);
        return i < 0 ? 0 : (i > 255 ? 255 : i);
    };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", ch(c.r), ch(c.g), ch(c.b));
    return std::string(buf);
}
inline Color3 colorFromHex(const std::string& hex, const Color3& fallback) {
    if (hex.size() != 7 || hex[0] != '#') return fallback;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = nib(hex[std::size_t(i) + 1]);
        if (v[i] < 0) return fallback;
    }
    Color3 c;
    c.r = float(v[0] * 16 + v[1]) / 255.0f;
    c.g = float(v[2] * 16 + v[3]) / 255.0f;
    c.b = float(v[4] * 16 + v[5]) / 255.0f;
    return c;
}

inline nlohmann::json toJson(const MeshAssetDesc& m) {
    nlohmann::json j;
    j["name"] = m.name;
    j["file"] = m.file;
    j["scale"] = m.scale;
    return j;
}

inline nlohmann::json toJson(const BodyDesc& b) {
    nlohmann::json j;
    j["name"] = b.name;
    j["shape"] = shapeName(b.shape);
    if (b.shape == ShapeKind::Model) j["mesh"] = b.mesh;
    j["collision"] = shapeName(b.collision);
    j["size"] = toJson(b.size);
    j["position"] = toJson(b.position);
    j["rotation"] = toJson(b.rotation);
    j["mass"] = b.mass;
    j["fixed"] = b.fixed;
    j["color"] = colorToHex(b.color);
    return j;
}

inline BodyDesc bodyFromJson(const nlohmann::json& j, const BodyDesc& base) {
    BodyDesc b = base;
    if (!j.is_object()) return b;
    if (j.contains("name") && j["name"].is_string()) b.name = j["name"];
    if (j.contains("shape") && j["shape"].is_string()) {
        b.shape = shapeFromName(j["shape"], b.shape);
        // collision の指定が無いときは見た目に合わせる（エディタで置いた物は
        // 常にこれ。分けて持つのは既存シーンの都合だけなので）。
        if (!j.contains("collision")) b.collision = b.shape;
    }
    if (j.contains("collision") && j["collision"].is_string()) {
        b.collision = shapeFromName(j["collision"], b.collision);
    }
    if (j.contains("mesh") && j["mesh"].is_string()) b.mesh = j["mesh"];
    b.size = vec3FromJson(j.value("size", nlohmann::json()), b.size);
    b.position = vec3FromJson(j.value("position", nlohmann::json()), b.position);
    b.rotation = vec3FromJson(j.value("rotation", nlohmann::json()), b.rotation);
    b.mass = j.value("mass", b.mass);
    b.fixed = j.value("fixed", b.fixed);
    if (j.contains("color") && j["color"].is_string()) {
        b.color = colorFromHex(j["color"], b.color);
    }
    return b;
}

inline nlohmann::json toJson(const JointDesc& jt) {
    nlohmann::json j;
    j["name"] = jt.name;
    j["kind"] = jointName(jt.kind);
    j["a"] = jt.bodyA;
    j["b"] = jt.bodyB;
    j["anchor"] = toJson(jt.anchor);
    j["axis"] = toJson(jt.axis);
    j["distance"] = jt.distance;
    return j;
}

inline JointDesc jointFromJson(const nlohmann::json& j, const JointDesc& base) {
    JointDesc jt = base;
    if (!j.is_object()) return jt;
    if (j.contains("name") && j["name"].is_string()) jt.name = j["name"];
    if (j.contains("kind") && j["kind"].is_string()) {
        jt.kind = jointFromName(j["kind"], jt.kind);
    }
    jt.bodyA = j.value("a", jt.bodyA);
    jt.bodyB = j.value("b", jt.bodyB);
    jt.anchor = vec3FromJson(j.value("anchor", nlohmann::json()), jt.anchor);
    jt.axis = vec3FromJson(j.value("axis", nlohmann::json()), jt.axis);
    jt.distance = j.value("distance", jt.distance);
    return jt;
}

inline nlohmann::json toJson(const LightDesc& l) {
    nlohmann::json j;
    j["name"] = l.name;
    j["kind"] = lightKindName(l.kind);
    j["position"] = toJson(l.position);
    j["rotation"] = toJson(l.rotation);
    j["color"] = colorToHex(l.color);
    j["intensity"] = l.intensity;
    j["falloff"] = l.falloff;
    j["spotInnerDeg"] = l.spotInnerDeg;
    j["spotOuterDeg"] = l.spotOuterDeg;
    j["shadows"] = l.shadows;
    return j;
}

inline LightDesc lightFromJson(const nlohmann::json& j, const LightDesc& base) {
    LightDesc l = base;
    if (!j.is_object()) return l;
    if (j.contains("name") && j["name"].is_string()) l.name = j["name"];
    if (j.contains("kind") && j["kind"].is_string()) {
        l.kind = lightKindFromName(j["kind"], l.kind);
    }
    l.position = vec3FromJson(j.value("position", nlohmann::json()), l.position);
    l.rotation = vec3FromJson(j.value("rotation", nlohmann::json()), l.rotation);
    if (j.contains("color") && j["color"].is_string()) {
        l.color = colorFromHex(j["color"], l.color);
    }
    l.intensity = j.value("intensity", l.intensity);
    l.falloff = j.value("falloff", l.falloff);
    l.spotInnerDeg = j.value("spotInnerDeg", l.spotInnerDeg);
    l.spotOuterDeg = j.value("spotOuterDeg", l.spotOuterDeg);
    l.shadows = j.value("shadows", l.shadows);
    return l;
}

inline nlohmann::json toJson(const CameraPose& c) {
    nlohmann::json j;
    j["azimuth"] = c.azimuth;
    j["elevation"] = c.elevation;
    j["radius"] = c.radius;
    j["target"] = toJson(c.target);
    j["active"] = c.active;
    return j;
}

inline CameraPose cameraPoseFromJson(const nlohmann::json& j,
                                     const CameraPose& base) {
    CameraPose c = base;
    if (!j.is_object()) return c;
    c.azimuth = j.value("azimuth", c.azimuth);
    c.elevation = j.value("elevation", c.elevation);
    c.radius = j.value("radius", c.radius);
    c.target = vec3FromJson(j.value("target", nlohmann::json()), c.target);
    c.active = j.value("active", c.active);
    return c;
}

inline nlohmann::json toJson(const NodeDesc& n) {
    nlohmann::json j;
    j["id"] = n.id;
    j["kind"] = nodeKindName(n.kind);
    j["x"] = n.x;
    j["y"] = n.y;
    j["target"] = n.target;
    j["other"] = n.other;
    j["seconds"] = n.seconds;
    j["color"] = colorToHex(n.color);
    j["vec"] = toJson(n.vec);
    j["value"] = n.value;
    return j;
}

inline NodeDesc nodeFromJson(const nlohmann::json& j, const NodeDesc& base) {
    NodeDesc n = base;
    if (!j.is_object()) return n;
    n.id = jsonInt(j, "id", n.id);
    if (j.contains("kind") && j["kind"].is_string()) {
        n.kind = nodeKindFromName(j["kind"], n.kind);
    }
    n.x = jsonNumber(j, "x", n.x);
    n.y = jsonNumber(j, "y", n.y);
    n.target = jsonInt(j, "target", n.target);
    n.other = jsonInt(j, "other", n.other);
    n.seconds = jsonNumber(j, "seconds", n.seconds);
    if (j.contains("color") && j["color"].is_string()) {
        n.color = colorFromHex(j["color"], n.color);
    }
    n.vec = vec3FromJson(j.value("vec", nlohmann::json()), n.vec);
    n.value = jsonNumber(j, "value", n.value);
    return n;
}

inline nlohmann::json toJson(const WireDesc& w) {
    return nlohmann::json{{"from", w.from}, {"to", w.to}};
}

inline WireDesc wireFromJson(const nlohmann::json& j, const WireDesc& base) {
    WireDesc w = base;
    if (!j.is_object()) return w;
    w.from = jsonInt(j, "from", w.from);
    w.to = jsonInt(j, "to", w.to);
    return w;
}

// 文書に書いてよいファイル参照（<mesh file> / <ground texture> /
// <environment hdr>）は assets/ からの相対パスだけ。".." と絶対パス
// （/ 始まり・ドライブレター）を弾いて、読み込み先を assets/ の下に
// 閉じ込める（保存名の正規化と同じ動機。文書もブラウザの入力欄も
// 手で書けるので、XML の読み込みと edit.* の両方がこれを通す）。
inline bool assetFileAllowed(const std::string& file) {
    if (file.empty()) return false;
    if (file[0] == '/' || file[0] == '\\') return false;
    if (file.size() > 1 && file[1] == ':') return false;  // C:\ ...
    if (file.find("..") != std::string::npos) return false;
    return true;
}

// 地面と環境光。キーは XML の属性名と同じ（size = 物理の半寸法、visual =
// 見える地面の半寸法）。ブラウザの World 節（Inspector）とやり取りする。
inline nlohmann::json toJson(const GroundDesc& g) {
    nlohmann::json j;
    j["size"] = g.half;
    j["visual"] = g.visualHalf;
    j["texture"] = g.texture;
    j["tile"] = g.tile;
    j["color"] = colorToHex(g.tint);
    return j;
}

inline GroundDesc groundFromJson(const nlohmann::json& j,
                                 const GroundDesc& base) {
    GroundDesc g = base;
    if (!j.is_object()) return g;
    g.half = jsonNumber(j, "size", g.half);
    g.visualHalf = jsonNumber(j, "visual", g.visualHalf);
    if (j.contains("texture") && j["texture"].is_string()) {
        // 空 = 市松模様。パスとして不正なもの（".." 等）は無視して現状維持。
        const std::string t = j["texture"];
        if (t.empty() || assetFileAllowed(t)) g.texture = t;
    }
    g.tile = jsonNumber(j, "tile", g.tile);
    if (j.contains("color") && j["color"].is_string()) {
        g.tint = colorFromHex(j["color"], g.tint);
    }
    return g;
}

inline nlohmann::json toJson(const EnvironmentDesc& e) {
    nlohmann::json j;
    j["hdr"] = e.hdr;
    j["intensity"] = e.intensity;
    return j;
}

inline EnvironmentDesc environmentFromJson(const nlohmann::json& j,
                                           const EnvironmentDesc& base) {
    EnvironmentDesc e = base;
    if (!j.is_object()) return e;
    if (j.contains("hdr") && j["hdr"].is_string()) {
        // 空 = 環境マップ無し。不正なパスは無視して現状維持。
        const std::string h = j["hdr"];
        if (h.empty() || assetFileAllowed(h)) e.hdr = h;
    }
    e.intensity = jsonNumber(j, "intensity", e.intensity);
    return e;
}

inline nlohmann::json toJson(const SimSettings& s) {
    nlohmann::json j;
    j["gravity"] = s.gravity;
    j["hz"] = s.hz;
    j["substeps"] = s.substeps;
    j["iterations"] = s.iterations;
    j["envelope"] = s.envelope;
    j["recovery"] = s.recovery;
    j["friction"] = s.friction;
    j["restitution"] = s.restitution;
    j["linearDamping"] = s.linearDamping;
    j["angularDamping"] = s.angularDamping;
    j["sleeping"] = s.sleeping;
    return j;
}

inline SimSettings simFromJson(const nlohmann::json& j, const SimSettings& base) {
    SimSettings s = base;
    if (!j.is_object()) return s;
    s.gravity = j.value("gravity", s.gravity);
    s.hz = j.value("hz", s.hz);
    s.substeps = j.value("substeps", s.substeps);
    s.iterations = j.value("iterations", s.iterations);
    s.envelope = j.value("envelope", s.envelope);
    s.recovery = j.value("recovery", s.recovery);
    s.friction = j.value("friction", s.friction);
    s.restitution = j.value("restitution", s.restitution);
    s.linearDamping = j.value("linearDamping", s.linearDamping);
    s.angularDamping = j.value("angularDamping", s.angularDamping);
    s.sleeping = j.value("sleeping", s.sleeping);
    return s;
}

inline nlohmann::json toJson(const GizmoSettings& g) {
    nlohmann::json j;
    j["mode"] = gizmoModeName(g.mode);
    j["space"] = gizmoSpaceName(g.space);
    j["snap"] = g.snap;
    j["moveStep"] = g.moveStep;
    j["rotateStep"] = g.rotateStep;
    j["scaleStep"] = g.scaleStep;
    j["grid"] = g.grid;
    j["gridStep"] = g.gridStep;
    return j;
}

inline GizmoSettings gizmoFromJson(const nlohmann::json& j,
                                   const GizmoSettings& base) {
    GizmoSettings g = base;
    if (!j.is_object()) return g;
    if (j.contains("mode") && j["mode"].is_string()) {
        g.mode = gizmoModeFromName(j["mode"], g.mode);
    }
    if (j.contains("space") && j["space"].is_string()) {
        g.space = gizmoSpaceFromName(j["space"], g.space);
    }
    g.snap = j.value("snap", g.snap);
    g.moveStep = j.value("moveStep", g.moveStep);
    g.rotateStep = j.value("rotateStep", g.rotateStep);
    g.scaleStep = j.value("scaleStep", g.scaleStep);
    g.grid = j.value("grid", g.grid);
    g.gridStep = j.value("gridStep", g.gridStep);
    // 刻みが 0 だとスナップの割り算が壊れる。
    if (g.moveStep < 1e-4) g.moveStep = 0.25;
    if (g.rotateStep < 1e-4) g.rotateStep = 15.0;
    if (g.scaleStep < 1e-4) g.scaleStep = 0.1;
    // グリッドの間隔。下限は使い勝手（100m 幅で 0.25m だと 800 本 =
    // これ以上細かくしてもモアレで見えない）。
    if (g.gridStep < 0.25) g.gridStep = 0.25;
    if (g.gridStep > 10.0) g.gridStep = 10.0;
    return g;
}

// UI から来た値の常識的な範囲。手書きリクエスト対策のクランプであって、
// チューニングの推奨値ではない。
inline SimSettings clampSim(SimSettings s) {
    auto cl = [](auto v, auto lo, auto hi) { return v < lo ? lo : (v > hi ? hi : v); };
    s.gravity = cl(s.gravity, -100.0, 100.0);
    s.hz = cl(s.hz, 10, 240);
    s.substeps = cl(s.substeps, 1, 8);
    s.iterations = cl(s.iterations, 1, 2000);
    s.envelope = cl(s.envelope, 1e-4, 0.2);
    s.recovery = cl(s.recovery, 0.0, 10.0);
    s.friction = cl(s.friction, 0.0f, 2.0f);
    s.restitution = cl(s.restitution, 0.0f, 1.0f);
    s.linearDamping = cl(s.linearDamping, 0.0, 10.0);
    s.angularDamping = cl(s.angularDamping, 0.0, 10.0);
    return s;
}

// 置ける大きさ・質量の範囲。0 やマイナスは Chrono を壊すのでここで止める。
inline BodyDesc clampBody(BodyDesc b) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    b.size.x = cl(b.size.x, 0.01, 50.0);
    b.size.y = cl(b.size.y, 0.01, 50.0);
    b.size.z = cl(b.size.z, 0.01, 50.0);
    b.mass = cl(b.mass, 0.001, 100000.0);
    b.position.x = cl(b.position.x, -500.0, 500.0);
    b.position.y = cl(b.position.y, -500.0, 500.0);
    b.position.z = cl(b.position.z, -500.0, 500.0);
    return b;
}

// ライトの常識的な範囲。0 やマイナスの強さ・範囲は Filament 側で意味を持たない。
inline LightDesc clampLight(LightDesc l) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    l.position.x = cl(l.position.x, -500.0, 500.0);
    l.position.y = cl(l.position.y, -500.0, 500.0);
    l.position.z = cl(l.position.z, -500.0, 500.0);
    l.intensity = cl(l.intensity, 0.0, 10000000.0);
    l.falloff = cl(l.falloff, 0.1, 500.0);
    l.spotInnerDeg = cl(l.spotInnerDeg, 1.0, 88.0);
    l.spotOuterDeg = cl(l.spotOuterDeg, l.spotInnerDeg, 89.0);
    return l;
}

// 地面・環境光の常識的な範囲。0 や負の広さは描画も物理も壊す。
inline GroundDesc clampGround(GroundDesc g) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    g.half = cl(g.half, 1.0, 1000.0);
    g.visualHalf = cl(g.visualHalf, 1.0, 1000.0);
    g.tile = cl(g.tile, 0.1, 100.0);
    return g;
}
inline EnvironmentDesc clampEnvironment(EnvironmentDesc e) {
    if (e.intensity < 0.0) e.intensity = 0.0;
    if (e.intensity > 10000000.0) e.intensity = 10000000.0;
    return e;
}

// ノードの常識的な範囲。タイマーの下限が一番大事: 0 に近いと毎ステップ
// 発火して、繋いだアクション（力・色）が暴走する。
inline NodeDesc clampNode(NodeDesc n) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    n.x = cl(n.x, 0.0, 4000.0);
    n.y = cl(n.y, 0.0, 4000.0);
    if (n.target < -1) n.target = -1;
    if (n.other < -2) n.other = -2;
    // -2（何でも）は OnCollision の相手フィルタだけの語彙。CameraLookAt の
    // other は注視先のオブジェクト番号なので、-1（未設定）が下限。
    if (n.kind == NodeKind::CameraLookAt && n.other < -1) n.other = -1;
    n.seconds = cl(n.seconds, 0.05, 3600.0);
    n.vec.x = cl(n.vec.x, -100.0, 100.0);
    n.vec.y = cl(n.vec.y, -100.0, 100.0);
    n.vec.z = cl(n.vec.z, -100.0, 100.0);
    // value の意味は種類ごと: SetFixed は 0/1、SetLightIntensity はルーメン。
    n.value = (n.kind == NodeKind::SetFixed) ? (n.value != 0.0 ? 1.0 : 0.0)
                                             : cl(n.value, 0.0, 10000000.0);
    return n;
}

}  // namespace editor
}  // namespace wizengine
