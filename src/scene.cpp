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

#include "AssetError.h"
#include "GizmoComponent.h"
#include "PhysicsWorld.h"
#include "Renderer.h"
#include "math_bridge.h"

using namespace chrono;
namespace ed = wizengine::editor;

namespace {

// Scene parameters (cameras, lights, physics, ...) live in SceneConfig.h;
// below are only derived helpers and the implementation. シーンの中身
// （オブジェクト・ジョイント・アセット）はここではなく文書（XML）が持つ。

// 既定のマウス操作スクリプト（イベントアセット）の名前。エンジンは「掴んだら
// 動く」を持たず、この名前のアセットがシーンに付いていて初めて物が動く。
// 節を持たない文書と「全消し」はこれを作って付ける（defaultPickupAsset /
// Scene::resetEventsToDefaults）。既定シーン assets/scenes/default.xml にも
// 同じ名前で書いてある。
constexpr const char* kMouseEventAsset = "pickup";

// エディタの回転（オイラー角・度）を Chrono の四元数へ。順序の定義は
// scene_math.h に 1 か所だけ置いてある（インスペクタの数字・ギズモの回転・
// 物理に渡す姿勢が食い違わないように）。
ChQuaternion<> quatFromEuler(const ed::Vec3d& degrees) {
    const scenemath::Quat q =
        scenemath::quatFromEulerDegrees(degrees.x, degrees.y, degrees.z);
    return ChQuaternion<>(q.w(), q.x(), q.y(), q.z());
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

// ---- ライト -----------------------------------------------------------------
constexpr double kRadToDeg = 180.0 / scenemath::kPi;
constexpr double kDegToRad = scenemath::kPi / 180.0;

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

// ---- シーンファイルの読み込み -----------------------------------------------
// 保存名から assets/scenes の文書を読む。今の形式は XML（*.xml）で、同じ名前の
// XML が無ければ旧形式（*.json、version 1〜3）を探す。どちらも読めなければ
// false と理由を返す。読み取りの警告（打ち間違い・未対応の節など。
// SceneDocument.h の「読み取りの約束」）は全文をログに出し、件数を
// warningCount へ返す - ステータス行に「警告 N 件」と出すため。
bool readSceneDocument(const std::string& name, ed::SceneDocument& doc,
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

// ブラウザのドラッグ（掴む・引く・離す）を受け、エディタモードでは掴んだ物を
// 置き直し、カメラごとの色分けハイライトを保つ。
//
// **シミュレート中に掴んだ物を引き寄せるのは、ここではなくイベントアセット**
// （onGrab → grabPull）。エンジンに「掴んだら動く」は焼き込まない、という
// 整理で、既定シーンに付いている "pickup" スクリプトがその配線を持つ。
// 掴みの計算そのもの（対象・カーソルの指す点・引っぱり線）は Scene が持つ
// （Scene::pointerGrab）: エディタの置き直しとイベントの引き寄せで同じ答えが
// 要るため。
class BoxControlComponent : public SceneComponent {
public:
    // エディタモードでのドラッグ: 力で引っぱるのではなく、置いた場所そのものを
    // 書き換える。掴んだ物がカーソルに正確に付いてくるので、配置作業がしやすい。
    // 置き直せるのはエディタカメラだけ - 他のカメラは見る・選ぶまで。
    void onEditorStep(Scene& scene, double dt) override {
        (void)dt;
        const std::size_t editorCam = scene.editorCamera();
        scene.clearGrabLines();  // エディタでは物がカーソル上にあるので線は出さない
        Scene::PointerGrab g;
        if (scene.pointerGrab(editorCam, g) && g.valid) {
            scene.moveObject(g.index, g.tgtX, g.tgtY, g.tgtZ);
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
        // in 3D and are visible from every camera. 端点を書くのは物理スレッド
        // （grabPull アクション）で、ここは読んでレンダラへ流すだけ。
        for (std::size_t c = 0; c < scene.cameraCount(); ++c) {
            double a[3] = {0.0, 0.0, 0.0};
            double b[3] = {0.0, 0.0, 0.0};
            const bool on = scene.grabLine(c, a, b);
            scene.renderer().setGrabLine(
                c, {float(a[0]), float(a[1]), float(a[2])},
                {float(b[0]), float(b[1]), float(b[2])}, on);
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
    static void setMark(Scene& scene, std::size_t index, std::size_t cam,
                        bool on) {
        if (index >= scene.objectCount()) return;
        const GameObject& obj = scene.object(index);
        if (obj.renderId == GameObject::kInvalidId) return;
        // 描き方はオブジェクトごと: 箱・球は組み込みメッシュ、mesh 指定の
        // 物は glTF の実体（<asset> の <mesh> をシーン文書が割り当てる）。
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
    // メッシュアセット（文書の <asset>）。アセットパネルのタイルと
    // Inspector の「Model (名前)」表示が使う。
    j["meshes"] = nlohmann::json::array();
    for (const auto& m : meshes_) j["meshes"].push_back(ed::toJson(m.desc));
    // 地面と環境光（Inspector の World 節が編集する）。
    j["ground"] = ed::toJson(ground_);
    j["environment"] = ed::toJson(environment_);
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

    // イベントアセット（ASSETS パネルのタイル・Inspector の付け外し・
    // ノードエディタが描く）。発火回数付き: シミュレート中にどのノードが
    // 動いたかが見える（Node-RED のデバッグバッジに相当）。
    {
        nlohmann::json ev;
        ev["assets"] = nlohmann::json::array();
        const auto fires = editor_.nodeFireCounts();
        for (const auto& a : editor_.eventAssets()) {
            nlohmann::json e = ed::toJson(a);
            for (auto& n : e["nodes"]) {
                const auto it = fires.find({a.name, ed::jsonInt(n, "id", -1)});
                if (it != fires.end()) n["fired"] = it->second;
            }
            ev["assets"].push_back(std::move(e));
        }
        // シーン全体に付いているアセット（オブジェクトに付いているぶんは
        // それぞれの selected / objects 側に入る）。
        ev["world"] = editor_.worldEvents();
        j["events"] = ev;
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
        // 付いているイベントアセット（階層一覧の ⚡ バッジ）。付いていない
        // 物のほうが多いので、あるときだけ送る。
        if (!boxes_[i].desc.events.empty()) e["events"] = boxes_[i].desc.events;
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
    // 掴み用の配列（奥行きの記憶と引っぱり線）はここで作り切る。カメラは
    // 固定プールなので後から増えない = 描画スレッドが読んでいる最中に
    // 配列が伸びる、が起きない。
    ensureGrabState();
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
    // Ground: 物理の床はここで作る（物理は最初のステップから要る）。見える
    // 地面と環境光は desc + dirty で持ち、最初の applyToRenderer（RENDER
    // スレッド）が作る - シーン読込による実行時の差し替えと同じ経路に揃える。
    // 既定値は GroundDesc / EnvironmentDesc（EditorTypes.h）のまま。
    rebuildGroundBody();

    // シーンの初期ライトを編集可能な設計値として取り込む。実体（Filament の
    // ライト）は RENDER スレッドが最初の applyToRenderer（syncLights）で作る。
    for (const auto& desc : lightConfigs()) {
        LightItem item;
        item.desc = desc;
        lights_.push_back(item);
    }

    // 既定のイベントアセット（マウス操作スクリプト）。起動シーンがイベントの
    // 節を持っていればそちらで上書きされ、読めなければこのまま - どちらでも
    // 「掴んだら動く」が最初から効く。
    resetEventsToDefaults();

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

    // ---- シーンの中身は文書（XML）から ------------------------------------
    // 以前ここにあった格子の自動生成は廃止した。オブジェクトの配置・glTF
    // モデルの割り当てはすべて assets/scenes/*.xml が持ち、コード側は
    // kStartupScene（既定 "default"）を読むだけ。読めなければ警告を出して
    // 空のシーン（地面のみ）で起動する - ブラウザから別のシーンを読み込めば
    // よいので、起動は止めない。
    if (kStartupScene[0] != '\0') {
        ed::SceneDocument doc;
        std::string reason;
        if (readSceneDocument(kStartupScene, doc, reason)) {
            loadDocument(doc);
            editor_.setSceneFile(kStartupScene);
            editor_.setStatus(std::string("読み込みました: ") + kStartupScene);
            LOGI("scene", "startup scene '%s' loaded (%zu bodies, %zu meshes)",
                 kStartupScene, doc.bodies.size(), doc.meshes.size());
        } else {
            LOGW("scene", "startup scene '%s' not loaded: %s (starting empty)",
                 kStartupScene, reason.c_str());
        }
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
    // イベントグラフは step の後: 衝突トリガーはこのステップが作った接触を
    // 見る（前に置くと 1 ステップ古い接触に反応する）。
    runEventGraph(dt);
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
    // イベントの実行状態と、アクションが加えた上書き（色・ライト・固定）も
    // やり直しにする - Reset は「シミュレートを最初から」の意味なので。
    resetGraphRuntime();
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

// ---- マウスの掴み（PHYSICS スレッド）---------------------------------------
// 「どの物を、カーソルのどの点へ」を出すだけ。実際に動かすのはエディタの
// 置き直し（BoxControlComponent）か、イベントアセットの grabPull アクション。

void Scene::ensureGrabState() {
    if (grabDepths_.size() == cameras_.size()) return;
    grabDepths_.assign(cameras_.size(), GrabDepth{});
    grabLines_.clear();
    for (std::size_t i = 0; i < cameras_.size(); ++i) {
        grabLines_.push_back(std::make_unique<GrabLine>());
    }
}

bool Scene::pointerGrab(std::size_t cameraIndex, PointerGrab& out) {
    out = PointerGrab{};
    ensureGrabState();
    if (cameraIndex >= cameras_.size()) return false;

    BoxController& ctl = *controllers_[cameraIndex];
    const std::size_t sel = ctl.selected();
    GrabDepth& depth = grabDepths_[cameraIndex];
    if (sel >= boxes_.size() || !boxes_[sel].alive) {
        depth.valid = false;
        return false;
    }

    double ndcX = 0.0, ndcY = 0.0;
    if (!ctl.pointer(ndcX, ndcY)) {
        // 掴んでいない = 次に掴んだときは奥行きを測り直す。エディタでは
        // 離しても選択が残るので、これが無いと前回のカメラ向きで測った
        // 奥行きを使い回してしまう。
        depth.held = false;
        return false;
    }

    const std::size_t physId = boxes_[sel].physId;
    if (physId == GameObject::kInvalidId) return false;
    const BodyTransform tr = physics_.bodyTransform(physId);

    // Camera basis and the point under the cursor, both from the shared Eigen
    // helpers so picking and dragging cannot drift apart.
    const auto basis = scenemath::cameraBasis(*cameras_[cameraIndex]);
    if (!basis.valid) return false;

    // Depth of the object when it was grabbed: the target rides on the plane
    // at that depth, so the object stays under the cursor without being
    // pulled towards or away from the camera.
    const scenemath::Vec3 objPos(tr.px, tr.py, tr.pz);
    if (!depth.valid || depth.sel != sel || !depth.held) {
        depth.valid = true;
        depth.held = true;
        depth.sel = sel;
        depth.z = std::max(0.1, (objPos - basis.eye).dot(basis.forward));
    }

    // Ray through the cursor, hit against that plane. Absolute, so the target
    // is exactly under the cursor every step - no drift.
    const scenemath::Vec3 dir = scenemath::rayThrough(
        basis, ndcX, ndcY, renderer_.verticalFovDegrees(), renderer_.aspect());
    const double along = dir.dot(basis.forward);
    if (along <= 1e-6) return false;
    const scenemath::Vec3 target = basis.eye + dir * (depth.z / along);

    out.valid = true;
    out.camera = cameraIndex;
    out.index = sel;
    out.physId = physId;
    out.objX = objPos.x();
    out.objY = objPos.y();
    out.objZ = objPos.z();
    out.tgtX = target.x();
    out.tgtY = target.y();
    out.tgtZ = target.z();
    return true;
}

void Scene::setGrabLine(std::size_t cameraIndex, bool on, double ax, double ay,
                        double az, double bx, double by, double bz) {
    ensureGrabState();
    if (cameraIndex >= grabLines_.size()) return;
    GrabLine& l = *grabLines_[cameraIndex];
    l.ax.store(ax);
    l.ay.store(ay);
    l.az.store(az);
    l.bx.store(bx);
    l.by.store(by);
    l.bz.store(bz);
    l.on.store(on);
}

void Scene::clearGrabLines() {
    ensureGrabState();
    for (auto& l : grabLines_) l->on.store(false);
}

bool Scene::grabLine(std::size_t cameraIndex, double* from3,
                     double* to3) const {
    if (cameraIndex >= grabLines_.size()) return false;
    const GrabLine& l = *grabLines_[cameraIndex];
    from3[0] = l.ax.load();
    from3[1] = l.ay.load();
    from3[2] = l.az.load();
    to3[0] = l.bx.load();
    to3[1] = l.by.load();
    to3[2] = l.bz.load();
    return l.on.load();
}

// ---- イベントグラフの実行 ---------------------------------------------------
// すべて PHYSICS スレッド。シミュレートの 1 サブステップごとに、トリガーを
// 判定してワイヤー先のアクションを実行する 1 段だけの評価（アクション →
// アクションの連鎖は無い）。グラフ本体は EditorState、ここは実行するだけ。

namespace {

// 既定のマウス操作スクリプト。「掴んでいる間、カーソルへ引き寄せる」だけの
// 2 ノード。エンジンには焼き込まず、シーンのアセットとして持つ - 動きは全部
// ノードで説明できる、という整理のため。付け先はワールド（誰が何を掴んでも
// 効く）で、対象を書いていない（target = -1）ので「掴んだ物」に働く。
ed::EventAssetDesc defaultPickupAsset() {
    ed::EventAssetDesc a;
    a.name = kMouseEventAsset;
    ed::NodeDesc trigger;
    trigger.id = 1;
    trigger.kind = ed::NodeKind::OnGrab;
    trigger.x = 40.0;
    trigger.y = 40.0;
    ed::NodeDesc action;
    action.id = 2;
    action.kind = ed::NodeKind::GrabPull;
    action.x = 300.0;
    action.y = 40.0;
    action.value = 1.0;  // 引き寄せる強さの倍率
    a.nodes.push_back(trigger);
    a.nodes.push_back(action);
    a.wires.push_back({1, 2});
    return a;
}

}  // namespace

void Scene::resetEventsToDefaults() {
    std::vector<ed::EventAssetDesc> assets;
    assets.push_back(defaultPickupAsset());
    editor_.setEventAssets(std::move(assets),
                           std::vector<std::string>{kMouseEventAsset});
    // オブジェクト側の付け先も落とす（アセットが総取っ替えになったので、
    // 残っていると存在しない名前を指し続ける）。
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        for (auto& o : boxes_) o.desc.events.clear();
    }
    resetGraphRuntime();
}

void Scene::detachEventFromObjects(const std::string& name) {
    std::lock_guard<std::mutex> lk(objectsMutex_);
    for (auto& o : boxes_) {
        o.desc.events.erase(
            std::remove(o.desc.events.begin(), o.desc.events.end(), name),
            o.desc.events.end());
    }
}

int Scene::graphTarget(const ed::NodeDesc& node, const GraphContext& ctx) {
    if (node.target >= 0) return node.target;   // 番号を書いてあればそれ
    if (ctx.object >= 0) return ctx.object;     // トリガーが指した物
    return ctx.self;                            // 付いている相手（-1 = 無し）
}

void Scene::rebuildGraphInstances() {
    graphRt_.instances.clear();
    auto findAsset = [this](const std::string& name) {
        for (std::size_t i = 0; i < graphRt_.assets.size(); ++i) {
            if (graphRt_.assets[i].name == name) return i;
        }
        return graphRt_.assets.size();  // 見つからない
    };
    // ワールドに付いているぶん（owner = -1）。
    for (const auto& name : editor_.worldEvents()) {
        const std::size_t a = findAsset(name);
        if (a < graphRt_.assets.size()) {
            graphRt_.instances.push_back({a, -1});
        }
    }
    // オブジェクトに付いているぶん。同じアセットを何個のオブジェクトに
    // 付けてもよく、1 個のオブジェクトに何本付けてもよい。
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        if (!boxes_[i].alive) continue;
        for (const auto& name : boxes_[i].desc.events) {
            const std::size_t a = findAsset(name);
            if (a < graphRt_.assets.size()) {
                graphRt_.instances.push_back({a, int(i)});
            }
        }
    }
}

void Scene::runEventGraph(double dt) {
    // 引っぱり線は毎ステップ消してから、grabPull が引いたぶんだけ点ける
    // （スクリプトが無ければ線も出ない = 何も起きていないことが見える）。
    clearGrabLines();

    // グラフが変わっていたら一覧を取り直す。タイマー等は（アセット名, 付け先,
    // ノード id）で引くので、シミュレート中の編集でも残りの状態は保たれる。
    const std::uint64_t v = editor_.graphVersion();
    if (v != graphRt_.version) {
        graphRt_.version = v;
        graphRt_.assets = editor_.eventAssets();
        rebuildGraphInstances();
    }
    if (graphRt_.instances.empty()) {
        graphRt_.startFired = true;  // 後から足した OnSimStart を発火させない
        return;
    }

    // 接触の走査と掴みの計算は「そのトリガーを使っているアセットが付いて
    // いるときだけ」（どちらもタダではない）。
    bool wantContacts = false;
    bool wantGrabs = false;
    for (const auto& inst : graphRt_.instances) {
        for (const auto& n : graphRt_.assets[inst.asset].nodes) {
            if (n.kind == ed::NodeKind::OnCollision) wantContacts = true;
            if (n.kind == ed::NodeKind::OnGrab) wantGrabs = true;
        }
    }

    // 今ステップの接触をオブジェクト番号のペア（-1 = 地面）に引き直し、
    // 前ステップに無かったものだけを「新しくぶつかった」として残す。NSC では
    // 積まれているだけでも毎ステップ接触が立つので、差分を取らないと乗って
    // いるだけで発火し続ける。
    std::vector<std::pair<int, int>> newPairs;
    if (wantContacts) {
        // physId -> オブジェクト番号。ボディは他にも居る（地面・静的メッシュ・
        // 作り直しで退場した旧ボディ）ので、生きている物だけを引く。
        std::map<std::size_t, int> byPhys;
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            if (boxes_[i].alive && boxes_[i].physId != GameObject::kInvalidId) {
                byPhys[boxes_[i].physId] = int(i);
            }
        }
        std::set<std::pair<int, int>> now;
        for (const auto& pr : physics_.activeContactPairs()) {
            int a = 0, b = 0;
            const auto ia = byPhys.find(pr.first);
            const auto ib = byPhys.find(pr.second);
            if (ia != byPhys.end()) a = ia->second;
            else if (pr.first == groundPhysId_) a = -1;
            else continue;  // 退場済みボディや静的メッシュは対象外
            if (ib != byPhys.end()) b = ib->second;
            else if (pr.second == groundPhysId_) b = -1;
            else continue;
            if (a > b) std::swap(a, b);
            now.insert({a, b});
        }
        // 最初のパスは今の接触を覚えるだけで発火させない。シミュレート開始の
        // 時点で既に触れていたペア（積んである箱・地面の上の箱）まで「新しく
        // ぶつかった」ことになってしまうため。
        if (graphRt_.contactsPrimed) {
            for (const auto& p : now) {
                if (!graphRt_.prevContacts.count(p)) newPairs.push_back(p);
            }
        }
        graphRt_.contactsPrimed = true;
        graphRt_.prevContacts.swap(now);
    }

    // 掴んでいるカメラぶん（カメラごとに違う物を掴める）。
    std::vector<PointerGrab> grabs;
    if (wantGrabs) {
        for (std::size_t c = 0; c < cameras_.size(); ++c) {
            PointerGrab g;
            if (pointerGrab(c, g) && g.valid) grabs.push_back(g);
        }
    }

    // ペア (a, b) がノードに合うか。対象は「番号を書いてあればそれ、無ければ
    // 付いている相手」。どちらも無い（ワールド付け）なら「どのオブジェクト
    // でも」。相手フィルタ（other）は対象でない側に掛かる。合ったときは
    // 対象側の番号を hit に返す（アクションへ渡す文脈になる）。
    auto collisionMatches = [](const ed::NodeDesc& n, int owner, int a, int b,
                               int& hit) {
        const int want = n.target >= 0 ? n.target : owner;
        auto pairOk = [&n, want](int self, int partner) {
            if (want >= 0 && self != want) return false;
            if (want < 0 && self < 0) return false;  // 地面は対象ではない
            if (n.other == -2) return true;
            return partner == n.other;
        };
        if (pairOk(a, b)) { hit = a; return true; }
        if (pairOk(b, a)) { hit = b; return true; }
        return false;
    };

    // ---- 付いているアセットを 1 つずつ走らせる ----
    for (const auto& inst : graphRt_.instances) {
        const ed::EventAssetDesc& asset = graphRt_.assets[inst.asset];
        // 付け先が消えていたら（削除の直後など）このパスは飛ばす。
        if (inst.owner >= 0 &&
            (std::size_t(inst.owner) >= boxes_.size() ||
             !boxes_[std::size_t(inst.owner)].alive)) {
            continue;
        }
        GraphRuntime::InstanceState& st =
            graphRt_.state[{asset.name, inst.owner}];

        for (const auto& n : asset.nodes) {
            if (!ed::nodeIsTrigger(n.kind)) continue;
            // 1 回のステップで複数の文脈が立つことがある（2 台のカメラが
            // 別々の物を掴んでいる・複数のペアが同時に当たった）。
            std::vector<GraphContext> fires;
            GraphContext base;
            base.self = inst.owner;
            switch (n.kind) {
                case ed::NodeKind::OnSimStart:
                    if (!graphRt_.startFired) fires.push_back(base);
                    break;
                case ed::NodeKind::OnTimer: {
                    double& t = st.timers[n.id];
                    t += dt;
                    if (t >= n.seconds) {
                        // 溜まっていても 1 回だけ（重いフレームの後に
                        // 連射しない）。
                        t = std::fmod(t, n.seconds);
                        fires.push_back(base);
                    }
                    break;
                }
                case ed::NodeKind::OnGrab:
                    for (const auto& g : grabs) {
                        const int want = n.target >= 0 ? n.target : inst.owner;
                        if (want >= 0 && g.index != std::size_t(want)) continue;
                        GraphContext c = base;
                        c.object = int(g.index);
                        c.camera = g.camera;
                        c.hasPoint = true;
                        c.px = g.tgtX;
                        c.py = g.tgtY;
                        c.pz = g.tgtZ;
                        fires.push_back(c);
                    }
                    break;
                case ed::NodeKind::OnCollision:
                    for (const auto& p : newPairs) {
                        int hit = -1;
                        if (!collisionMatches(n, inst.owner, p.first, p.second,
                                              hit)) {
                            continue;
                        }
                        GraphContext c = base;
                        c.object = hit;
                        fires.push_back(c);
                    }
                    break;
                default:
                    break;  // アクションは自分からは発火しない
            }
            if (fires.empty()) continue;

            // 発火バッジは「このステップで動いた」の 1 回ぶん（文脈の数だけ
            // 数えると、掴んでいるあいだ 2 倍速で増えて読みにくい）。
            editor_.noteNodeFired(asset.name, n.id);
            for (const auto& w : asset.wires) {
                if (w.from != n.id) continue;
                for (const auto& an : asset.nodes) {
                    if (an.id != w.to) continue;
                    editor_.noteNodeFired(asset.name, an.id);
                    for (const auto& ctx : fires) runGraphAction(an, ctx, dt);
                    break;
                }
            }
        }
    }
    graphRt_.startFired = true;
}

void Scene::runGraphAction(const ed::NodeDesc& n, const GraphContext& ctx,
                           double dt) {
    // オブジェクトを対象にするアクションの相手。番号 → トリガーが渡した物 →
    // 付いている相手、の順で決まる（graphTarget）。
    const int obj = graphTarget(n, ctx);
    auto objectAlive = [this](int index) {
        return index >= 0 && std::size_t(index) < boxes_.size() &&
               boxes_[std::size_t(index)].alive;
    };

    switch (n.kind) {
        case ed::NodeKind::SetColor: {
            if (!objectAlive(obj)) return;
            // 実行時の上書きだけ。desc.color（設計値）は触らない - 停止で
            // resetGraphRuntime が元の色へ戻す。glTF インスタンス描画の
            // オブジェクトには効かない（個別のベース色を持てないため）。
            std::lock_guard<std::mutex> lk(objectsMutex_);
            GameObject& o = boxes_[std::size_t(obj)];
            o.runtimeColor = n.color;
            o.hasRuntimeColor = true;
            o.colorDirty = true;
            return;
        }
        case ed::NodeKind::ApplyImpulse: {
            if (!objectAlive(obj) || dt <= 0.0) return;
            const GameObject& o = boxes_[std::size_t(obj)];
            if (o.physId == GameObject::kInvalidId) return;
            // applyForce は v += F*dt/m なので、F = m*Δv/dt でちょうど vec
            // ぶん速度が変わる（レート非依存）。
            const double m = physics_.bodyMass(o.physId);
            if (m <= 0.0) return;
            physics_.applyForce(
                o.physId,
                chrono::ChVector3d(n.vec.x * m / dt, n.vec.y * m / dt,
                                   n.vec.z * m / dt),
                dt);
            return;
        }
        case ed::NodeKind::SetFixed: {
            if (!objectAlive(obj)) return;
            const GameObject& o = boxes_[std::size_t(obj)];
            if (o.physId == GameObject::kInvalidId) return;
            physics_.setBodyFixed(o.physId, n.value != 0.0);
            // 設計値（desc.fixed）は変えない。停止時に戻す対象として記録。
            graphRt_.fixedTouched.insert(std::size_t(obj));
            return;
        }
        case ed::NodeKind::GrabPull: {
            // マウスで掴んでいる物をカーソルへ引き寄せる。文脈にカーソルの
            // 点が無い（onGrab 以外から繋がれた）ときは何もしない。
            if (!ctx.hasPoint || !objectAlive(obj) || dt <= 0.0) return;
            const GameObject& o = boxes_[std::size_t(obj)];
            if (o.physId == GameObject::kInvalidId) return;
            const double mass = physics_.bodyMass(o.physId);
            if (mass <= 0.0) return;

            // サーボ: F = m * (kp*e - kd*v)、加速度は上限で頭打ち。毎ステップ
            // 掛けるからこそ追従する（1 発の力積では摩擦ですぐ死ぬ）。ばね
            // 定数は BoxController の設定（SceneConfig.h）で、ノードの value は
            // その倍率。
            const BoxController::Config& cfg =
                controllers_[ctx.camera < controllers_.size() ? ctx.camera : 0]
                    ->config();
            const double gain = n.value > 0.0 ? n.value : 1.0;
            const BodyTransform tr = physics_.bodyTransform(o.physId);
            const scenemath::Vec3 objPos(tr.px, tr.py, tr.pz);
            const scenemath::Vec3 target(ctx.px, ctx.py, ctx.pz);
            const chrono::ChVector3d cv = physics_.bodyVelocity(o.physId);
            const scenemath::Vec3 vel(cv.x(), cv.y(), cv.z());
            scenemath::Vec3 accel = cfg.stiffness * gain * (target - objPos) -
                                    cfg.damping * vel;
            const double a = accel.norm();
            const double maxA = cfg.maxAcceleration * gain;
            if (a > maxA) accel *= maxA / a;
            const scenemath::Vec3 force = accel * mass;
            physics_.applyForce(
                o.physId,
                chrono::ChVector3d(force.x(), force.y(), force.z()), dt);

            // 引っぱり線（物 → カーソルの点）。シーンの中の線なので、どの
            // カメラから見ても同じフレームに乗る。
            setGrabLine(ctx.camera, true, objPos.x(), objPos.y(), objPos.z(),
                        target.x(), target.y(), target.z());
            return;
        }
        case ed::NodeKind::SetLightColor:
        case ed::NodeKind::SetLightIntensity: {
            // ライトとカメラは「付いている相手」を持たない（付け先は
            // オブジェクトかワールドだけ）ので、番号は必ず明示させる。
            if (n.target < 0 || std::size_t(n.target) >= lights_.size() ||
                !lights_[std::size_t(n.target)].alive) {
                return;
            }
            std::lock_guard<std::mutex> lk(objectsMutex_);
            LightItem& l = lights_[std::size_t(n.target)];
            if (n.kind == ed::NodeKind::SetLightColor) {
                l.runtimeColor = n.color;
                l.hasRuntimeColor = true;
            } else {
                l.runtimeIntensity = n.value;
                l.hasRuntimeIntensity = true;
            }
            l.stateDirty = true;
            return;
        }
        case ed::NodeKind::CameraLookAt: {
            if (n.target < 0 || std::size_t(n.target) >= cameras_.size() ||
                !cameraActive(std::size_t(n.target))) {
                return;
            }
            // 注視先は other。書いていなければ文脈の物（触れた物・掴んだ物、
            // または付いている相手）を見る。
            const int look = n.other >= 0 ? n.other
                                          : (ctx.object >= 0 ? ctx.object
                                                             : ctx.self);
            if (!objectAlive(look)) return;
            const GameObject& o = boxes_[std::size_t(look)];
            if (o.physId == GameObject::kInvalidId) return;
            // 注視点だけ動かす（視点は保つ）。CameraObject は atomic なので
            // 物理スレッドから書いてよい。ユーザーのカメラ操作と同じ口。
            const BodyTransform tr = physics_.bodyTransform(o.physId);
            cameras_[std::size_t(n.target)]->setTarget(tr.px, tr.py, tr.pz);
            return;
        }
        default:
            return;  // トリガーに in は無い（EditorState が張らせない）
    }
}

void Scene::resetGraphRuntime() {
    graphRt_.version = ~std::uint64_t(0);  // 次のステップで取り込み直す
    graphRt_.assets.clear();
    graphRt_.instances.clear();
    graphRt_.state.clear();
    graphRt_.startFired = false;
    graphRt_.prevContacts.clear();
    graphRt_.contactsPrimed = false;
    editor_.clearNodeFireCounts();
    clearGrabLines();

    // SetFixed が触ったオブジェクトだけ設計値の固定状態へ戻す。
    for (const std::size_t i : graphRt_.fixedTouched) {
        if (i < boxes_.size() && boxes_[i].alive &&
            boxes_[i].physId != GameObject::kInvalidId) {
            physics_.setBodyFixed(boxes_[i].physId, boxes_[i].desc.fixed);
        }
    }
    graphRt_.fixedTouched.clear();

    // 色・ライトの実行時上書きを捨て、dirty で実体へ設計値を再送させる。
    std::lock_guard<std::mutex> lk(objectsMutex_);
    for (auto& o : boxes_) {
        if (o.hasRuntimeColor) {
            o.hasRuntimeColor = false;
            o.colorDirty = true;
        }
    }
    for (auto& l : lights_) {
        if (l.hasRuntimeColor || l.hasRuntimeIntensity) {
            l.hasRuntimeColor = false;
            l.hasRuntimeIntensity = false;
            l.stateDirty = true;
        }
    }
}

void Scene::pruneGraphForRemoved(ed::NodeTargetKind kind, int index) {
    // 対象そのものが消えたノードは削除（ワイヤーは removeGraphNode が一緒に
    // 落とす）。OnCollision の相手フィルタだけが消えた場合は「何でも」(-2) に
    // 戻して生かす。残すと「対象が無い」まま黙って動かないだけになる - 消えた
    // ことが見えるほうがよい、というジョイントと同じ判断。
    //
    // 番号を書いていないノード（target = -1 = 付いている相手）は触らない:
    // 付け先が消えたのならアセットの実体ごと居なくなるだけで、アセット自体は
    // 他の相手にも付けられる部品として残る。
    for (const auto& asset : editor_.eventAssets()) {
        for (const auto& n : asset.nodes) {
            const bool targetGone =
                ed::nodeTargetKind(n.kind) == kind && n.target == index;
            const bool otherGone = kind == ed::NodeTargetKind::Object &&
                                   ed::nodeOtherIsObject(n.kind) &&
                                   n.other == index;
            if (targetGone) {
                editor_.removeGraphNode(asset.name, n.id);
            } else if (otherGone) {
                if (n.kind == ed::NodeKind::OnCollision) {
                    editor_.updateGraphNode(asset.name, n.id, {{"other", -2}});
                } else {
                    // 注視先の無い LookAt は「文脈の物を見る」に落とす
                    // （番号を書かない = 触れた物 / 掴んだ物）。
                    editor_.updateGraphNode(asset.name, n.id, {{"other", -1}});
                }
            }
        }
    }
}

void Scene::enterMode(ed::AppMode target) {
    if (target == editor_.mode()) return;
    // どちら向きの遷移でもイベントの実行状態は捨てる: 開始はまっさらから
    // （タイマー・発火カウントのリセット）、停止は実行時の上書き（色・
    // ライト・固定）を設計値へ戻すため。
    resetGraphRuntime();
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

// 名前からメッシュアセット番号を引く（-1 = 無い）。
int Scene::meshIndexFor(const std::string& name) const {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < meshes_.size(); ++i) {
        if (meshes_[i].desc.name == name) return int(i);
    }
    return -1;
}

// メッシュの凸包。最初に使うときに読み込む（ファイル IO と cgltf の CPU
// 処理だけなので PHYSICS スレッドでよい - Filament は触らない）。読めなければ
// nullptr = 呼び出し側が球へ倒す。
const std::vector<ChVector3d>* Scene::meshHull(int meshIndex) {
    if (meshIndex < 0 || std::size_t(meshIndex) >= meshes_.size()) {
        return nullptr;
    }
    MeshAsset& m = meshes_[std::size_t(meshIndex)];
    if (!m.hullTried) {
        m.hullTried = true;
        m.hull =
            wizengine::loadCollisionPoints(m.desc.file, float(m.desc.scale));
        if (m.hull.empty()) {
            LOGW("scene",
                 "mesh '%s' (%s): convex hull unavailable - bodies fall back "
                 "to spheres",
                 m.desc.name.c_str(), m.desc.file.c_str());
        }
    }
    return m.hull.empty() ? nullptr : &m.hull;
}

// 設計値から Chrono のボディを 1 個作る（createObject / rebuildBody 共通）。
// 当たり判定の Model は「メッシュの凸包」で、点群が無ければ球へ。
std::size_t Scene::createBody(const ed::BodyDesc& desc, int meshIndex) {
    const ChVector3d pos(desc.position.x, desc.position.y, desc.position.z);
    const ChQuaternion<> rot = quatFromEuler(desc.rotation);
    if (desc.collision == ed::ShapeKind::Model) {
        if (const auto* hull = meshHull(meshIndex)) {
            const std::size_t physId =
                physics_.addConvexHull(*hull, desc.density(), pos, rot);
            if (desc.fixed) physics_.setBodyFixed(physId, true);
            return physId;
        }
    }
    if (desc.collision == ed::ShapeKind::Box) {
        return physics_.addBox(desc.size.x, desc.size.y, desc.size.z,
                               desc.density(), pos, rot, desc.fixed);
    }
    return physics_.addSphere(desc.size.x * 0.5, desc.density(), pos, rot,
                              desc.fixed);
}

std::size_t Scene::createObject(const ed::BodyDesc& descIn) {
    const ed::BodyDesc desc = ed::clampBody(descIn);

    // 描画するメッシュ。見つからない名前は組み込みの球で描く。desc は
    // 書き換えない - 文書としては参照を保つので、アセットを足して読み直せば
    // そのまま直る。
    int meshIndex = -1;
    if (desc.shape == ed::ShapeKind::Model) {
        meshIndex = meshIndexFor(desc.mesh);
        if (meshIndex < 0) {
            LOGW("scene",
                 "object '%s': mesh '%s' is not declared - drawing a sphere",
                 desc.name.c_str(), desc.mesh.c_str());
        }
    }

    GameObject obj;
    obj.physId = createBody(desc, meshIndex);
    obj.meshIndex = meshIndex;
    obj.desc = desc;
    obj.alive = true;
    obj.colorDirty = true;
    // renderId は RENDER スレッドが syncRenderables で作る（モデルも組み込みも）。

    // イベントの実行側は「付いているアセット」の一覧を版番号で取り直す。
    // オブジェクトが増減すると付け先も変わるので、ここで版を進める。
    editor_.bumpGraphVersion();

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

    // このオブジェクトを見張る / 動かすイベントノードも一緒に掃除する
    // （付いていたアセットは desc ごと残るが、実体の一覧からは外れる）。
    pruneGraphForRemoved(ed::NodeTargetKind::Object, int(index));
    editor_.bumpGraphVersion();

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

    const std::size_t physId = createBody(obj.desc, obj.meshIndex);

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
    // このライトを動かすイベントノードも一緒に掃除する。
    pruneGraphForRemoved(ed::NodeTargetKind::Light, int(index));
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
    for (const auto& desc : lightConfigs()) {
        LightItem item;
        item.desc = desc;
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

// ---- 地面と環境光（シーン文書の <ground> / <environment>）------------------

void Scene::rebuildGroundBody() {
    // 古い床は退場（disableBody = 当たり判定を切って地面の下へ。Multicore の
    // ボディ削除は危ないので、オブジェクトの作り直しと同じ流儀）。
    if (groundPhysId_ != GameObject::kInvalidId) {
        physics_.disableBody(groundPhysId_);
    }
    // 番号を覚えておくのは、ジョイントの「ワールド側」に使うため
    // （buildJoints はシミュレート開始のたびに読み直すのでずれない）。
    groundPhysId_ = physics_.addBox(ground_.half * 2.0, 1.0, ground_.half * 2.0,
                                    1000.0, ChVector3d(0, -0.5, 0), QUNIT,
                                    /*fixed*/ true);
}

void Scene::setGroundAndEnvironment(const ed::GroundDesc& ground,
                                    const ed::EnvironmentDesc& env) {
    bool groundBodyChanged = false;
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        const bool groundChanged =
            ground.half != ground_.half ||
            ground.visualHalf != ground_.visualHalf ||
            ground.texture != ground_.texture || ground.tile != ground_.tile ||
            ground.tint.r != ground_.tint.r || ground.tint.g != ground_.tint.g ||
            ground.tint.b != ground_.tint.b;
        groundBodyChanged = ground.half != ground_.half;
        if (groundChanged) {
            ground_ = ground;
            groundDirty_ = true;  // 見た目は RENDER スレッドが作り直す
        }
        // 環境光は差し替えが重い（HDR デコード + GPU プリフィルタ）ので、
        // 実際に変わったときだけ dirty を立てる。
        if (env.hdr != environment_.hdr ||
            env.intensity != environment_.intensity) {
            environment_ = env;
            envDirty_ = true;
        }
    }
    if (groundBodyChanged) rebuildGroundBody();
}

// RENDER スレッド（applyToRenderer のロック中）。
void Scene::syncGround() {
    if (!groundDirty_) return;
    groundDirty_ = false;
    renderer_.addGround(float(ground_.visualHalf),
                        {ground_.tint.r, ground_.tint.g, ground_.tint.b},
                        float(ground_.tile), ground_.texture);
}

// RENDER スレッド。実体の無いライトを作り、消されたもの・作り直しが要る
// ものを片付ける。applyToRenderer からロック済みで呼ばれる。
void Scene::syncLights() {
    // イベントグラフの実行時上書き（色・強さ）は、実体へ流す直前に desc へ
    // 重ねる。desc そのものは設計値のままなので、停止時に上書きを落とせば
    // 元のライトに戻る。
    auto lightForRender = [](const LightItem& l) {
        wizengine::LightDesc r = toRendererLight(l.desc);
        if (l.hasRuntimeColor) {
            r.color = {l.runtimeColor.r, l.runtimeColor.g, l.runtimeColor.b};
        }
        if (l.hasRuntimeIntensity) r.intensity = float(l.runtimeIntensity);
        return r;
    };
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
            l.renderId = renderer_.addLight(lightForRender(l));
            l.stateDirty = false;
            continue;
        }
        if (l.stateDirty) {
            const wizengine::LightDesc r = lightForRender(l);
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

    // ---- イベントアセット --------------------------------------------------
    // ノードとワイヤーの操作は「どのアセットに対してか」を必ず伴う。
    // EditorComponent が名前を正規化して積むので、ここでは存在だけ見る。
    if (op.kind == "event.add") {
        const std::string name = a.value("name", "");
        if (name.empty()) return;
        if (!editor_.addEventAsset(name)) {
            editor_.setStatus("同じ名前のイベントがあります: " + name);
            return;
        }
        editor_.setStatus("イベントを作成: " + name);
        return;
    }

    if (op.kind == "event.remove") {
        const std::string name = a.value("name", "");
        if (name.empty()) return;
        // 付いている先（オブジェクト・ワールド）から先に外す。外し忘れると
        // 存在しない名前を指したままになる。
        detachEventFromObjects(name);
        if (!editor_.removeEventAsset(name)) return;
        // 実行中でも版が進めば次のステップで実体の一覧が作り直される。
        // 実行状態を丸ごと捨てない（resetGraphRuntime を呼ばない）のは、
        // それをすると走っている最中に OnSimStart がもう一度発火するため。
        editor_.bumpGraphVersion();
        editor_.setStatus("イベントを削除: " + name);
        return;
    }

    if (op.kind == "event.attach" || op.kind == "event.detach") {
        const std::string name = a.value("name", "");
        const int target = ed::jsonInt(a, "target", ed::kEventOwnerWorld);
        if (name.empty()) return;
        const bool attach = op.kind == "event.attach";
        const std::string what = attach ? "を付けました" : "を外しました";
        if (target < 0) {  // ワールド（シーン全体）
            const bool ok = attach ? editor_.attachWorldEvent(name)
                                   : editor_.detachWorldEvent(name);
            if (ok) editor_.setStatus("World に " + name + what);
            return;
        }
        if (std::size_t(target) >= boxes_.size() ||
            !boxes_[std::size_t(target)].alive) {
            return;
        }
        if (attach && !editor_.hasEventAsset(name)) return;  // 無いものは付けない
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            auto& list = boxes_[std::size_t(target)].desc.events;
            const auto it = std::find(list.begin(), list.end(), name);
            if (attach) {
                if (it != list.end()) return;  // 二重付けは意味が無い
                list.push_back(name);
            } else {
                if (it == list.end()) return;
                list.erase(it);
            }
        }
        editor_.bumpGraphVersion();
        editor_.setStatus("#" + std::to_string(target) + " に " + name + what);
        return;
    }

    if (op.kind == "node.add") {
        const std::string asset = a.value("asset", "");
        const ed::NodeDesc n = ed::clampNode(ed::nodeFromJson(a, ed::NodeDesc{}));
        const int id = editor_.addGraphNode(asset, n);
        if (id < 0) {
            editor_.setStatus("イベントが見つかりません: " + asset);
            return;
        }
        editor_.setStatus("ノードを追加: " +
                          std::string(ed::nodeKindName(n.kind)) + " #" +
                          std::to_string(id) + "（" + asset + "）");
        return;
    }

    if (op.kind == "node.set") {
        const std::string asset = a.value("asset", "");
        const int id = ed::jsonInt(a, "id", -1);
        if (id < 0) return;
        // ドラッグ（位置）の連投でも来るので、ステータスは出さない。
        editor_.updateGraphNode(asset, id, a);
        return;
    }

    if (op.kind == "node.remove") {
        const std::string asset = a.value("asset", "");
        const int id = ed::jsonInt(a, "id", -1);
        if (id < 0 || !editor_.removeGraphNode(asset, id)) return;
        editor_.setStatus("ノードを削除: #" + std::to_string(id));
        return;
    }

    if (op.kind == "wire.add") {
        const std::string asset = a.value("asset", "");
        const int from = ed::jsonInt(a, "from", -1);
        const int to = ed::jsonInt(a, "to", -1);
        if (editor_.addGraphWire(asset, from, to)) {
            editor_.setStatus("ノードを接続: #" + std::to_string(from) +
                              " → #" + std::to_string(to));
        }
        return;
    }

    if (op.kind == "wire.remove") {
        const std::string asset = a.value("asset", "");
        const int from = ed::jsonInt(a, "from", -1);
        const int to = ed::jsonInt(a, "to", -1);
        if (editor_.removeGraphWire(asset, from, to)) {
            editor_.setStatus("接続を解除: #" + std::to_string(from) + " → #" +
                              std::to_string(to));
        }
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
        // このカメラを動かすイベントノードも一緒に掃除する。
        pruneGraphForRemoved(ed::NodeTargetKind::Camera, index);
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

    if (op.kind == "ground") {
        // 送られてきたキーだけ上書き（edit.sim と同じ部分更新）。パスの検証は
        // groundFromJson が行い、不正なら現状維持になる。
        const ed::GroundDesc g =
            ed::clampGround(ed::groundFromJson(a, ground_));
        setGroundAndEnvironment(g, environment_);
        editor_.setStatus("地面を更新");
        return;
    }

    if (op.kind == "environment") {
        const ed::EnvironmentDesc e =
            ed::clampEnvironment(ed::environmentFromJson(a, environment_));
        setGroundAndEnvironment(ground_, e);
        editor_.setStatus(e.hdr.empty() ? "環境光を更新（環境マップ無し）"
                                        : "環境光を更新");
        return;
    }

    if (op.kind == "xml") {
        // ブラウザの XML エディタから。INPUT スレッドが一度パースを通して
        // いる（壊れた XML はここまで来ない）ので、ここでの失敗は実質無い
        // が、念のため同じ扱いにする。警告は読込と同じくログ + 件数。
        ed::SceneDocument doc;
        std::string error;
        std::vector<std::string> warnings;
        if (!ed::parseXml(a.value("text", std::string()), doc, error,
                          &warnings)) {
            editor_.setStatus("XML が読めません: " + error);
            return;
        }
        for (const auto& w : warnings) {
            LOGW("editor", "xml apply: %s", w.c_str());
        }
        loadDocument(doc);
        // 保存名は触らない: 適用はファイルにしていない編集で、💾 保存して
        // 初めてファイルになる（モーダルの見出しにもそう書いてある）。
        editor_.setStatus(
            warnings.empty()
                ? "XML を適用しました"
                : "XML を適用しました（警告 " +
                      std::to_string(warnings.size()) + " 件 - コンソール参照）");
        return;
    }

    if (op.kind == "clear") {
        physics_.removeAllJoints();
        editor_.setJoints({});
        for (std::size_t i = 0; i < boxes_.size(); ++i) destroyObject(i);
        {
            // メッシュアセットの宣言もシーンの一部。
            std::lock_guard<std::mutex> lk(objectsMutex_);
            meshes_.clear();
        }
        // 地面・環境光も初期値へ（ライト・カメラと同じ扱い）。
        setGroundAndEnvironment(ed::GroundDesc{}, ed::EnvironmentDesc{});
        for (auto& c : controllers_) c->setSelected(BoxController::kNone);
        // ライトとカメラも初期状態へ（真っ暗なシーンから始めさせない）。
        // イベントアセットも同じ扱いで既定へ - マウスで掴んでも何も起きない
        // シーンから始めさせない（resetEventsToDefaults が実行状態も捨てる）。
        resetLightsToDefaults();
        resetCamerasToDefaults();
        resetEventsToDefaults();
        editor_.clearSel();
        editor_.setSceneFile("");
        editor_.setStatus("シーンを空にしました");
        snapshot();
        return;
    }

    if (op.kind == "save") {
        // 名前はここで一度だけ正規化し、以後（ファイル名・文書の model・
        // sceneFile 表示）は同じ文字列を使う。生の名前を残すと、一覧
        // （ファイル名の語幹 = 正規化後）と突き合わせるタイルの選択表示が
        // 外れる。
        const std::string name =
            EditorState::sanitizeSceneName(a.value("name", std::string()));
        const std::string path = EditorState::scenePath(name);
        if (path.empty()) {
            editor_.setStatus("保存名が不正です（英数字と _ - のみ）");
            return;
        }
        ed::SceneDocument doc = document();
        doc.model = name;  // <wizengine model="..."> に出る名前
        std::string reason;
        if (EditorState::writeText(path, ed::toXmlText(doc), reason)) {
            editor_.setSceneFile(name);
            editor_.setStatus("保存しました: " + name + ".xml");
        } else {
            editor_.setStatus("保存できません: " + reason);
        }
        editor_.refreshSceneFiles();
        return;
    }

    if (op.kind == "load") {
        const std::string name =
            EditorState::sanitizeSceneName(a.value("name", std::string()));
        ed::SceneDocument doc;
        std::string reason;
        std::size_t warnCount = 0;
        if (!readSceneDocument(name, doc, reason, &warnCount)) {
            editor_.setStatus("読み込めません: " +
                              (reason.empty() ? name : reason));
            return;
        }
        loadDocument(doc);
        editor_.setSceneFile(name);
        editor_.setStatus(
            warnCount == 0
                ? "読み込みました: " + name
                : "読み込みました: " + name + "（警告 " +
                      std::to_string(warnCount) + " 件 - コンソール参照）");
        editor_.refreshSceneFiles();
        return;
    }

    LOGW("editor", "unknown edit operation '%s'", op.kind.c_str());
}

// シーンの今の中身を文書にする。保存されるのはこれを XML にしたもので、
// ブラウザ API（/scene の JSON）とは別物 - あちらは「今どう見えているか」、
// こちらは「何を設計したか」。オブジェクト一覧のロックを取るので HTTP
// スレッドから呼んでもよい（ロック順は objects -> editor）。
ed::SceneDocument Scene::document() {
    std::lock_guard<std::mutex> lk(objectsMutex_);

    ed::SceneDocument doc;
    doc.model = editor_.sceneFile();
    doc.sim = editor_.sim();
    doc.hasSim = true;

    // メッシュアセット（<asset> 節）。オブジェクトから参照されていなくても
    // 宣言は文書の一部としてそのまま書く（作業途中のシーンで消えると困る）。
    for (const auto& m : meshes_) doc.meshes.push_back(m.desc);

    doc.ground = ground_;
    doc.hasGround = true;
    doc.environment = environment_;
    doc.hasEnvironment = true;

    // ライト（削除済みは詰める。イベントノードがライト番号を参照するので、
    // オブジェクトと同じく詰めた先への対応表を持つ）。
    std::vector<int> lightRemap(lights_.size(), -1);
    {
        int nextLight = 0;
        for (std::size_t i = 0; i < lights_.size(); ++i) {
            if (!lights_[i].alive) continue;
            lightRemap[i] = nextLight++;
            doc.lights.push_back(lights_[i].desc);
        }
    }
    doc.hasLights = true;

    // カメラ（全スロット、姿勢と有効フラグ。スロット番号 = 配列位置）。
    for (std::size_t i = 0; i < cameras_.size(); ++i) {
        ed::CameraPose p;
        p.azimuth = cameras_[i]->azimuth();
        p.elevation = cameras_[i]->elevation();
        p.radius = cameras_[i]->radius();
        const auto t = cameras_[i]->target();
        p.target = {t.x, t.y, t.z};
        p.active = camerasActive_[i] != 0;
        doc.cameras.push_back(p);
    }
    doc.hasCameras = true;

    // オブジェクト。保存では番号を詰めるので、ジョイントとイベントノードの
    // 参照も詰めた番号へ付け替える。
    std::vector<int> remap(boxes_.size(), -1);
    int next = 0;
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        if (!boxes_[i].alive) continue;
        remap[i] = next++;
        doc.bodies.push_back(boxes_[i].desc);
    }

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
        doc.joints.push_back(copy);
    }

    // ---- イベントアセット --------------------------------------------------
    // 中身（ノード）はそのまま。ノードの対象番号だけ詰めた番号へ付け替える
    // （オブジェクト / ライト。カメラはスロット固定なのでそのまま）。対象が
    // 消えているノードは pruneGraphForRemoved が落としているはずだが、二重の
    // 安全でここでも弾き、落ちたノードに繋がるワイヤーも書かない。
    {
        auto fixObj = [&remap](int& ref) {
            if (ref < 0) return true;  // -1 / -2 の意味（自分・何でも）は保つ
            if (std::size_t(ref) >= remap.size() || remap[std::size_t(ref)] < 0)
                return false;
            ref = remap[std::size_t(ref)];
            return true;
        };
        for (const auto& assetIn : editor_.eventAssets()) {
            ed::EventAssetDesc asset;
            asset.name = assetIn.name;
            std::set<int> kept;
            for (const auto& nIn : assetIn.nodes) {
                ed::NodeDesc n = nIn;
                const ed::NodeTargetKind tk = ed::nodeTargetKind(n.kind);
                bool ok = true;
                if (tk == ed::NodeTargetKind::Object) ok = fixObj(n.target);
                if (ok && tk == ed::NodeTargetKind::Light && n.target >= 0) {
                    if (std::size_t(n.target) >= lightRemap.size() ||
                        lightRemap[std::size_t(n.target)] < 0) {
                        ok = false;
                    } else {
                        n.target = lightRemap[std::size_t(n.target)];
                    }
                }
                if (ok && ed::nodeOtherIsObject(n.kind)) {
                    if (n.kind == ed::NodeKind::OnCollision) {
                        if (!fixObj(n.other)) n.other = -2;  // 相手だけ消: 何でも
                    } else if (!fixObj(n.other)) {
                        n.other = -1;  // 注視先だけ消: 文脈の物を見る
                    }
                }
                if (!ok) continue;
                kept.insert(n.id);
                asset.nodes.push_back(n);
            }
            for (const auto& w : assetIn.wires) {
                if (kept.count(w.from) && kept.count(w.to)) {
                    asset.wires.push_back(w);
                }
            }
            doc.eventAssets.push_back(std::move(asset));
        }
        doc.worldEvents = editor_.worldEvents();
        doc.hasEvents = true;
    }
    return doc;
}

std::string Scene::documentXml() {
    return ed::toXmlText(document());
}

void Scene::loadDocument(const ed::SceneDocument& doc) {
    // いま在るものを全部畳んでから作り直す。番号は 0 から振り直されるので、
    // 掴んでいる選択も落とす。
    physics_.removeAllJoints();
    editor_.setJoints({});
    for (std::size_t i = 0; i < boxes_.size(); ++i) destroyObject(i);
    for (auto& c : controllers_) c->setSelected(BoxController::kNone);
    editor_.clearSel();

    // メッシュアセットのカタログを文書のもので置き換える。Renderer 側の
    // 原型はパスでキャッシュされているので、同じファイルを使う文書へ
    // 読み替えても再ロードは起きない（凸包は次に使うときに読み直す）。
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        meshes_.clear();
        for (const auto& m : doc.meshes) {
            MeshAsset a;
            a.desc = m;
            meshes_.push_back(std::move(a));
        }
    }

    // 地面と環境光もシーンの一部。節を持たない文書は既定値へ戻す
    // （ライト・カメラと同じ扱い）。
    setGroundAndEnvironment(
        doc.hasGround ? doc.ground : ed::GroundDesc{},
        doc.hasEnvironment ? doc.environment : ed::EnvironmentDesc{});

    // ライト。文書がライトを 1 つも持たなければ初期構成へ戻す（旧 v1 の保存や、
    // 手で書いた最小の XML）。読み込んだシーンが保存時と同じ見た目になるのが
    // 原則で、ライトを書いていない文書は「指定なし」とみなす。
    // 文書のライト番号は 0 起点で、実際にはこの位置から後ろに足されるので、
    // ライトを参照するイベントノードはこのぶんずらす（オブジェクトの base と
    // 同じ理屈）。
    const std::size_t lightBase = lights_.size();
    if (doc.hasLights) {
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            for (auto& l : lights_) l.alive = false;
        }
        for (const auto& l : doc.lights) createLight(l);
    } else {
        resetLightsToDefaults();
    }

    // カメラ。配列位置 = スロット番号。持たない文書は初期構成へ。
    if (doc.hasCameras) {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        for (std::size_t i = 0; i < cameras_.size(); ++i) {
            if (i >= doc.cameras.size()) { camerasActive_[i] = 0; continue; }
            const ed::CameraPose& p = doc.cameras[i];
            cameras_[i]->setPose(p.azimuth, p.elevation, p.radius);
            cameras_[i]->setTarget(p.target.x, p.target.y, p.target.z);
            camerasActive_[i] = p.active ? 1 : 0;
        }
        camerasActive_[editorCamera()] = 1;  // エディタカメラは常に居る
    } else {
        resetCamerasToDefaults();
    }

    if (doc.hasSim) {
        editor_.setSim(ed::clampSim(doc.sim));
        applySimSettings();
    }

    const std::size_t base = boxes_.size();  // 追加ぶんの先頭番号
    for (const auto& b : doc.bodies) createObject(ed::clampBody(b));

    std::vector<ed::JointDesc> joints;
    for (const auto& jIn : doc.joints) {
        ed::JointDesc j = jIn;
        // 文書の中では 0 起点。実際の番号は既存ぶんだけずれる。
        if (j.bodyA >= 0) j.bodyA += int(base);
        if (j.bodyB >= 0) j.bodyB += int(base);
        joints.push_back(j);
    }
    editor_.setJoints(std::move(joints));

    // イベントアセット。中身のノードの対象番号は、ジョイントと同じく今回
    // 足されたぶんの先頭（base / lightBase）だけずらす（カメラはスロット
    // 番号なのでそのまま）。付け先はオブジェクト側が BodyDesc::events
    // （createObject が desc ごと持ち込み済み）、ワールド側がこの一覧。
    // イベントの節を持たない文書は既定の構成へ - ライトを 1 灯も書かない
    // 文書が初期構成で開くのと同じ扱いで、「掴んでも動かないシーン」で
    // 始めさせないため。
    if (doc.hasEvents) {
        std::vector<ed::EventAssetDesc> assets;
        for (const auto& assetIn : doc.eventAssets) {
            ed::EventAssetDesc asset;
            asset.name = assetIn.name;
            for (const auto& nIn : assetIn.nodes) {
                ed::NodeDesc n = ed::clampNode(nIn);
                const ed::NodeTargetKind tk = ed::nodeTargetKind(n.kind);
                if (tk == ed::NodeTargetKind::Object && n.target >= 0) {
                    n.target += int(base);
                }
                if (tk == ed::NodeTargetKind::Light && n.target >= 0) {
                    n.target += int(lightBase);
                }
                if (ed::nodeOtherIsObject(n.kind) && n.other >= 0) {
                    n.other += int(base);
                }
                asset.nodes.push_back(n);
            }
            asset.wires = assetIn.wires;
            assets.push_back(std::move(asset));
        }
        editor_.setEventAssets(std::move(assets), doc.worldEvents);
        resetGraphRuntime();
    } else {
        resetEventsToDefaults();
    }

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
                // glTF の実体は 1 個だけ壊せないので、隠して空き番号にする
                // （同じモデルの次のオブジェクトが再利用する）。
                renderer_.releaseModelInstance(obj.renderId);
            } else {
                renderer_.removeShape(obj.renderId);
            }
            obj.renderId = GameObject::kInvalidId;
            obj.modelDraw = false;
        }
        obj.renderDirty = false;
        if (!obj.alive) continue;

        if (obj.renderId == GameObject::kInvalidId) {
            // メッシュ指定があればモデルの実体を作る。原型はここで最初に
            // 使うときに読み込む（Filament を触れるのはこのスレッドだけ）。
            // 読めないファイルはシーンを止めず、組み込みの球で描いて警告に
            // 留める - 起動時の一括検証と違い、実行中のシーン読込から来る
            // ため（文書の他の部分は生かす）。
            if (obj.desc.shape == ed::ShapeKind::Model && obj.meshIndex >= 0 &&
                std::size_t(obj.meshIndex) < meshes_.size()) {
                MeshAsset& m = meshes_[std::size_t(obj.meshIndex)];
                if (m.modelId == GameObject::kInvalidId && !m.loadFailed) {
                    try {
                        m.modelId = renderer_.loadModel(m.desc.file);
                        const float raw = renderer_.modelSize(m.modelId);
                        LOGI("scene",
                             "mesh '%s': '%s' (model size %.4f -> %.4f m at "
                             "scale %.3f)",
                             m.desc.name.c_str(), m.desc.file.c_str(), raw,
                             raw * float(m.desc.scale), float(m.desc.scale));
                    } catch (const wizengine::AssetError& e) {
                        m.loadFailed = true;
                        LOGW("scene", "mesh '%s': %s", m.desc.name.c_str(),
                             e.what());
                    }
                }
                if (m.modelId != GameObject::kInvalidId) {
                    obj.renderId = renderer_.addModelInstance(m.modelId);
                    obj.modelDraw = obj.renderId != GameObject::kInvalidId;
                }
            }
            if (obj.renderId == GameObject::kInvalidId) {
                // 組み込みメッシュ（Box / Sphere。読めないモデルも球で代役）。
                obj.renderId = renderer_.addShape(
                    obj.desc.shape == ed::ShapeKind::Box
                        ? wizengine::ShapeMesh::Box
                        : wizengine::ShapeMesh::Sphere);
                obj.modelDraw = false;
            }
            obj.colorDirty = true;
        }
        if (obj.colorDirty && !obj.modelDraw) {
            // イベントグラフ（SetColor）の実行時上書きがあればそちらを描く。
            // 設計値は desc.color のままなので、停止時に落とせば元へ戻る。
            const ed::Color3& c =
                obj.hasRuntimeColor ? obj.runtimeColor : obj.desc.color;
            renderer_.setShapeColor(obj.renderId, {c.r, c.g, c.b});
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
    syncGround();

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
            // モデルの見た目の大きさは <mesh scale>（アセット単位 → m）。
            // 当たり判定の寸法（desc.size）とは独立。
            const float sc =
                (obj.meshIndex >= 0 &&
                 std::size_t(obj.meshIndex) < meshes_.size())
                    ? float(meshes_[std::size_t(obj.meshIndex)].desc.scale)
                    : 1.0f;
            s = filament::math::float3{sc};
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

    // ---- 環境光 -----------------------------------------------------------
    // HDR のデコードと GPU プリフィルタは重い（数十〜数百 ms）ので、
    // オブジェクト一覧のロックを持ったままやらない - 物理スレッドを
    // 止めないため。dirty と desc だけロック中に取り出して、外で適用する。
    ed::EnvironmentDesc envPending;
    bool applyEnv = false;
    if (envDirty_) {
        envDirty_ = false;
        envPending = environment_;
        applyEnv = true;
    }
    lk.unlock();
    if (applyEnv) {
        if (envPending.hdr.empty()) {
            renderer_.clearEnvironment();
            LOGI("scene", "environment: none (flat ambient)");
        } else {
            try {
                renderer_.loadEnvironment(envPending.hdr,
                                          float(envPending.intensity));
            } catch (const wizengine::AssetError& e) {
                // 環境光はシーン文書の内容（手で書ける）なので、読めなくても
                // シーンは止めない。前の環境（または一様アンビエント）のまま。
                LOGW("scene", "environment '%s': %s", envPending.hdr.c_str(),
                     e.what());
            }
        }
    }
}
