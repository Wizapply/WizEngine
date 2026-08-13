#include "Scene.h"
#include "SceneConfig.h"
#include "MeshCollision.h"
#include "Log.h"

#include "scene_math.h"

#include <chrono/core/ChQuaternion.h>
#include <chrono/core/ChVector3.h>

#include <math/mat4.h>
#include <math/quat.h>
#include <math/vec3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <random>

#include "AssetError.h"
#include "GizmoComponent.h"
#include "PhysicsWorld.h"
#include "Renderer.h"
#include "math_bridge.h"

using namespace chrono;
namespace ed = wizengine::editor;

namespace {

// Scene parameters (grid, cameras, lights, physics, ...) live in
// SceneConfig.h; below are only derived helpers and the implementation.

std::size_t boxTotal() { return static_cast<std::size_t>(kNx * kNy * kNz); }

// エディタの回転（オイラー角・度）を Chrono の四元数へ。順序の定義は
// scene_math.h に 1 か所だけ置いてある（インスペクタの数字・ギズモの回転・
// 物理に渡す姿勢が食い違わないように）。
ChQuaternion<> quatFromEuler(const ed::Vec3d& degrees) {
    const scenemath::Quat q =
        scenemath::quatFromEulerDegrees(degrees.x, degrees.y, degrees.z);
    return ChQuaternion<>(q.w(), q.x(), q.y(), q.z());
}

// 逆変換。格子の初期傾きのように四元数で作った姿勢を、設計値（オイラー角）
// として持ち直すために使う。
ed::Vec3d eulerFromQuat(const ChQuaternion<>& q) {
    const scenemath::Vec3 e = scenemath::eulerDegreesFromQuat(
        scenemath::Quat(q.e0(), q.e1(), q.e2(), q.e3()));
    return {e.x(), e.y(), e.z()};
}

// エディタの JointKind を PhysicsWorld の enum へ。
JointType toJointType(ed::JointKind k) {
    switch (k) {
        case ed::JointKind::Fixed: return JointType::Fixed;
        case ed::JointKind::Spherical: return JointType::Spherical;
        case ed::JointKind::Prismatic: return JointType::Prismatic;
        case ed::JointKind::Distance: return JointType::Distance;
        case ed::JointKind::Revolute: break;
    }
    return JointType::Revolute;
}

// ジョイントの種類ごとの線の色（リニア RGB）。ビューを見ただけで何の拘束か
// 分かるようにしておく。
filament::math::float3 jointColor(ed::JointKind k) {
    switch (k) {
        case ed::JointKind::Fixed: return {0.95f, 0.85f, 0.20f};      // 黄
        case ed::JointKind::Spherical: return {0.95f, 0.45f, 0.85f};  // 桃
        case ed::JointKind::Prismatic: return {0.35f, 0.90f, 0.90f};  // 水
        case ed::JointKind::Distance: return {0.60f, 0.95f, 0.40f};   // 黄緑
        case ed::JointKind::Revolute: break;
    }
    return {1.0f, 0.55f, 0.15f};  // 橙
}

ChVector3d gridPos(std::size_t k) {
    const int nyz = kNy * kNz;
    const int i = static_cast<int>(k) / nyz;
    const int rem = static_cast<int>(k) % nyz;
    const int j = rem / kNz;
    const int l = rem % kNz;
    return ChVector3d((i - (kNx - 1) / 2.0) * kSpacing, kBaseY + j * kSpacing,
                      (l - (kNz - 1) / 2.0) * kSpacing);
}

ChQuaternion<> dropTilt(std::size_t k) {
    const double a = 0.08 + 0.02 * static_cast<double>(k % 5);
    return QuatFromAngleAxis(a, ChVector3d(1, 0.3, 1).GetNormalized());
}

ChVector3d jitter() {
    static std::mt19937 rng(1234);
    std::uniform_real_distribution<double> d(-0.02, 0.02);
    return ChVector3d(d(rng), d(rng), d(rng));
}

// ---- ライト -----------------------------------------------------------------
constexpr double kRadToDeg = 180.0 / scenemath::kPi;
constexpr double kDegToRad = scenemath::kPi / 180.0;

// SceneConfig.h の LightObject::Config（方向ベクトルで書く従来の設定）を
// エディタの設計値（向きはオイラー角）へ。初期ライトの取り込みに使う。
ed::LightDesc lightDescFromConfig(const LightObject::Config& c) {
    ed::LightDesc d;
    switch (c.type) {
        case LightObject::Type::Directional: d.kind = ed::LightKind::Sun; break;
        case LightObject::Type::Spot: d.kind = ed::LightKind::Spot; break;
        case LightObject::Type::Point: d.kind = ed::LightKind::Point; break;
    }
    d.color = {c.color.x, c.color.y, c.color.z};
    d.intensity = c.intensity;
    d.position = {c.position.x, c.position.y, c.position.z};
    const scenemath::Vec3 e = scenemath::eulerDegreesFromDirection(
        scenemath::Vec3(c.direction.x, c.direction.y, c.direction.z));
    d.rotation = {e.x(), e.y(), e.z()};
    d.falloff = c.falloffRadius;
    d.spotInnerDeg = c.spotInnerRadians * kRadToDeg;
    d.spotOuterDeg = c.spotOuterRadians * kRadToDeg;
    d.shadows = c.castShadows;
    return d;
}

// エディタの設計値をレンダラの語彙へ（向きはここでベクトルに戻す）。
wizengine::LightDesc toRendererLight(const ed::LightDesc& d) {
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
double cameraPitchDeg(const CameraObject& c) {
    return -c.elevation() * kRadToDeg;
}
double cameraYawDeg(const CameraObject& c) {
    double yaw = c.azimuth() * kRadToDeg + 180.0;
    while (yaw > 180.0) yaw -= 360.0;
    while (yaw < -180.0) yaw += 360.0;
    return yaw;
}
// 方位角・仰角から「注視点 → 視点」の単位ベクトル（eye = target + radius*v）。
scenemath::Vec3 orbitVector(double azimuthRad, double elevationRad) {
    return scenemath::Vec3(std::cos(elevationRad) * std::sin(azimuthRad),
                           std::sin(elevationRad),
                           std::cos(elevationRad) * std::cos(azimuthRad));
}

}  // namespace

namespace {

// Routes camera commands from a browser page to that page's CameraObject.
class CameraControlComponent : public SceneComponent {
public:
    bool onCommand(Scene& scene, std::size_t cam,
                   const nlohmann::json& msg) override {
        const std::string cmd = msg.value("cmd", "");
        if (cmd == "camera") {  // arrow keys: fixed-size orbit steps
            scene.camera(cam).stepOrbit(msg.value("yaw", 0.0),
                                        msg.value("pitch", 0.0));
        } else if (cmd == "orbit") {
            scene.camera(cam).orbit(msg.value("dx", 0.0), msg.value("dy", 0.0));
        } else if (cmd == "pan") {
            scene.camera(cam).pan(msg.value("dx", 0.0), msg.value("dy", 0.0));
        } else if (cmd == "zoom") {
            scene.camera(cam).zoom(msg.value("d", 0.0));
        } else if (cmd == "pick") {
            const std::size_t hit =
                scene.pickBoxAt(msg.value("x", 0.0), msg.value("y", 0.0), cam);
            LOGD("scene", "pick (camera %zu): %s", cam,
                 hit == BoxController::kNone
                     ? "(none)"
                     : ("object " + std::to_string(hit)).c_str());
        } else {
            return false;
        }
        return true;
    }
};

// Applies each camera's mouse drag to its grabbed object and keeps the
// per-camera coloured highlight in sync.
class BoxControlComponent : public SceneComponent {
public:
    void onPhysicsStep(Scene& scene, double dt) override {
        prepare(scene);
        for (std::size_t c = 0; c < scene.cameraCount(); ++c) {
            const Grab g = grabFor(scene, c);
            if (!g.valid) continue;

            // Servo towards the target: F = m * (kp*e - kd*v), acceleration
            // capped. Applied every step, which is what makes it track - a
            // one-shot impulse dies to friction immediately.
            const auto& cfg = scene.boxController(c).config();
            const chrono::ChVector3d cv = scene.physics().bodyVelocity(g.physId);
            const scenemath::Vec3 vel(cv.x(), cv.y(), cv.z());
            scenemath::Vec3 accel =
                cfg.stiffness * (g.target - g.objPos) - cfg.damping * vel;
            const double a = accel.norm();
            if (a > cfg.maxAcceleration) accel *= cfg.maxAcceleration / a;

            const double mass = scene.physics().bodyMass(g.physId);
            if (mass <= 0.0) continue;
            const scenemath::Vec3 force = accel * mass;
            scene.physics().applyForce(
                g.physId, chrono::ChVector3d(force.x(), force.y(), force.z()),
                dt);

            // Line endpoints for the render thread: object -> cursor point.
            LineState& ln = *lines_[c];
            ln.ax.store(g.objPos.x());
            ln.ay.store(g.objPos.y());
            ln.az.store(g.objPos.z());
            ln.bx.store(g.target.x());
            ln.by.store(g.target.y());
            ln.bz.store(g.target.z());
            ln.on.store(true);
        }
    }

    // エディタモードでのドラッグ: 力で引っぱるのではなく、置いた場所そのものを
    // 書き換える。掴んだ物がカーソルに正確に付いてくるので、配置作業がしやすい。
    // 置き直せるのはエディタカメラだけ - 他のカメラは見る・選ぶまで。
    void onEditorStep(Scene& scene, double dt) override {
        (void)dt;
        prepare(scene);
        const std::size_t editorCam = scene.editorCamera();
        for (std::size_t c = 0; c < scene.cameraCount(); ++c) {
            if (c != editorCam) {
                lines_[c]->on.store(false);
                continue;
            }
            const Grab g = grabFor(scene, c);
            if (!g.valid) continue;
            scene.moveObject(g.index, g.target.x(), g.target.y(),
                             g.target.z());
            // 物がカーソル上にあるので、引っぱり線は出さない。
            lines_[c]->on.store(false);
        }
    }

    bool onCommand(Scene& scene, std::size_t cam,
                   const nlohmann::json& msg) override {
        const std::string cmd = msg.value("cmd", "");
        if (cmd == "drag") {
            const double x = msg.value("x", 0.0);
            const double y = msg.value("y", 0.0);
            BoxController& ctl = scene.boxController(cam);
            if (ctl.selected() != BoxController::kNone) {
                ctl.setPointer(x, y);  // holding something: pull it
            } else if (msg.value("touch", false) &&
                       lastPointer_.size() > cam && lastPointer_[cam].valid) {
                // Touch only: a finger that grabbed nothing orbits instead.
                // The client cannot know whether the press hit an object
                // without waiting for a round trip, which would swallow the
                // start of the gesture, so the decision is made here.
                // With a mouse there is no such problem - Ctrl+drag is the
                // camera and a plain drag that hits nothing does nothing.
                scene.camera(cam).orbit(
                    -(x - lastPointer_[cam].x) * kOrbitRadPerNdc,
                    -(y - lastPointer_[cam].y) * kOrbitRadPerNdc);
            }
            if (lastPointer_.size() <= cam) lastPointer_.resize(cam + 1);
            lastPointer_[cam] = {true, x, y};
            return true;
        }
        if (cmd == "release") {
            // エディタでは（エディタカメラに限り）掴んだままにしておく。離した
            // 瞬間に選択が外れると、置いた直後にインスペクタで数値を詰める、
            // という流れができない。シミュレート中と他のカメラは今までどおり。
            const bool keepSelection =
                scene.mode() == wizengine::editor::AppMode::Editor &&
                cam == scene.editorCamera();
            if (!keepSelection) {
                scene.boxController(cam).setSelected(BoxController::kNone);
            }
            scene.boxController(cam).clearPointer();
            if (lastPointer_.size() > cam) lastPointer_[cam].valid = false;
            return true;
        }
        if (cmd == "select") {  // clicked in the sidebar hierarchy
            const int idx = msg.value("index", -1);
            // INPUT スレッドからオブジェクト一覧を見るのでロックする。
            auto lk = scene.lockObjects();
            const bool ok = idx >= 0 && std::size_t(idx) < scene.objectCount() &&
                            scene.objectAlive(std::size_t(idx));
            scene.boxController(cam).setSelected(ok ? std::size_t(idx)
                                                    : BoxController::kNone);
            // オブジェクトを選んだら、ライト / カメラのエディタ選択は外す
            // （両方が同時に立つとギズモの対象が曖昧になる）。
            if (cam == scene.editorCamera()) scene.editor().clearSel();
            return true;
        }
        return false;
    }

    void onRender(Scene& scene) override {
        // Grab lines are real geometry in the scene, so they follow the object
        // in 3D and are visible from every camera.
        for (std::size_t c = 0; c < lines_.size(); ++c) {
            const LineState& ln = *lines_[c];
            const bool on = ln.on.load();
            scene.renderer().setGrabLine(
                c,
                {float(ln.ax.load()), float(ln.ay.load()), float(ln.az.load())},
                {float(ln.bx.load()), float(ln.by.load()), float(ln.bz.load())},
                on);
        }

        // Selections are per camera, so several highlights can be lit at once;
        // the renderer is only touched when a selection changes.
        if (last_.size() != scene.cameraCount()) {
            last_.assign(scene.cameraCount(), BoxController::kNone);
        }
        for (std::size_t c = 0; c < scene.cameraCount(); ++c) {
            const std::size_t sel = scene.boxController(c).selected();
            if (sel == last_[c]) continue;
            setMark(scene, last_[c], c, false);
            setMark(scene, sel, c, true);
            last_[c] = sel;
        }
    }

private:
    // 掴んでいる対象と、カーソルが指す3D点。押す（シミュレート）のと
    // 置き直す（エディタ）のとで違うのは最後の一手だけなので、そこまでの
    // 計算はここにまとめてある。
    struct Grab {
        bool valid = false;
        std::size_t index = BoxController::kNone;  // オブジェクト番号
        std::size_t physId = 0;
        scenemath::Vec3 objPos;
        scenemath::Vec3 target;
    };

    void prepare(Scene& scene) {
        if (depths_.size() == scene.cameraCount()) return;
        depths_.assign(scene.cameraCount(), Depth{});
        lines_.clear();
        for (std::size_t k = 0; k < scene.cameraCount(); ++k) {
            lines_.push_back(std::make_unique<LineState>());
        }
    }

    Grab grabFor(Scene& scene, std::size_t c) {
        Grab g;
        BoxController& ctl = scene.boxController(c);
        const std::size_t sel = ctl.selected();
        Depth& depth = depths_[c];
        if (sel >= scene.objectCount() || !scene.objectAlive(sel)) {
            depth.valid = false;
            lines_[c]->on.store(false);
            return g;
        }

        double ndcX = 0.0, ndcY = 0.0;
        if (!ctl.pointer(ndcX, ndcY)) {
            // 掴んでいない = 次に掴んだときは奥行きを測り直す。エディタでは
            // 離しても選択が残るので、これが無いと前回のカメラ向きで測った
            // 奥行きを使い回してしまう。
            depth.held = false;
            lines_[c]->on.store(false);
            return g;
        }

        const std::size_t physId = scene.object(sel).physId;
        const BodyTransform tr = scene.physics().bodyTransform(physId);

        // Camera basis and the point under the cursor, both from the
        // shared Eigen helpers so picking and dragging cannot drift apart.
        const auto basis = scenemath::cameraBasis(scene.camera(c));
        if (!basis.valid) return g;

        // Depth of the object when it was grabbed: the target rides on the
        // plane at that depth, so the object stays under the cursor
        // without being pulled towards or away from the camera.
        const scenemath::Vec3 objPos(tr.px, tr.py, tr.pz);
        if (!depth.valid || depth.sel != sel || !depth.held) {
            depth.valid = true;
            depth.held = true;
            depth.sel = sel;
            depth.z = std::max(0.1, (objPos - basis.eye).dot(basis.forward));
        }

        // Ray through the cursor, hit against that plane. Absolute, so the
        // target is exactly under the cursor every step - no drift.
        const scenemath::Vec3 dir = scenemath::rayThrough(
            basis, ndcX, ndcY, scene.renderer().verticalFovDegrees(),
            scene.renderer().aspect());
        const double along = dir.dot(basis.forward);
        if (along <= 1e-6) return g;

        g.valid = true;
        g.index = sel;
        g.physId = physId;
        g.objPos = objPos;
        g.target = basis.eye + dir * (depth.z / along);
        return g;
    }

    static void setMark(Scene& scene, std::size_t index, std::size_t cam,
                        bool on) {
        if (index >= scene.objectCount()) return;
        const GameObject& obj = scene.object(index);
        if (obj.renderId == GameObject::kInvalidId) return;
        // 描き方はオブジェクトごと: エディタで置いた箱・球は組み込みメッシュ、
        // シーン既定の物は glTF インスタンス。
        if (obj.modelDraw) {
            scene.renderer().setModelInstanceTint(
                obj.renderId, scene.cameraColor(cam),
                on ? scene.selectedWhiten() : 0.0f);
        } else {
            scene.renderer().setBoxHighlighted(obj.renderId, on ? int(cam) : -1);
        }
    }

    std::vector<std::size_t> last_;  // render thread only

    // Previous pointer position per camera (input thread), so a drag that
    // grabbed nothing can be turned into an orbit.
    struct LastPointer {
        bool valid = false;
        double x = 0.0, y = 0.0;
    };
    std::vector<LastPointer> lastPointer_;
    // Radians of orbit per unit of normalised device coords. The screen spans
    // 2 units, so this is roughly "half a screen drag = this many radians".
    static constexpr double kOrbitRadPerNdc = 1.6;

    // Distance from the camera to the object at grab time (physics thread).
    // held は「前のパスでも掴んでいたか」。掴み直しのたびに奥行きを測り直す
    // ための印で、選択が残ったままカメラを回しても破綻しない。
    struct Depth {
        bool valid = false;
        bool held = false;
        std::size_t sel = BoxController::kNone;
        double z = 0.0;
    };
    std::vector<Depth> depths_;

    // Line endpoints handed from the physics thread to the render thread: the
    // object and the point it is being pulled towards.
    struct LineState {
        std::atomic<bool> on{false};
        std::atomic<double> ax{0.0}, ay{0.0}, az{0.0};  // object
        std::atomic<double> bx{0.0}, by{0.0}, bz{0.0};  // target
    };
    std::vector<std::unique_ptr<LineState>> lines_;
};

// --- Example ObjectAction ---------------------------------------------------
// Shoves its object upward for a moment every few seconds. Attach in
// Scene::build() to see per-object behaviour working:
//     boxes_[0].actions.push_back(std::make_unique<PulseUpAction>());
class PulseUpAction : public ObjectAction {
public:
    void onPhysicsStep(GameObject& object, PhysicsWorld& physics,
                       double dt) override {
        clock_ += dt;
        if (clock_ >= kPeriod) clock_ -= kPeriod;
        if (clock_ < kBurst) {
            const double f = physics.bodyMass(object.physId) * 30.0;  // ~3 g
            physics.applyForce(object.physId, chrono::ChVector3d(0, f, 0), dt);
        }
    }

private:
    static constexpr double kPeriod = 3.0;  // seconds between pulses
    static constexpr double kBurst = 0.15;  // how long each shove lasts
    double clock_ = 0.0;
};

}  // namespace

float Scene::selectedWhiten() const {
    return kSelectedWhiten;
}

std::string Scene::hierarchyJson(std::size_t cameraIndex) {
    auto hex = [](const filament::math::float3& c) {
        auto ch = [](float v) {
            const int i = int(v * 255.0f + 0.5f);
            return i < 0 ? 0 : (i > 255 ? 255 : i);
        };
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", ch(c.x), ch(c.y),
                      ch(c.z));
        return std::string(buf);
    };

    nlohmann::json j;
    j["camera"] = int(cameraIndex);
    // 形はオブジェクトごとに持つようになったので、ここに出すのはシーン共通の
    // モデル名だけ（インスペクタが「Model (apple2.glb)」と出すのに使う）。
    j["model"] = useModel_ ? kBoxModelPath : "";

    // ここからライト・カメラ有効フラグ・オブジェクト一覧を読むのでロックする
    // （物理スレッドが追加・削除している最中かもしれない）。ロック順は
    // objects → editor → poses の一方向（CLAUDE.md の規約）。
    std::unique_lock<std::mutex> lk(objectsMutex_);

    // Cameras, with what each one currently holds. 無効（削除済み）スロットも
    // active=false で送る: ブラウザ側が一覧から隠す判断に使う。
    for (std::size_t c = 0; c < cameras_.size(); ++c) {
        nlohmann::json e;
        e["index"] = int(c);
        e["color"] = hex(cameraColor(c));
        e["active"] = camerasActive_[c] != 0;
        const std::size_t sel = controllers_[c]->selected();
        e["selected"] = (sel == BoxController::kNone) ? -1 : int(sel);
        j["cameras"].push_back(e);
    }

    // Lights。エディタで選ぶ・編集するので、設計値を番号付きで送る。
    const auto selKind = editor_.selKind();
    const int selIndex = editor_.selIndex();
    j["lights"] = nlohmann::json::array();
    for (std::size_t i = 0; i < lights_.size(); ++i) {
        if (!lights_[i].alive) continue;  // 削除済みは出さない（番号は空く）
        nlohmann::json e = ed::toJson(lights_[i].desc);
        e["index"] = int(i);
        j["lights"].push_back(e);
    }

    // ---- エディタ選択（ライト / カメラ）----------------------------------
    // オブジェクトの選択（下の "selected"）とは別枠。UI はこちらが立っていれば
    // ライト / カメラのインスペクタを出す。
    {
        nlohmann::json s;
        s["kind"] = selKind == EditorState::SelKind::Light    ? "light"
                    : selKind == EditorState::SelKind::Camera ? "camera"
                                                              : "none";
        s["index"] = selIndex;
        j["editorSel"] = s;
        if (selKind == EditorState::SelKind::Light && selIndex >= 0 &&
            std::size_t(selIndex) < lights_.size() &&
            lights_[std::size_t(selIndex)].alive) {
            nlohmann::json d = ed::toJson(lights_[std::size_t(selIndex)].desc);
            d["index"] = selIndex;
            j["selectedLight"] = d;
        }
        if (selKind == EditorState::SelKind::Camera && selIndex >= 0 &&
            std::size_t(selIndex) < cameras_.size() &&
            camerasActive_[std::size_t(selIndex)] != 0) {
            ed::Vec3d pos, rot;
            cameraEditPose(std::size_t(selIndex), pos, rot);
            nlohmann::json d;
            d["index"] = selIndex;
            d["position"] = ed::toJson(pos);
            d["rotation"] = ed::toJson(rot);
            d["radius"] = cameras_[std::size_t(selIndex)]->radius();
            j["selectedCamera"] = d;
        }
    }

    // Objects. Positions come from the shared pose snapshot, so this never
    // touches the physics world from the HTTP thread.
    std::vector<BodyTransform> poses;
    {
        std::lock_guard<std::mutex> pl(poseMutex_);
        poses = latestPoses_;
    }

    // ---- エディタの状態 --------------------------------------------------
    // モード・ジョイント・設定・保存ファイル一覧。ブラウザはこれ 1 本で
    // サイドバー全体を描ける。
    j["mode"] = ed::modeName(editor_.mode());
    j["editorCam"] = int(editorCamera());
    j["sim"] = ed::toJson(editor_.sim());
    j["gizmo"] = ed::toJson(editor_.gizmo());
    j["status"] = editor_.status();
    j["sceneFile"] = editor_.sceneFile();
    for (const auto& f : editor_.sceneFiles()) j["files"].push_back(f);
    j["objects"] = nlohmann::json::array();
    j["joints"] = nlohmann::json::array();
    {
        const auto joints = editor_.joints();
        for (std::size_t i = 0; i < joints.size(); ++i) {
            nlohmann::json e = ed::toJson(joints[i]);
            e["index"] = int(i);
            j["joints"].push_back(e);
        }
    }

    // Who (if anyone) is holding each object, so the list can colour it.
    std::vector<int> heldBy(boxes_.size(), -1);
    for (std::size_t c = 0; c < controllers_.size(); ++c) {
        const std::size_t sel = controllers_[c]->selected();
        if (sel < heldBy.size()) heldBy[sel] = int(c);
    }
    int alive = 0;
    for (std::size_t i = 0; i < boxes_.size() && i < poses.size(); ++i) {
        if (!boxes_[i].alive) continue;  // 削除済みは出さない（番号は空く）
        ++alive;
        nlohmann::json e;
        e["index"] = int(i);
        e["x"] = poses[i].px;
        e["y"] = poses[i].py;
        e["z"] = poses[i].pz;
        e["heldBy"] = heldBy[i];
        e["shape"] = ed::shapeName(boxes_[i].desc.shape);
        if (boxes_[i].desc.fixed) e["fixed"] = true;
        // 名前は付けた物だけ送る（512個ぶんの既定名を毎回運ぶのは無駄）。
        if (!boxes_[i].desc.name.empty()) e["name"] = boxes_[i].desc.name;
        j["objects"].push_back(e);
    }
    j["aliveCount"] = alive;

    // インスペクタ用に、このカメラが選んでいる物だけ設計値を丸ごと送る。
    const std::size_t sel = controllers_[cameraIndex]->selected();
    if (sel < boxes_.size() && boxes_[sel].alive) {
        nlohmann::json d = ed::toJson(boxes_[sel].desc);
        d["index"] = int(sel);
        if (sel < poses.size()) {
            d["px"] = poses[sel].px;
            d["py"] = poses[sel].py;
            d["pz"] = poses[sel].pz;
        }
        j["selected"] = d;
    }
    return j.dump();
}

filament::math::float3 Scene::cameraColor(std::size_t cameraIndex) const {
    const auto& colors = cameraColors();
    return colors[cameraIndex % colors.size()];
}

void Scene::addComponent(std::unique_ptr<SceneComponent> component) {
    components_.push_back(std::move(component));
}

bool Scene::dispatchCommand(std::size_t camIndex, const nlohmann::json& msg) {
    for (auto& c : components_) {
        if (c->onCommand(*this, camIndex, msg)) return true;
    }
    return false;
}

Scene::Scene(PhysicsWorld& physics, wizengine::Renderer& renderer,
             std::size_t maxCameras)
    : physics_(physics), renderer_(renderer) {
    // スロット数: exe 引数（--max-cameras、0 = 未指定）が優先、無ければ
    // SceneConfig.h の kMaxCameras。カメラ 0（エディタカメラ）は必ず要るので
    // 1 未満にはしない。極端な値はページ・受け口・スレッドを無駄に増やす
    // だけなので 16 で止める。
    std::size_t slots = maxCameras > 0 ? maxCameras : kMaxCameras;
    if (slots < 1) slots = 1;
    if (slots > 16) {
        LOGW("scene", "max cameras %zu is excessive - clamped to 16", slots);
        slots = 16;
    }
    // cameraConfigs() のぶんが最初から有効で（スロット数を超えるぶんは
    // 落とす）、残りは「カメラを追加」で有効になるまで一覧に出ない予備。
    const auto cfgs = cameraConfigs();
    for (std::size_t i = 0; i < slots; ++i) {
        const CameraObject::Config cfg =
            (i < cfgs.size()) ? cfgs[i] : cfgs.empty() ? CameraObject::Config{}
                                                       : cfgs[0];
        cameras_.push_back(std::make_unique<CameraObject>(cfg));
        // One controller per camera; they act on the same bodies but hold
        // separate selections.
        controllers_.push_back(
            std::make_unique<BoxController>(boxControllerConfig()));
        camerasActive_.push_back(i < cfgs.size() ? 1 : 0);
    }
    // ギズモが最初。pick / drag をカメラやグラブより先に見て、ハンドルに
    // 当たっていればそこで止める（選択し直しや自由移動をさせないため）。
    addComponent(std::make_unique<GizmoComponent>());
    addComponent(std::make_unique<CameraControlComponent>());
    addComponent(std::make_unique<BoxControlComponent>());
}

std::size_t Scene::pickBoxAt(double ndcX, double ndcY,
                             std::size_t cameraIndex) {
    if (cameraIndex >= cameras_.size()) return BoxController::kNone;

    // Ray from the camera through the clicked point, intersected with every
    // object. Done here rather than through the collision engine so it behaves
    // the same on the Core and Multicore backends.
    const auto basis = scenemath::cameraBasis(*cameras_[cameraIndex]);
    if (!basis.valid) return BoxController::kNone;
    const scenemath::Vec3 dir = scenemath::rayThrough(
        basis, ndcX, ndcY, renderer_.verticalFovDegrees(), renderer_.aspect());

    // オブジェクトごとに形と大きさが違うので、姿勢は共有スナップショットから
    // 取る（HTTP/INPUT スレッドから物理世界を読まないため）。
    std::vector<BodyTransform> poses;
    {
        std::lock_guard<std::mutex> pl(poseMutex_);
        poses = latestPoses_;
    }

    std::unique_lock<std::mutex> lk(objectsMutex_);
    std::size_t best = BoxController::kNone;
    double bestT = 0.0;
    for (std::size_t i = 0; i < boxes_.size() && i < poses.size(); ++i) {
        if (!boxes_[i].alive) continue;
        const ed::BodyDesc& d = boxes_[i].desc;
        const BodyTransform t = poses[i];
        const double hit =
            (d.shape == ed::ShapeKind::Box)
                ? scenemath::rayHitsBox(
                      basis.eye, dir, t,
                      scenemath::Vec3(d.size.x * 0.5, d.size.y * 0.5,
                                      d.size.z * 0.5))
                : scenemath::rayHitsSphere(basis.eye, dir, t, d.radius());
        if (hit < 0.0) continue;
        if (best == BoxController::kNone || hit < bestT) {
            best = i;
            bestT = hit;
        }
    }
    lk.unlock();

    // エディタカメラの pick はオブジェクト選択（または空クリック）なので、
    // ライト / カメラのエディタ選択はここで外れる。アイコンに当たった pick は
    // GizmoComponent が先に消費していて、ここへは来ない。
    if (cameraIndex == editorCamera()) editor_.clearSel();
    controllers_[cameraIndex]->setSelected(best);
    return best;
}

PhysicsBackend scenePhysicsBackend() {
    return kBackend;
}

std::size_t Scene::editorCamera() const {
    return kEditorCamera < cameras_.size() ? kEditorCamera : 0;
}

// 実行時に変わりうる値は EditorState の設定が正。SceneConfig.h の定数は
// その初期値（build() で流し込む）。ここを経由させておくと、ブラウザで
// 変えた値・保存した値・起動時の既定値が同じ 1 か所から出てくる。
int Scene::substeps() const {
    return editor_.sim().substeps;
}

int Scene::solverIterations() const {
    return editor_.sim().iterations;
}

int Scene::physicsHz() const {
    return editor_.sim().hz;
}

double Scene::collisionEnvelope() const {
    return editor_.sim().envelope;
}

double Scene::contactRecovery() const {
    return editor_.sim().recovery;
}

bool Scene::idleWhenUnwatched() const {
    return kIdleWhenUnwatched;
}

void Scene::build() {
    // SceneConfig.h の定数を「エディタ文書の初期値」として EditorState に
    // 入れる。以後はこちらが正になり、ブラウザからの変更・保存・読み込みが
    // 同じ経路を通る。
    {
        ed::SimSettings s;
        s.gravity = kGravityY;
        s.hz = kPhysicsHz;
        s.substeps = kSubsteps;
        s.iterations = kSolverIterations;
        s.envelope = kCollisionEnvelope;
        s.recovery = kContactRecovery;
        s.friction = kFriction;
        s.restitution = kRestitution;
        s.linearDamping = kLinearDamping;
        s.angularDamping = kAngularDamping;
        s.sleeping = kSleepingEnabled;
        editor_.setSim(s);
    }
    editor_.setMode(kStartMode);
    editor_.refreshSceneFiles();

    // Must precede body creation.
    physics_.setCollisionTolerances(kCollisionEnvelope, kCollisionMargin);
    physics_.setContactSettings(kContactRecovery, kSolverTolerance);
    physics_.setRollingFriction(kRollingFriction, kSpinningFriction);
    applySimSettings();  // 重力・摩擦・減衰・スリープ・反復回数
    renderer_.setBoxColor({kBoxR, kBoxG, kBoxB});

    // Ground: a static collision box in physics + a lit plane in the renderer.
    // 番号を覚えておくのは、ジョイントの「ワールド側」に使うため。
    groundPhysId_ = physics_.addBox(kGroundSize, 1.0, kGroundSize, kDensity,
                                    ChVector3d(0, -0.5, 0), QUNIT,
                                    /*fixed*/ true);
    renderer_.addGround(kGroundHalf, {kGroundTintR, kGroundTintG, kGroundTintB},
                        kGroundTile, kGroundTexture);

    if (kModelPath[0] != '\0') {
        const std::size_t model = renderer_.addModel(kModelPath);
        {
            // Yaw about +Y, built by hand: quaternion factory names differ
            // between filament math versions, and an explicit quatf(w,x,y,z)
            // works everywhere.
            const float yaw = float(scenemath::radians(kModelYawDegrees));
            const filament::math::float3 position{kModelX, kModelY, kModelZ};
            const filament::math::quatf rotation(std::cos(yaw * 0.5f), 0.0f,
                                                 std::sin(yaw * 0.5f), 0.0f);
            renderer_.setModelTransform(model, position, rotation, kModelScale);
        }
        if (kModelCollision) {
            // Static collision mesh at the model's pose. A low-poly proxy
            // (kModelCollisionPath) is preferred when configured; otherwise
            // the drawn model itself is used.
            const char* collisionSrc = kModelCollisionPath[0] != '\0'
                                           ? kModelCollisionPath
                                           : kModelPath;
            auto mesh =
                chrono_types::make_shared<chrono::ChTriangleMeshConnected>();
            if (wizengine::loadCollisionMesh(collisionSrc, kModelScale,
                                             *mesh)) {
                const double yaw = scenemath::radians(kModelYawDegrees);
                physics_.addStaticMesh(
                    mesh, ChVector3d(kModelX, kModelY, kModelZ),
                    ChQuaternion<>(std::cos(yaw * 0.5), 0.0,
                                   std::sin(yaw * 0.5), 0.0));
                LOGI("scene", "model '%s': static mesh collision enabled",
                     collisionSrc);
            } else {
                LOGW("scene",
                     "model '%s': collision mesh unavailable - decoration only",
                     collisionSrc);
            }
        }
    }

    // Optionally draw the dynamic objects as a model instead of the built-in
    // cube. One asset, N instances - the mesh is shared.
    // Loading throws if the model is unusable, so reaching the next line means
    // the objects really are drawn as that model.
    useModel_ = kBoxModelPath[0] != '\0';
    if (useModel_) {
        renderer_.createModelInstances(kBoxModelPath, boxTotal());
        // インスタンスは数が固定。エディタで消しても番号は返ってこない
        // （インスタンスを壊す口が gltfio に無いため）ので、使い切ったら
        // 以降は組み込みの球で置き換える。
        modelCapacity_ = boxTotal();
        modelNext_ = 0;
    }
    // No list of files to keep in sync here: every loader throws AssetError
    // when it cannot read what it was given, so a file added to the scene
    // config later is validated by the same path automatically.

    // シーンの初期ライトを編集可能な設計値として取り込む。実体（Filament の
    // ライト）は RENDER スレッドが最初の applyToRenderer（syncLights）で作る。
    for (const auto& cfg : lightConfigs()) {
        LightItem item;
        item.desc = lightDescFromConfig(cfg);
        lights_.push_back(item);
    }

    if (kEnvironmentHdr[0] != '\0') {
        renderer_.loadEnvironment(kEnvironmentHdr, kEnvironmentIntensity);
    }

    renderer_.configureHighlightColors(cameraColors());
    // One line per camera, in that camera's colour.
    {
        std::vector<filament::math::float3> lineColors;
        for (std::size_t c = 0; c < cameras_.size(); ++c) {
            lineColors.push_back(cameraColor(c));
        }
        renderer_.configureGrabLines(lineColors);
    }
    // ギズモの線。色ごとに 1 レンダラブルで、中身は毎フレーム書き換える。
    renderer_.configureLineBatches(gizmo::batchColors(), gizmo::kMaxSegments);
    // Y=0 グリッド用の細線セット（作成順 = gizmo::kGridSet* の番号）。
    // 中身は GizmoComponent が表示状態や間隔の変わったときに入れる。
    for (const auto& c : gizmo::gridColors()) renderer_.addLineSet(c);

    if (useModel_) {
        modelScale_ = kBoxModelScale;
        const float modelSize = renderer_.modelInstanceSize();
        LOGI("scene",
             "bodies drawn as: glTF model instances ('%s', model size %.4f -> "
             "%.4f m at scale %.3f; shape is %.3f m across)",
             kBoxModelPath, modelSize, modelSize * modelScale_, modelScale_,
             kBoxSize);
    } else {
        LOGI("scene", "bodies drawn as: built-in cube (kBoxModelPath=\"%s\")",
             kBoxModelPath);
    }

    // ConvexHull bodies collide as the drawn model's hull: load the point
    // cloud once here, shared by every body. Empty = unavailable, and the
    // loop below falls back to spheres so the scene still runs.
    if (kBodyShape == BodyShape::ConvexHull) {
        if (useModel_) {
            hullPoints_ =
                wizengine::loadCollisionPoints(kBoxModelPath, kBoxModelScale);
        }
        if (hullPoints_.empty()) {
            LOGW("scene",
                 "convex hull unavailable (kBoxModelPath=\"%s\") - bodies "
                 "fall back to spheres",
                 kBoxModelPath);
        }
    }

    // 既定シーンの格子。エディタから見ると「最初から置いてあるオブジェクト」
    // で、掴む・動かす・消す・ジョイントを付けるが普通にできる。
    ed::BodyDesc proto;
    proto.shape = useModel_ ? ed::ShapeKind::Model
                            : (kBodyShape == BodyShape::Box
                                   ? ed::ShapeKind::Box
                                   : ed::ShapeKind::Sphere);
    proto.collision = (kBodyShape == BodyShape::Box) ? ed::ShapeKind::Box
                      : (kBodyShape == BodyShape::Sphere)
                          ? ed::ShapeKind::Sphere
                          : ed::ShapeKind::Model;  // Model = 凸包
    proto.size = {kBoxSize, kBoxSize, kBoxSize};
    proto.mass = kMassPerBody;
    proto.fixed = false;
    proto.color = {kBoxR, kBoxG, kBoxB};

    for (std::size_t k = 0; k < boxTotal(); ++k) {
        const ChVector3d p = gridPos(k) + jitter();
        ed::BodyDesc d = proto;
        d.position = {p.x(), p.y(), p.z()};
        // 初期の傾きもオイラー角に直して持つ。設計値が四元数とオイラー角に
        // 二重化すると、インスペクタの数字と実物がずれる元になる。
        d.rotation = eulerFromQuat(dropTilt(k));
        createObject(d);
    }
    snapshot();  // initial poses so the first frame shows the boxes in place
}

void Scene::stepPhysics(double dt) {
    for (auto& c : components_) c->onPhysicsStep(*this, dt);
    for (auto& obj : boxes_) {
        if (!obj.alive) continue;
        for (auto& a : obj.actions) a->onPhysicsStep(obj, physics_, dt);
    }
    physics_.step(dt);
    snapshot();
}

// エディタモードの 1 パス。積分しない代わりに、掴んでいる物の置き直しと
// 姿勢スナップショットの更新だけを行う。物理スレッドから呼ぶこと。
void Scene::stepEditor(double dt) {
    for (auto& c : components_) c->onEditorStep(*this, dt);
    snapshot();
}

// Put everything back where it was placed. Called from the browser (Reset) and
// whenever the simulation is stopped.
void Scene::reset() {
    restoreAuthoredPoses();
    physics_.wakeAll();  // last: it also rebuilds the solver layout
    snapshot();
}

void Scene::restoreAuthoredPoses() {
    for (auto& obj : boxes_) {
        if (!obj.alive || obj.physId == GameObject::kInvalidId) continue;
        const ChVector3d p(obj.desc.position.x, obj.desc.position.y,
                           obj.desc.position.z);
        physics_.placeBody(obj.physId, p, quatFromEuler(obj.desc.rotation));
    }
}

// ---- エディタ実装 ----------------------------------------------------------
// ここから下はすべて PHYSICS スレッドから呼ばれる（PhysicsWorld を触るのが
// そのスレッドだけ、という約束を守るため）。

void Scene::applySimSettings() {
    const ed::SimSettings s = editor_.sim();
    physics_.setGravityY(s.gravity);
    physics_.setSurfaceMaterial(s.friction, s.restitution);
    physics_.setDamping(s.linearDamping, s.angularDamping);
    physics_.setSleepingEnabled(s.sleeping, kSleepSeconds, kSleepMinLinVel,
                                kSleepMinAngVel);
    physics_.setSolverIterations(s.iterations);
    physics_.setContactRecoverySpeed(s.recovery);
}

std::size_t Scene::jointBodyId(int objectIndex) const {
    if (objectIndex < 0) return groundPhysId_;  // -1 = 地面（ワールド）
    const std::size_t i = std::size_t(objectIndex);
    if (i >= boxes_.size() || !boxes_[i].alive) return GameObject::kInvalidId;
    return boxes_[i].physId;
}

void Scene::buildJoints() {
    physics_.removeAllJoints();
    const auto joints = editor_.joints();
    std::size_t made = 0;
    for (const auto& j : joints) {
        const std::size_t a = jointBodyId(j.bodyA);
        const std::size_t b = jointBodyId(j.bodyB);
        if (a == GameObject::kInvalidId || b == GameObject::kInvalidId) {
            LOGW("editor", "joint '%s': endpoint is gone - skipped",
                 j.name.c_str());
            continue;
        }
        const std::size_t id = physics_.addJoint(
            toJointType(j.kind), a, b,
            ChVector3d(j.anchor.x, j.anchor.y, j.anchor.z),
            ChVector3d(j.axis.x, j.axis.y, j.axis.z), j.distance);
        if (id != PhysicsWorld::kInvalidJoint) ++made;
    }
    if (!joints.empty()) {
        LOGI("editor", "joints: %zu / %zu created", made, joints.size());
    }
}

void Scene::enterMode(ed::AppMode target) {
    if (target == editor_.mode()) return;
    if (target == ed::AppMode::Simulate) {
        // エディタ中に形や大きさを変えたぶんを、ここでまとめて実体に反映する。
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            if (boxes_[i].alive && boxes_[i].physDirty) rebuildBody(i);
        }
        // 設計どおりの姿勢から始める。ジョイントはここで作り直す:
        // エディタで物を動かしたあとも、拘束が今の位置に合った状態で張られる。
        restoreAuthoredPoses();
        applySimSettings();
        buildJoints();
        physics_.wakeAll();
    } else {
        // 止める = 設計状態へ巻き戻す。拘束を先に外さないと、置き直した
        // 姿勢と食い違ったまま次のステップで暴れる。
        physics_.removeAllJoints();
        restoreAuthoredPoses();
    }
    snapshot();
    editor_.setMode(target);
    // ライト / カメラの編集はエディタモード専用なので、シミュレートに入る
    // ときは選択も畳む（アイコンも消えるため、選択だけ残ると分かりにくい）。
    if (target == ed::AppMode::Simulate) editor_.clearSel();
    editor_.setStatus(target == ed::AppMode::Simulate ? "シミュレート開始"
                                                      : "エディタに戻りました");
    LOGI("editor", "mode -> %s", ed::modeName(target));
}

std::size_t Scene::createObject(const ed::BodyDesc& descIn) {
    ed::BodyDesc desc = ed::clampBody(descIn);

    // 描画: glTF インスタンスは有限。使い切ったら組み込みの球に落とす。
    bool modelDraw = false;
    std::size_t renderId = GameObject::kInvalidId;
    if (desc.shape == ed::ShapeKind::Model) {
        if (modelNext_ < modelCapacity_) {
            renderId = modelNext_++;
            modelDraw = true;
        } else {
            desc.shape = ed::ShapeKind::Sphere;
        }
    }

    const ChVector3d pos(desc.position.x, desc.position.y, desc.position.z);
    const ChQuaternion<> rot = quatFromEuler(desc.rotation);

    // 当たり判定。Model は「モデルの凸包」で、点群が無ければ球へ。
    std::size_t physId;
    if (desc.collision == ed::ShapeKind::Model && !hullPoints_.empty()) {
        physId = physics_.addConvexHull(hullPoints_, desc.density(), pos, rot);
        if (desc.fixed) physics_.setBodyFixed(physId, true);
    } else if (desc.collision == ed::ShapeKind::Box) {
        physId = physics_.addBox(desc.size.x, desc.size.y, desc.size.z,
                                 desc.density(), pos, rot, desc.fixed);
    } else {
        physId = physics_.addSphere(desc.size.x * 0.5, desc.density(), pos, rot,
                                    desc.fixed);
    }

    GameObject obj;
    obj.physId = physId;
    obj.renderId = renderId;   // Box/Sphere は RENDER スレッドが作る
    obj.modelDraw = modelDraw;
    obj.desc = desc;
    obj.alive = true;
    obj.colorDirty = true;

    std::lock_guard<std::mutex> lk(objectsMutex_);
    boxes_.push_back(std::move(obj));
    return boxes_.size() - 1;
}

void Scene::destroyObject(std::size_t index) {
    if (index >= boxes_.size() || !boxes_[index].alive) return;
    if (boxes_[index].physId != GameObject::kInvalidId) {
        physics_.disableBody(boxes_[index].physId);
    }

    // 消えたオブジェクトを参照するジョイントも一緒に落とす。残すと次の
    // シミュレート開始で「端点が無い」と言われ続けるだけ。
    auto joints = editor_.joints();
    const int idx = int(index);
    const std::size_t before = joints.size();
    joints.erase(std::remove_if(joints.begin(), joints.end(),
                                [idx](const ed::JointDesc& j) {
                                    return j.bodyA == idx || j.bodyB == idx;
                                }),
                 joints.end());
    if (joints.size() != before) {
        editor_.setJoints(std::move(joints));
        if (editor_.mode() == ed::AppMode::Simulate) buildJoints();
    }

    // 誰かが掴んだままなら離させる。
    for (auto& c : controllers_) {
        if (c->selected() == index) c->setSelected(BoxController::kNone);
    }

    std::lock_guard<std::mutex> lk(objectsMutex_);
    boxes_[index].alive = false;
    // renderId はそのまま。片付けは RENDER スレッド（syncRenderables）。
}

void Scene::moveObject(std::size_t index, double x, double y, double z) {
    if (index >= boxes_.size() || !boxes_[index].alive) return;
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        boxes_[index].desc.position = {x, y, z};
    }
    GameObject& obj = boxes_[index];
    if (obj.physId == GameObject::kInvalidId) return;
    physics_.placeBody(obj.physId, ChVector3d(x, y, z),
                       quatFromEuler(obj.desc.rotation));
}

void Scene::rotateObject(std::size_t index, double rx, double ry, double rz) {
    if (index >= boxes_.size() || !boxes_[index].alive) return;
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        boxes_[index].desc.rotation = {rx, ry, rz};
    }
    GameObject& obj = boxes_[index];
    if (obj.physId == GameObject::kInvalidId) return;
    const ed::Vec3d& p = obj.desc.position;
    physics_.placeBody(obj.physId, ChVector3d(p.x, p.y, p.z),
                       quatFromEuler(obj.desc.rotation));
}

void Scene::resizeObject(std::size_t index, double sx, double sy, double sz) {
    if (index >= boxes_.size() || !boxes_[index].alive) return;
    auto clamp = [](double v) { return v < 0.01 ? 0.01 : (v > 50.0 ? 50.0 : v); };
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        boxes_[index].desc.size = {clamp(sx), clamp(sy), clamp(sz)};
        // 描画は毎フレームこの寸法でスケール行列を作り直すので即座に効く
        // （renderDirty は「メッシュそのものが変わった」ときだけ）。剛体の
        // ほうは形を変えられないので作り直しが要る＝エディタ中は遅らせる。
        boxes_[index].physDirty = true;
    }
    if (editor_.mode() == ed::AppMode::Simulate) rebuildBody(index);
}

void Scene::rebuildBody(std::size_t index) {
    if (index >= boxes_.size() || !boxes_[index].alive) return;
    GameObject& obj = boxes_[index];
    if (!obj.physDirty) return;

    // 古いボディは退場させる（Multicore で本当に消すのは危ないので、当たり
    // 判定を切って地面の下へ）。エディタで何度も大きさを変えると空のボディが
    // 溜まるが、動かないし当たらないのでステップ時間には効かない。
    if (obj.physId != GameObject::kInvalidId) physics_.disableBody(obj.physId);

    const ed::BodyDesc& d = obj.desc;
    const ChVector3d pos(d.position.x, d.position.y, d.position.z);
    const ChQuaternion<> rot = quatFromEuler(d.rotation);
    std::size_t physId;
    if (d.collision == ed::ShapeKind::Model && !hullPoints_.empty()) {
        physId = physics_.addConvexHull(hullPoints_, d.density(), pos, rot);
        if (d.fixed) physics_.setBodyFixed(physId, true);
    } else if (d.collision == ed::ShapeKind::Box) {
        physId = physics_.addBox(d.size.x, d.size.y, d.size.z, d.density(), pos,
                                 rot, d.fixed);
    } else {
        physId = physics_.addSphere(d.size.x * 0.5, d.density(), pos, rot,
                                    d.fixed);
    }

    std::lock_guard<std::mutex> lk(objectsMutex_);
    obj.physId = physId;
    obj.physDirty = false;
    obj.renderDirty = true;  // メッシュも作り直す（球↔箱が変わりうる）
}

// ---- ライトの編集（PHYSICS スレッド）---------------------------------------

std::size_t Scene::createLight(const ed::LightDesc& descIn) {
    LightItem item;
    item.desc = ed::clampLight(descIn);
    std::lock_guard<std::mutex> lk(objectsMutex_);
    lights_.push_back(item);
    return lights_.size() - 1;
}

void Scene::destroyLight(std::size_t index) {
    if (index >= lights_.size() || !lights_[index].alive) return;
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        lights_[index].alive = false;
        // renderId はそのまま。実体の破棄は RENDER スレッド（syncLights）。
    }
    if (editor_.selKind() == EditorState::SelKind::Light &&
        editor_.selIndex() == int(index)) {
        editor_.clearSel();
    }
}

void Scene::moveLight(std::size_t index, double x, double y, double z) {
    if (index >= lights_.size() || !lights_[index].alive) return;
    std::lock_guard<std::mutex> lk(objectsMutex_);
    lights_[index].desc.position = {x, y, z};
    lights_[index].stateDirty = true;
}

void Scene::rotateLight(std::size_t index, double rx, double ry, double rz) {
    if (index >= lights_.size() || !lights_[index].alive) return;
    std::lock_guard<std::mutex> lk(objectsMutex_);
    lights_[index].desc.rotation = {rx, ry, rz};
    lights_[index].stateDirty = true;
}

// ---- カメラの編集（PHYSICS スレッド。CameraObject は atomic なので即時）----

void Scene::moveCamera(std::size_t index, double x, double y, double z) {
    if (index >= cameras_.size() || !cameraActive(index)) return;
    CameraObject& c = *cameras_[index];
    // 平行移動: 向きと距離は保つ。eye = target + radius*v なので、
    // 新しい target は新しい eye から同じぶんだけ引いた位置。
    const scenemath::Vec3 v = orbitVector(c.azimuth(), c.elevation());
    const double r = c.radius();
    c.setTarget(x - r * v.x(), y - r * v.y(), z - r * v.z());
}

void Scene::rotateCamera(std::size_t index, double pitchDeg, double yawDeg) {
    if (index >= cameras_.size() || !cameraActive(index)) return;
    CameraObject& c = *cameras_[index];
    // その場で向きだけ変える: eye を固定し、注視点を回した先へ置き直す。
    const auto eye = c.eye();
    c.setPose((yawDeg - 180.0) * kDegToRad, -pitchDeg * kDegToRad, c.radius());
    // setPose は仰角をクランプするので、実際に入った値で target を計算する。
    const scenemath::Vec3 v = orbitVector(c.azimuth(), c.elevation());
    const double r = c.radius();
    c.setTarget(eye.x - r * v.x(), eye.y - r * v.y(), eye.z - r * v.z());
}

void Scene::cameraEditPose(std::size_t index, ed::Vec3d& pos,
                           ed::Vec3d& rot) const {
    if (index >= cameras_.size()) return;
    const CameraObject& c = *cameras_[index];
    const auto eye = c.eye();
    pos = {eye.x, eye.y, eye.z};
    rot = {cameraPitchDeg(c), cameraYawDeg(c), 0.0};
}

// ---- 初期値へのリセット（clear と、古い保存文書の読み込み）------------------

void Scene::resetLightsToDefaults() {
    std::lock_guard<std::mutex> lk(objectsMutex_);
    for (auto& l : lights_) l.alive = false;  // 実体の片付けは syncLights
    for (const auto& cfg : lightConfigs()) {
        LightItem item;
        item.desc = lightDescFromConfig(cfg);
        lights_.push_back(item);
    }
}

void Scene::resetCamerasToDefaults() {
    const auto cfgs = cameraConfigs();
    std::lock_guard<std::mutex> lk(objectsMutex_);
    for (std::size_t i = 0; i < cameras_.size(); ++i) {
        const CameraObject::Config cfg =
            (i < cfgs.size()) ? cfgs[i] : cfgs.empty() ? CameraObject::Config{}
                                                       : cfgs[0];
        cameras_[i]->setPose(cfg.azimuth, cfg.elevation, cfg.radius);
        cameras_[i]->setTarget(cfg.targetX, cfg.targetY, cfg.targetZ);
        camerasActive_[i] = (i < cfgs.size()) ? 1 : 0;
    }
    camerasActive_[editorCamera()] = 1;  // エディタカメラは常に居る
}

// RENDER スレッド。実体の無いライトを作り、消されたもの・作り直しが要る
// ものを片付ける。applyToRenderer からロック済みで呼ばれる。
void Scene::syncLights() {
    for (auto& l : lights_) {
        const bool wantRelease = (!l.alive || l.rebuild) &&
                                 l.renderId != GameObject::kInvalidId;
        if (wantRelease) {
            renderer_.removeLight(l.renderId);
            l.renderId = GameObject::kInvalidId;
        }
        l.rebuild = false;
        if (!l.alive) continue;

        if (l.renderId == GameObject::kInvalidId) {
            l.renderId = renderer_.addLight(toRendererLight(l.desc));
            l.stateDirty = false;
            continue;
        }
        if (l.stateDirty) {
            const wizengine::LightDesc r = toRendererLight(l.desc);
            renderer_.updateLight(l.renderId, r.color, r.intensity,
                                  r.direction, r.position);
            l.stateDirty = false;
        }
    }
}

void Scene::applyPendingEdits() {
    // モード切替が先。オブジェクトの増減より前に済ませておくと、切替直後の
    // 操作が新しいモードの規則で処理される。
    ed::AppMode target;
    if (editor_.takeModeRequest(target)) enterMode(target);
    if (!editor_.hasPending()) return;
    for (const auto& op : editor_.drain()) applyEditorOp(op);
}

void Scene::applyEditorOp(const EditorState::Op& op) {
    const nlohmann::json& a = op.args;

    if (op.kind == "add") {
        ed::BodyDesc d = ed::bodyFromJson(a, ed::BodyDesc{});
        const std::size_t index = createObject(d);
        // 置いたものをそのカメラの選択にしておく（すぐ動かせる）。
        if (op.camera < controllers_.size()) {
            controllers_[op.camera]->setSelected(index);
        }
        editor_.clearSel();  // ライト / カメラの選択とは排他
        editor_.setStatus("オブジェクトを追加: #" + std::to_string(index));
        snapshot();
        return;
    }

    if (op.kind == "remove") {
        const int index = a.value("index", -1);
        if (index < 0 || std::size_t(index) >= boxes_.size()) return;
        destroyObject(std::size_t(index));
        editor_.setStatus("削除: #" + std::to_string(index));
        return;
    }

    if (op.kind == "duplicate") {
        const int index = a.value("index", -1);
        if (index < 0 || std::size_t(index) >= boxes_.size() ||
            !boxes_[std::size_t(index)].alive) {
            return;
        }
        ed::BodyDesc d = boxes_[std::size_t(index)].desc;
        // 真上に少しずらして置く（重ねて置くと選び分けられない）。
        d.position.y += std::max(d.size.y, d.size.x) * 1.2 + 0.05;
        const std::size_t made = createObject(d);
        if (op.camera < controllers_.size()) {
            controllers_[op.camera]->setSelected(made);
        }
        editor_.clearSel();
        editor_.setStatus("複製: #" + std::to_string(index) + " -> #" +
                          std::to_string(made));
        snapshot();
        return;
    }

    if (op.kind == "set") {
        const int index = a.value("index", -1);
        if (index < 0 || std::size_t(index) >= boxes_.size() ||
            !boxes_[std::size_t(index)].alive) {
            return;
        }
        GameObject& obj = boxes_[std::size_t(index)];
        const ed::BodyDesc before = obj.desc;
        ed::BodyDesc next = ed::clampBody(ed::bodyFromJson(a, before));

        const bool shapeChanged = next.shape != before.shape ||
                                  next.collision != before.collision;
        const bool sizeChanged = next.size.x != before.size.x ||
                                 next.size.y != before.size.y ||
                                 next.size.z != before.size.z;
        const bool massChanged = next.mass != before.mass;
        const bool poseChanged = next.position.x != before.position.x ||
                                 next.position.y != before.position.y ||
                                 next.position.z != before.position.z ||
                                 next.rotation.x != before.rotation.x ||
                                 next.rotation.y != before.rotation.y ||
                                 next.rotation.z != before.rotation.z;
        const bool colorChanged = next.color.r != before.color.r ||
                                  next.color.g != before.color.g ||
                                  next.color.b != before.color.b;
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            obj.desc = next;
            if (colorChanged) obj.colorDirty = true;
            if (shapeChanged || sizeChanged || massChanged) obj.physDirty = true;
            // レンダラブルを作り直すのは、メッシュが別物になるとき（箱↔球）
            // だけ。大きさはスケール行列で毎フレーム効くので作り直さない。
            if (shapeChanged) obj.renderDirty = true;
        }
        if (next.fixed != before.fixed && obj.physId != GameObject::kInvalidId) {
            physics_.setBodyFixed(obj.physId, next.fixed);
        }
        if (poseChanged && obj.physId != GameObject::kInvalidId) {
            physics_.placeBody(obj.physId,
                               ChVector3d(next.position.x, next.position.y,
                                          next.position.z),
                               quatFromEuler(next.rotation));
        }
        // 形が変わったら Chrono のボディを作り直す必要がある。エディタ中は
        // シミュレート開始まで待つ（当たり判定は使っていないので困らない）。
        if (obj.physDirty && editor_.mode() == ed::AppMode::Simulate) {
            rebuildBody(std::size_t(index));
        }
        snapshot();
        return;
    }

    if (op.kind == "joint.add") {
        ed::JointDesc j = ed::jointFromJson(a, ed::JointDesc{});
        if (j.bodyA == j.bodyB) {
            editor_.setStatus("ジョイント: 同じオブジェクト同士は繋げません");
            return;
        }
        const std::size_t bodyA = jointBodyId(j.bodyA);
        const std::size_t bodyB = jointBodyId(j.bodyB);
        if (bodyA == GameObject::kInvalidId || bodyB == GameObject::kInvalidId) {
            editor_.setStatus("ジョイント: 対象が見つかりません");
            return;
        }
        // アンカー無指定なら 2 体の中点。「AとBを繋ぐ」と言われたときに
        // いちばん妥当な軸位置で、ブラウザ側が座標を知らなくても作れる。
        // 相手が地面のときは中点を取らない（地面のボディは原点にあるので、
        // 中点だと関係ない場所に軸ができてしまう）。オブジェクト自身の位置
        // ＝「その場で床に留める」が期待どおりの動き。
        if (!a.contains("anchor")) {
            const BodyTransform ta = physics_.bodyTransform(bodyA);
            const BodyTransform tb = physics_.bodyTransform(bodyB);
            if (j.bodyA < 0) {
                j.anchor = {tb.px, tb.py, tb.pz};
            } else if (j.bodyB < 0) {
                j.anchor = {ta.px, ta.py, ta.pz};
            } else {
                j.anchor = {(ta.px + tb.px) * 0.5, (ta.py + tb.py) * 0.5,
                            (ta.pz + tb.pz) * 0.5};
            }
        }
        const int index = editor_.addJoint(j);
        if (editor_.mode() == ed::AppMode::Simulate) buildJoints();
        editor_.setStatus("ジョイントを追加: #" + std::to_string(index) + " (" +
                          ed::jointName(j.kind) + ")");
        return;
    }

    if (op.kind == "joint.remove") {
        const int index = a.value("index", -1);
        if (!editor_.removeJoint(index)) return;
        if (editor_.mode() == ed::AppMode::Simulate) buildJoints();
        editor_.setStatus("ジョイントを削除: #" + std::to_string(index));
        return;
    }

    // ---- ライト -----------------------------------------------------------
    if (op.kind == "light.add") {
        const std::size_t index = createLight(ed::lightFromJson(a, ed::LightDesc{}));
        // 追加したライトを選択にして、すぐギズモ / インスペクタで動かせるように。
        if (op.camera < controllers_.size()) {
            controllers_[op.camera]->setSelected(BoxController::kNone);
        }
        editor_.setSel(EditorState::SelKind::Light, int(index));
        editor_.setStatus("ライトを追加: #" + std::to_string(index));
        return;
    }

    if (op.kind == "light.set") {
        const int index = a.value("index", -1);
        if (index < 0 || std::size_t(index) >= lights_.size() ||
            !lights_[std::size_t(index)].alive) {
            return;
        }
        LightItem& l = lights_[std::size_t(index)];
        const ed::LightDesc before = l.desc;
        const ed::LightDesc next = ed::clampLight(ed::lightFromJson(a, before));
        // 種類・影・減衰・円錐角は Filament のライト実体を決めるので作り直し。
        // それ以外（色・強さ・位置・向き）は実体を保ったまま流し込める。
        const bool rebuild = next.kind != before.kind ||
                             next.shadows != before.shadows ||
                             next.falloff != before.falloff ||
                             next.spotInnerDeg != before.spotInnerDeg ||
                             next.spotOuterDeg != before.spotOuterDeg;
        std::lock_guard<std::mutex> lk(objectsMutex_);
        l.desc = next;
        l.stateDirty = true;
        if (rebuild) l.rebuild = true;
        return;
    }

    if (op.kind == "light.remove") {
        const int index = a.value("index", -1);
        if (index < 0 || std::size_t(index) >= lights_.size()) return;
        destroyLight(std::size_t(index));
        editor_.setStatus("ライトを削除: #" + std::to_string(index));
        return;
    }

    // ---- カメラ -----------------------------------------------------------
    if (op.kind == "camera.add") {
        // 空きスロット（削除済み or 予備）を有効化する。エンドポイントは
        // 起動時に全スロットぶん作ってあるので、ページはもう存在している。
        std::size_t slot = cameras_.size();
        for (std::size_t i = 0; i < cameras_.size(); ++i) {
            if (!cameraActive(i)) { slot = i; break; }
        }
        if (slot >= cameras_.size()) {
            editor_.setStatus("カメラはこれ以上増やせません（最大 " +
                              std::to_string(cameras_.size()) + "）");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            camerasActive_[slot] = 1;
        }
        // 置き場所: 既定の見下ろし位置を、スロットごとに向きを変えて。
        cameras_[slot]->setPose(0.66 + 1.1 * double(slot), 0.30, 10.0);
        cameras_[slot]->setTarget(0.0, 1.0, 0.0);
        editor_.setSel(EditorState::SelKind::Camera, int(slot));
        if (op.camera < controllers_.size()) {
            controllers_[op.camera]->setSelected(BoxController::kNone);
        }
        editor_.setStatus("カメラ " + std::to_string(slot) +
                          " を追加（/cam" + std::to_string(slot) + "/）");
        return;
    }

    if (op.kind == "camera.set") {
        const int index = a.value("index", -1);
        if (index < 0 || std::size_t(index) >= cameras_.size() ||
            !cameraActive(std::size_t(index))) {
            return;
        }
        if (a.contains("position")) {
            const ed::Vec3d p = ed::vec3FromJson(a["position"], ed::Vec3d{});
            moveCamera(std::size_t(index), p.x, p.y, p.z);
        }
        if (a.contains("rotation")) {
            const ed::Vec3d r = ed::vec3FromJson(a["rotation"], ed::Vec3d{});
            rotateCamera(std::size_t(index), r.x, r.y);
        }
        return;
    }

    if (op.kind == "camera.remove") {
        const int index = a.value("index", -1);
        if (index < 0 || std::size_t(index) >= cameras_.size()) return;
        if (std::size_t(index) == editorCamera()) {
            editor_.setStatus("Editor Camera は削除できません");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            camerasActive_[std::size_t(index)] = 0;
        }
        controllers_[std::size_t(index)]->setSelected(BoxController::kNone);
        if (editor_.selKind() == EditorState::SelKind::Camera &&
            editor_.selIndex() == index) {
            editor_.clearSel();
        }
        editor_.setStatus("カメラ " + std::to_string(index) + " を削除");
        return;
    }

    if (op.kind == "sim") {
        editor_.setSim(ed::clampSim(ed::simFromJson(a, editor_.sim())));
        applySimSettings();
        editor_.setStatus("シミュレート設定を更新");
        return;
    }

    if (op.kind == "clear") {
        physics_.removeAllJoints();
        editor_.setJoints({});
        for (std::size_t i = 0; i < boxes_.size(); ++i) destroyObject(i);
        for (auto& c : controllers_) c->setSelected(BoxController::kNone);
        // ライトとカメラも初期状態へ（真っ暗なシーンから始めさせない）。
        resetLightsToDefaults();
        resetCamerasToDefaults();
        editor_.clearSel();
        editor_.setSceneFile("");
        editor_.setStatus("シーンを空にしました");
        snapshot();
        return;
    }

    if (op.kind == "save") {
        const std::string name = a.value("name", std::string());
        const std::string path = EditorState::scenePath(name);
        if (path.empty()) {
            editor_.setStatus("保存名が不正です（英数字と _ - のみ）");
            return;
        }
        std::string reason;
        if (EditorState::writeJson(path, documentJson(), reason)) {
            editor_.setSceneFile(name);
            editor_.setStatus("保存しました: " + name);
        } else {
            editor_.setStatus("保存できません: " + reason);
        }
        editor_.refreshSceneFiles();
        return;
    }

    if (op.kind == "load") {
        const std::string name = a.value("name", std::string());
        const std::string path = EditorState::scenePath(name);
        nlohmann::json doc;
        std::string reason;
        if (path.empty() || !EditorState::readJson(path, doc, reason)) {
            editor_.setStatus("読み込めません: " +
                              (reason.empty() ? name : reason));
            return;
        }
        loadDocument(doc);
        editor_.setSceneFile(name);
        editor_.setStatus("読み込みました: " + name);
        editor_.refreshSceneFiles();
        return;
    }

    LOGW("editor", "unknown edit operation '%s'", op.kind.c_str());
}

nlohmann::json Scene::documentJson() const {
    nlohmann::json doc;
    doc["format"] = "wizengine-scene";
    // version 2: lights / cameras が加わった。v1（無印）の文書も読める
    // （欠けている節は初期値へリセットされる。loadDocument を参照）。
    doc["version"] = 2;
    doc["sim"] = ed::toJson(editor_.sim());

    // ライト（削除済みは詰める。オブジェクトと違い番号参照が無いので安全）。
    doc["lights"] = nlohmann::json::array();
    for (const auto& l : lights_) {
        if (l.alive) doc["lights"].push_back(ed::toJson(l.desc));
    }

    // カメラ（全スロット、姿勢と有効フラグ。スロット番号 = 配列位置）。
    doc["cameras"] = nlohmann::json::array();
    for (std::size_t i = 0; i < cameras_.size(); ++i) {
        ed::CameraPose p;
        p.azimuth = cameras_[i]->azimuth();
        p.elevation = cameras_[i]->elevation();
        p.radius = cameras_[i]->radius();
        const auto t = cameras_[i]->target();
        p.target = {t.x, t.y, t.z};
        p.active = camerasActive_[i] != 0;
        doc["cameras"].push_back(ed::toJson(p));
    }

    doc["objects"] = nlohmann::json::array();
    // 保存では番号を詰める。読み込み側は 0 から順に作るので、ジョイントの
    // 参照も詰めた番号に付け替える必要がある。
    std::vector<int> remap(boxes_.size(), -1);
    int next = 0;
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        if (!boxes_[i].alive) continue;
        remap[i] = next++;
        doc["objects"].push_back(ed::toJson(boxes_[i].desc));
    }
    doc["joints"] = nlohmann::json::array();
    for (const auto& j : editor_.joints()) {
        ed::JointDesc copy = j;
        auto fix = [&](int& ref) {
            if (ref < 0) return true;  // 地面はそのまま
            if (std::size_t(ref) >= remap.size() || remap[std::size_t(ref)] < 0)
                return false;
            ref = remap[std::size_t(ref)];
            return true;
        };
        if (!fix(copy.bodyA) || !fix(copy.bodyB)) continue;
        doc["joints"].push_back(ed::toJson(copy));
    }
    return doc;
}

void Scene::loadDocument(const nlohmann::json& doc) {
    // いま在るものを全部畳んでから作り直す。番号は 0 から振り直されるので、
    // 掴んでいる選択も落とす。
    physics_.removeAllJoints();
    editor_.setJoints({});
    for (std::size_t i = 0; i < boxes_.size(); ++i) destroyObject(i);
    for (auto& c : controllers_) c->setSelected(BoxController::kNone);
    editor_.clearSel();

    // ライト。文書に無ければ（v1 の保存）初期構成へ戻す - 読み込んだシーンが
    // 保存時と同じ見た目になるのが原則で、v1 の保存時は必ず初期構成だった。
    if (doc.contains("lights") && doc["lights"].is_array()) {
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            for (auto& l : lights_) l.alive = false;
        }
        for (const auto& lj : doc["lights"]) {
            createLight(ed::lightFromJson(lj, ed::LightDesc{}));
        }
    } else {
        resetLightsToDefaults();
    }

    // カメラ。配列位置 = スロット番号。無い文書は初期構成へ。
    if (doc.contains("cameras") && doc["cameras"].is_array()) {
        const auto& arr = doc["cameras"];
        std::lock_guard<std::mutex> lk(objectsMutex_);
        for (std::size_t i = 0; i < cameras_.size(); ++i) {
            if (i >= arr.size()) { camerasActive_[i] = 0; continue; }
            const ed::CameraPose p =
                ed::cameraPoseFromJson(arr[i], ed::CameraPose{});
            cameras_[i]->setPose(p.azimuth, p.elevation, p.radius);
            cameras_[i]->setTarget(p.target.x, p.target.y, p.target.z);
            camerasActive_[i] = p.active ? 1 : 0;
        }
        camerasActive_[editorCamera()] = 1;  // エディタカメラは常に居る
    } else {
        resetCamerasToDefaults();
    }

    if (doc.contains("sim")) {
        editor_.setSim(ed::clampSim(ed::simFromJson(doc["sim"], editor_.sim())));
        applySimSettings();
    }

    ed::BodyDesc defaults;
    const std::size_t base = boxes_.size();  // 追加ぶんの先頭番号
    if (doc.contains("objects") && doc["objects"].is_array()) {
        for (const auto& o : doc["objects"]) {
            createObject(ed::bodyFromJson(o, defaults));
        }
    }

    std::vector<ed::JointDesc> joints;
    if (doc.contains("joints") && doc["joints"].is_array()) {
        for (const auto& jj : doc["joints"]) {
            ed::JointDesc j = ed::jointFromJson(jj, ed::JointDesc{});
            // 文書の中では 0 起点。実際の番号は既存ぶんだけずれる。
            if (j.bodyA >= 0) j.bodyA += int(base);
            if (j.bodyB >= 0) j.bodyB += int(base);
            joints.push_back(j);
        }
    }
    editor_.setJoints(std::move(joints));
    if (editor_.mode() == ed::AppMode::Simulate) buildJoints();
    snapshot();
}

void Scene::snapshot() {
    std::vector<BodyTransform> poses;
    poses.reserve(boxes_.size());
    for (const auto& obj : boxes_) {
        if (obj.physId == GameObject::kInvalidId) {
            poses.push_back(BodyTransform{0, 0, 0, 1, 0, 0, 0});
            continue;
        }
        poses.push_back(physics_.bodyTransform(obj.physId));
    }
    std::lock_guard<std::mutex> lk(poseMutex_);
    latestPoses_.swap(poses);
}

// RENDER スレッド。まだ実体の無いオブジェクトのレンダラブルを作り、消された
// もの・形が変わったものを片付ける。applyToRenderer からロック済みで呼ばれる。
void Scene::syncRenderables() {
    for (auto& obj : boxes_) {
        // 消えた物 / 作り直しが要る物の後始末。
        const bool wantRelease =
            !obj.alive || (obj.renderDirty && obj.renderId != GameObject::kInvalidId);
        if (wantRelease && obj.renderId != GameObject::kInvalidId) {
            if (obj.modelDraw) {
                // glTF インスタンスは 1 個だけ壊せないので、潰して見えなくする。
                renderer_.setModelInstanceTransform(
                    obj.renderId,
                    filament::math::mat4f::scaling(filament::math::float3{0.0f}));
            } else {
                renderer_.removeShape(obj.renderId);
            }
            obj.renderId = GameObject::kInvalidId;
        }
        obj.renderDirty = false;
        if (!obj.alive) continue;

        if (obj.renderId == GameObject::kInvalidId) {
            // モデル描画は起動時に配ったインスタンス番号でしか使えないので、
            // ここで作れるのは組み込みメッシュだけ（createObject が形を
            // 落としてある）。
            obj.renderId = renderer_.addShape(obj.desc.shape == ed::ShapeKind::Sphere
                                                  ? wizengine::ShapeMesh::Sphere
                                                  : wizengine::ShapeMesh::Box);
            obj.modelDraw = false;
            obj.colorDirty = true;
        }
        if (obj.colorDirty && !obj.modelDraw) {
            renderer_.setShapeColor(
                obj.renderId,
                {obj.desc.color.r, obj.desc.color.g, obj.desc.color.b});
            obj.colorDirty = false;
        }
    }
}

void Scene::applyToRenderer() {
    // オブジェクト一覧を触るあいだはロックしたまま。この中から呼ばれる
    // onRender も同じ一覧を読むので、個々のアクセサはロックを取らない。
    std::unique_lock<std::mutex> lk(objectsMutex_);
    syncRenderables();
    syncLights();

    for (auto& c : components_) c->onRender(*this);

    std::vector<BodyTransform> poses;
    {
        std::lock_guard<std::mutex> pl(poseMutex_);
        poses = latestPoses_;
    }

    // The cube/sphere meshes are unit-sized; scale them to the object's size
    // when placing. A model gets its own tuning factor instead (the mesh is
    // whatever the artist exported).
    const std::size_t n = std::min(poses.size(), boxes_.size());
    for (std::size_t k = 0; k < n; ++k) {
        GameObject& obj = boxes_[k];
        if (!obj.alive || obj.renderId == GameObject::kInvalidId) continue;
        const ed::BodyDesc& d = obj.desc;
        filament::math::float3 s;
        if (obj.modelDraw) {
            s = filament::math::float3{modelScale_};
        } else if (d.shape == ed::ShapeKind::Sphere) {
            s = filament::math::float3{float(d.size.x)};
        } else {
            s = filament::math::float3{float(d.size.x), float(d.size.y),
                                       float(d.size.z)};
        }
        const auto m = toFilament(poses[k]) * filament::math::mat4f::scaling(s);
        if (obj.modelDraw) {
            renderer_.setModelInstanceTransform(obj.renderId, m);
        } else {
            renderer_.setBoxTransform(obj.renderId, m);
        }
    }

    // ---- ジョイントの線 --------------------------------------------------
    // エディタでは A→アンカー→B の 2 本（どこを軸にしたかが見える）、
    // シミュレート中は A→B の 1 本（拘束が今どこを結んでいるかが見える）。
    const auto joints = editor_.joints();
    const bool editing = editor_.isEditor();
    renderer_.setJointLineCount(joints.size() * 2);
    auto endpoint = [&](int body, const ed::Vec3d& anchor,
                        filament::math::float3& out) {
        if (body < 0) {  // 地面: アンカーの真下
            out = {float(anchor.x), 0.0f, float(anchor.z)};
            return true;
        }
        const std::size_t i = std::size_t(body);
        if (i >= n || !boxes_[i].alive) return false;
        out = {float(poses[i].px), float(poses[i].py), float(poses[i].pz)};
        return true;
    };
    for (std::size_t i = 0; i < joints.size(); ++i) {
        const ed::JointDesc& j = joints[i];
        // 片方が見つからないと && で右側が評価されないので、初期値を入れておく
        // （見えない線に渡すだけとはいえ、未初期化の値は流さない）。
        filament::math::float3 pa{0.0f}, pb{0.0f};
        const bool ok = endpoint(j.bodyA, j.anchor, pa) &&
                        endpoint(j.bodyB, j.anchor, pb);
        const filament::math::float3 col = jointColor(j.kind);
        const filament::math::float3 anchor{float(j.anchor.x),
                                            float(j.anchor.y),
                                            float(j.anchor.z)};
        if (!ok) {
            renderer_.setJointLine(i * 2, pa, pb, col, false);
            renderer_.setJointLine(i * 2 + 1, pa, pb, col, false);
            continue;
        }
        if (editing) {
            renderer_.setJointLine(i * 2, pa, anchor, col, true);
            renderer_.setJointLine(i * 2 + 1, anchor, pb, col, true);
        } else {
            renderer_.setJointLine(i * 2, pa, pb, col, true);
            renderer_.setJointLine(i * 2 + 1, pa, pb, col, false);
        }
    }
}
