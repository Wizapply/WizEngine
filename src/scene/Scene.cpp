// Scene の中核: 構築・物理ステップ・組み込みコンポーネント（カメラ操作と
// マウス操作）・ジョイントの張り直し・描画スレッドへの反映。
// エディタ操作は SceneEdit.cpp、イベントの実行は SceneEvents.cpp、
// 文書との変換は SceneSerialize.cpp。共通の下準備は SceneInternal.h。
#include "scene/SceneInternal.h"
#include "scene/PrefabDefaults.h"

using namespace chrono;
using namespace scene_detail;

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

}  // namespace

float Scene::selectedWhiten() const {
    return kSelectedWhiten;
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
    // プレハブの部品にも当てる（階段の段・車のキャビンをクリックしても
    // その持ち主が選べる）。一覧のコピーは editor のロック（objects より先に
    // 取る = ロック順 objects → editor を守るため、objects を取る前に済ます）。
    const std::vector<ed::PrefabDesc> prefabs = editor_.prefabAssets();

    std::unique_lock<std::mutex> lk(objectsMutex_);
    std::size_t best = BoxController::kNone;
    double bestT = 0.0;
    for (std::size_t i = 0; i < boxes_.size() && i < poses.size(); ++i) {
        if (!boxes_[i].alive) continue;
        const ed::BodyDesc& d = boxes_[i].desc;
        const BodyTransform t = poses[i];
        auto consider = [&](double hit) {
            if (hit < 0.0) return;
            if (best == BoxController::kNone || hit < bestT) {
                best = i;
                bestT = hit;
            }
        };
        if (d.shape == ed::ShapeKind::Box) {
            consider(scenemath::rayHitsBox(
                basis.eye, dir, t,
                scenemath::Vec3(d.size.x * 0.5, d.size.y * 0.5, d.size.z * 0.5)));
        } else if (d.shape != ed::ShapeKind::None) {
            consider(scenemath::rayHitsSphere(basis.eye, dir, t, d.radius()));
        }
        // 部品（車体に固定のものだけ。車輪に付く部品は走行中に動くので外す）。
        // 付いていない車両は組み込みの見た目（builtinCarPrefab）が相手。
        const ed::PrefabDesc* prefab = nullptr;
        ed::PrefabDesc builtin;
        if (!d.prefab.empty()) {
            for (const auto& p : prefabs) {
                if (p.name == d.prefab) prefab = &p;
            }
        } else if (d.hasVehicle) {
            builtin = ed::builtinCarPrefab(d);
            prefab = &builtin;
        }
        if (!prefab) continue;
        const scenemath::Quat rot(t.qw, t.qx, t.qy, t.qz);
        const scenemath::Vec3 pos(t.px, t.py, t.pz);
        for (const ed::PartDesc& part : prefab->parts) {
            if (!part.socket.empty() || part.kind == ed::PartKind::Mesh) continue;
            const scenemath::Vec3 wp =
                pos + rot * scenemath::Vec3(part.position.x, part.position.y, part.position.z);
            const scenemath::Quat wq =
                rot * scenemath::quatFromEulerDegrees(part.rotation.x, part.rotation.y,
                                                      part.rotation.z);
            BodyTransform pt;
            pt.px = wp.x();
            pt.py = wp.y();
            pt.pz = wp.z();
            pt.qw = wq.w();
            pt.qx = wq.x();
            pt.qy = wq.y();
            pt.qz = wq.z();
            if (part.kind == ed::PartKind::Sphere) {
                consider(scenemath::rayHitsSphere(basis.eye, dir, pt, part.size.x * 0.5));
            } else if (part.kind == ed::PartKind::Cylinder) {
                // 軸 X の円柱は長さ x・直径 y の箱で近似（選択判定なので十分）。
                consider(scenemath::rayHitsBox(
                    basis.eye, dir, pt,
                    scenemath::Vec3(part.size.x * 0.5, part.size.y * 0.5, part.size.y * 0.5)));
            } else {
                consider(scenemath::rayHitsBox(
                    basis.eye, dir, pt,
                    scenemath::Vec3(part.size.x * 0.5, part.size.y * 0.5, part.size.z * 0.5)));
            }
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
    // ソフトボディの拡縮は粒子の作り直しなので、ギズモのドラッグが落ち着く
    // まで待ってからまとめて反映する（resizeObject がタイマーを積む）。
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        GameObject& obj = boxes_[i];
        if (!obj.alive || obj.softRebuildTimer <= 0.0) continue;
        obj.softRebuildTimer -= dt;
        if (obj.softRebuildTimer <= 0.0 && obj.physDirty) rebuildBody(i);
    }
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
    if (target == ed::AppMode::Simulate) {
        editor_.clearSel();
        editor_.setPrefabEditObject(-1);  // プレハブ編集はエディタの仕事
    }
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

std::size_t Scene::meshModelId(int meshIndex) {
    if (meshIndex < 0 || std::size_t(meshIndex) >= meshes_.size()) {
        return GameObject::kInvalidId;
    }
    MeshAsset& m = meshes_[std::size_t(meshIndex)];
    if (m.modelId == GameObject::kInvalidId && !m.loadFailed) {
        try {
            m.modelId = renderer_.loadModel(m.desc.file);
            LOGI("scene", "mesh '%s': '%s' (loaded for a prefab part)",
                 m.desc.name.c_str(), m.desc.file.c_str());
        } catch (const wizengine::AssetError& e) {
            m.loadFailed = true;
            LOGW("scene", "mesh '%s': %s", m.desc.name.c_str(), e.what());
        }
    }
    return m.modelId;
}

double Scene::meshScale(int meshIndex) const {
    if (meshIndex < 0 || std::size_t(meshIndex) >= meshes_.size()) return 1.0;
    return meshes_[std::size_t(meshIndex)].desc.scale;
}

bool Scene::latestPose(std::size_t index, BodyTransform& out) {
    std::lock_guard<std::mutex> lk(poseMutex_);
    if (index >= latestPoses_.size()) return false;
    out = latestPoses_[index];
    return true;
}

void Scene::snapshot() {
    std::vector<BodyTransform> poses;
    poses.reserve(boxes_.size());
    // ソフトボディの粒子位置も一緒に。オブジェクト番号で引けるように
    // 全オブジェクトぶんの外側の配列を持つ（剛体のぶんは空のまま = 確保
    // しない）。
    std::vector<std::vector<float>> soft(boxes_.size());
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        const GameObject& obj = boxes_[i];
        if (obj.physId == GameObject::kInvalidId) {
            poses.push_back(BodyTransform{0, 0, 0, 1, 0, 0, 0});
            continue;
        }
        // ソフトボディの代表番号なら、粒子群に当てはめた剛体姿勢が返る。
        poses.push_back(physics_.bodyTransform(obj.physId));
        if (obj.alive && obj.lattice) {
            physics_.softParticlePositions(obj.physId, soft[i]);
        }
    }
    std::lock_guard<std::mutex> lk(poseMutex_);
    latestPoses_.swap(poses);
    latestSoft_.swap(soft);
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
        // geom の無いボディ（プレハブの部品だけ）は自分の実体を持たない。
        // 部品は PrefabComponent が描く。
        if (obj.desc.shape == ed::ShapeKind::None && !obj.lattice) continue;

        if (obj.renderId == GameObject::kInvalidId) {
            // ソフトボディ: 表面粒子を結んだ変形メッシュ（頂点は毎フレーム
            // applyToRenderer が粒子のスナップショットから組む）。形状
            // スロットの 1 つなので色・ハイライトは組み込み形状と同じ口。
            if (obj.lattice) {
                obj.renderId = renderer_.addSoftShape(obj.lattice->surface.size(),
                                                      obj.lattice->indices);
                obj.modelDraw = false;
            }
            // メッシュ指定があればモデルの実体を作る。原型はここで最初に
            // 使うときに読み込む（Filament を触れるのはこのスレッドだけ）。
            // 読めないファイルはシーンを止めず、組み込みの球で描いて警告に
            // 留める - 起動時の一括検証と違い、実行中のシーン読込から来る
            // ため（文書の他の部分は生かす）。
            if (obj.renderId == GameObject::kInvalidId &&
                obj.desc.shape == ed::ShapeKind::Model && obj.meshIndex >= 0 &&
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
    std::vector<std::vector<float>> softPts;
    {
        std::lock_guard<std::mutex> pl(poseMutex_);
        poses = latestPoses_;
        softPts = latestSoft_;
    }

    // The cube/sphere meshes are unit-sized; scale them to the object's size
    // when placing. A model gets its own tuning factor instead (the mesh is
    // whatever the artist exported).
    const std::size_t n = std::min(poses.size(), boxes_.size());
    for (std::size_t k = 0; k < n; ++k) {
        GameObject& obj = boxes_[k];
        if (!obj.alive || obj.renderId == GameObject::kInvalidId) continue;
        const ed::BodyDesc& d = obj.desc;
        if (obj.lattice) {
            // ソフトボディ: 粒子のスナップショットから表面を組んで、頂点を
            // ワールド座標のまま流す（姿勢行列は使わない）。スナップショット
            // がまだ無い（作った直後）フレームは前回の頂点のまま。
            if (k < softPts.size() &&
                softPts[k].size() == obj.lattice->particleCount() * 3) {
                wizengine::softlattice::buildSurface(*obj.lattice, softPts[k].data(),
                                                     softVerts_, softNormals_);
                renderer_.setSoftShapeVertices(obj.renderId, softVerts_.data(),
                                               softNormals_.data(),
                                               obj.lattice->surface.size());
            }
            continue;
        }
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
