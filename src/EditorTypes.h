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

// 剛体の形。Model は scene.cpp の kBoxModelPath で作った glTF インスタンス群
// （数が固定のプール）を指す。エディタで新規に置けるのは Box と Sphere。
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

// 1個の剛体の設計値。position/rotation は「エディタで置いた姿勢」で、
// シミュレートを止めるとここに戻る（＝オーサリング状態は壊れない）。
struct BodyDesc {
    std::string name;
    ShapeKind shape = ShapeKind::Box;
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

inline nlohmann::json toJson(const Vec3d& v) {
    return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
}
inline Vec3d vec3FromJson(const nlohmann::json& j, const Vec3d& fallback) {
    if (!j.is_object()) return fallback;
    Vec3d v;
    v.x = j.value("x", fallback.x);
    v.y = j.value("y", fallback.y);
    v.z = j.value("z", fallback.z);
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

inline nlohmann::json toJson(const BodyDesc& b) {
    nlohmann::json j;
    j["name"] = b.name;
    j["shape"] = shapeName(b.shape);
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

}  // namespace editor
}  // namespace wizengine
