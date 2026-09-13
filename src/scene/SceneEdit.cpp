// エディタ操作の実装（PHYSICS スレッド）: 剛体の生成・作り直し・削除、
// ライト / カメラ / 地面 / 環境光の編集、キューに積まれた Op の実行、
// プレハブ部品の移動。ブラウザからの入口は EditorComponent → EditorState。
#include "scene/SceneInternal.h"

using namespace chrono;
using namespace scene_detail;

// 設計値から Chrono のボディを 1 個作る（createObject / rebuildBody 共通）。
// 当たり判定の Model は「メッシュの凸包」で、点群が無ければ球へ。
// ソフトボディは粒子の格子（scene/SoftLattice.h）を PhysicsWorld に渡し、
// 代表番号を physId として使う。
// プレハブの collide 部品 → 追加の当たり形状。部品はボディのローカル座標
// なので、Chrono の複合形状のフレームにそのまま渡せる（socket 付きは
// clampPart が collide を落としているので来ない）。
std::vector<PhysicsWorld::ExtraShape> Scene::collisionShapes(const ed::BodyDesc& desc) const {
    std::vector<PhysicsWorld::ExtraShape> out;
    if (desc.prefab.empty() || desc.hasSoft) return out;
    for (const ed::PrefabDesc& p : editor_.prefabAssets()) {
        if (p.name != desc.prefab) continue;
        for (const ed::PartDesc& part : p.parts) {
            if (!part.collide || !part.socket.empty()) continue;
            if (part.kind == ed::PartKind::Mesh) continue;  // メッシュ部品は見た目だけ
            PhysicsWorld::ExtraShape s;
            s.sphere = part.kind == ed::PartKind::Sphere;
            s.cylinder = part.kind == ed::PartKind::Cylinder;  // x = 長さ、y = 直径
            s.size = ChVector3d(part.size.x, part.size.y, part.size.z);
            s.pos = ChVector3d(part.position.x, part.position.y, part.position.z);
            s.rot = quatFromEuler(part.rotation);
            out.push_back(s);
        }
        break;
    }
    return out;
}

void Scene::markPrefabUsersDirty(const std::string& prefab) {
    std::vector<std::size_t> users;
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            if (!boxes_[i].alive || boxes_[i].desc.prefab != prefab) continue;
            boxes_[i].physDirty = true;
            users.push_back(i);
        }
    }
    if (editor_.mode() == ed::AppMode::Simulate) {
        for (const std::size_t i : users) rebuildBody(i);
    }
}

std::size_t Scene::createBody(
    const ed::BodyDesc& desc, int meshIndex,
    std::shared_ptr<const wizengine::softlattice::Lattice>& lattice,
    std::vector<std::size_t>& children, ed::Vec3d& hullCenter) {
    children.clear();
    hullCenter = ed::Vec3d{0.0, 0.0, 0.0};
    const ChVector3d pos(desc.position.x, desc.position.y, desc.position.z);
    const ChQuaternion<> rot = quatFromEuler(desc.rotation);
    lattice.reset();
    if (desc.hasSoft) {
        auto l = std::make_shared<const wizengine::softlattice::Lattice>(
            wizengine::softlattice::build(desc));
        PhysicsWorld::SoftBodySpec spec;
        spec.rest.reserve(l->rest.size());
        for (const auto& r : l->rest) spec.rest.emplace_back(r[0], r[1], r[2]);
        spec.radius = l->radius;
        spec.particleMass = l->particleMass;
        spec.springs.reserve(l->springs.size());
        for (const auto& s : l->springs) {
            PhysicsWorld::SoftBodySpec::Spring sp;
            sp.a = std::size_t(s.a);
            sp.b = std::size_t(s.b);
            sp.rest = s.rest;
            sp.k = s.k;
            sp.c = s.c;
            spec.springs.push_back(sp);
        }
        spec.iterations = desc.soft.iterations;
        spec.fixed = desc.fixed;
        const std::size_t physId = physics_.addSoftBody(spec, pos, rot);
        if (physId != GameObject::kInvalidId) {
            lattice = l;
            return physId;
        }
        LOGW("scene", "object '%s': soft body could not be created - rigid",
             desc.name.c_str());
    }
    // プレハブの collide 部品（階段の段など）。固定の持ち主なら部品ごとに
    // 別の固定ボディにする（「固定の箱を並べた階段」と同じ物理 = Core /
    // Multicore とも実績のある経路。接触ペアは setAlias で持ち主の番号に
    // 寄せる）。動く持ち主なら本体に足す複合形状（部品が一緒に動く）。
    const std::vector<PhysicsWorld::ExtraShape> parts = collisionShapes(desc);
    const bool separate = desc.fixed && !parts.empty();
    const std::vector<PhysicsWorld::ExtraShape> extras =
        separate ? std::vector<PhysicsWorld::ExtraShape>{} : parts;
    // 接触の物性・衝突レイヤ・重力（文書の <geom friction layer gravity ...>）。
    const BodyOptions opt = toBodyOptions(desc);
    std::size_t physId = GameObject::kInvalidId;
    if (desc.collision == ed::ShapeKind::None) {
        // geom の無いボディ: 部品の当たり判定だけを持つフレーム（階段など）。
        physId = physics_.addFrame(desc.mass, pos, rot, desc.fixed, extras, opt);
    } else if (desc.collision == ed::ShapeKind::Trimesh) {
        // 三角メッシュそのまま（凹形状）。読めない・この Chrono に形状が無い
        // ときは凸包へ倒す（さらに無ければ球）。
        if (const auto* tm = meshTrimesh(meshIndex)) {
            physId = physics_.addTriangleMesh(tm->vertices, tm->triangles, desc.mass, pos,
                                              rot, desc.fixed, extras, opt);
        }
        if (physId == GameObject::kInvalidId) {
            if (const auto* hull = meshHull(meshIndex)) {
                ChVector3d center;
                physId = physics_.addConvexHull(*hull, desc.mass, pos, rot, extras,
                                                &center, opt);
                hullCenter = ed::Vec3d{center.x(), center.y(), center.z()};
                if (desc.fixed) physics_.setBodyFixed(physId, true);
            }
        }
    } else if (desc.collision == ed::ShapeKind::Model) {
        if (const auto* hull = meshHull(meshIndex)) {
            // 質量は文書の値そのもの（密度ではない。PhysicsWorld の注記）。
            ChVector3d center;
            physId = physics_.addConvexHull(*hull, desc.mass, pos, rot, extras,
                                            &center, opt);
            hullCenter = ed::Vec3d{center.x(), center.y(), center.z()};
            if (desc.fixed) physics_.setBodyFixed(physId, true);
        }
    }
    if (physId == GameObject::kInvalidId) {
        physId = desc.collision == ed::ShapeKind::Box
                     ? physics_.addBox(desc.size.x, desc.size.y, desc.size.z,
                                       desc.density(), pos, rot, desc.fixed, extras, opt)
                     : physics_.addSphere(desc.size.x * 0.5, desc.density(), pos, rot,
                                          desc.fixed, extras, opt);
    }
    if (separate) {
        for (const PhysicsWorld::ExtraShape& e : parts) {
            const ChVector3d wp = pos + rot.Rotate(e.pos);
            const ChQuaternion<> wr = rot * e.rot;
            std::size_t child = GameObject::kInvalidId;
            if (e.cylinder) {
                // 円柱の部品は単独のボディにできない（addCylinder は無い）ので、
                // 1 部品だけの複合形状を持つフレームとして作る。
                child = physics_.addFrame(1.0, wp, wr, true, {PhysicsWorld::ExtraShape{
                    false, true, e.size, ChVector3d(0, 0, 0), ChQuaternion<>(1, 0, 0, 0)}},
                    opt);
            } else if (e.sphere) {
                child = physics_.addSphere(e.size.x() * 0.5, 1000.0, wp, wr, true, {}, opt);
            } else {
                child = physics_.addBox(e.size.x(), e.size.y(), e.size.z(), 1000.0, wp, wr,
                                        true, {}, opt);
            }
            physics_.setAlias(child, physId);
            children.push_back(child);
        }
    }
    return physId;
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
    obj.physId = createBody(desc, meshIndex, obj.lattice, obj.childPhysIds,
                            obj.hullCenter);
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
    for (const std::size_t c : boxes_[index].childPhysIds) physics_.disableBody(c);
    boxes_[index].childPhysIds.clear();
    // 編集中のプレハブの持ち主が消えたら編集モードも終わる。
    if (editor_.prefabEditObject() == int(index)) {
        editor_.setPrefabEditObject(-1);
        if (editor_.selKind() == EditorState::SelKind::Part) editor_.clearSel();
    }

    // 消えたオブジェクトを参照するジョイントも一緒に落とす。残すと次の
    // シミュレート開始で「端点が無い」と言われ続けるだけ。番号で参照する
    // ノード（onJointBreak / setMotor）も後ろから順に詰め直す。
    auto joints = editor_.joints();
    const int idx = int(index);
    std::vector<int> removedJoints;
    for (std::size_t k = 0; k < joints.size(); ++k) {
        if (joints[k].bodyA == idx || joints[k].bodyB == idx) removedJoints.push_back(int(k));
    }
    if (!removedJoints.empty()) {
        for (auto it = removedJoints.rbegin(); it != removedJoints.rend(); ++it) {
            joints.erase(joints.begin() + *it);
            pruneJointNodes(*it);
        }
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
    rebuildChildren(index);
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
    rebuildChildren(index);
}

// 部品を別ボディで持つ固定の持ち主を動かしたら、部品もその場所へ（作り直し。
// エディタ中は physDirty だけ = シミュレート開始でまとめて、シミュレート中は
// 即）。placeBody は持ち主しか動かさないため。
void Scene::rebuildChildren(std::size_t index) {
    GameObject& obj = boxes_[index];
    if (obj.childPhysIds.empty()) return;
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        obj.physDirty = true;
    }
    if (editor_.mode() == ed::AppMode::Simulate) rebuildBody(index);
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
        // ソフトボディは見た目も粒子から組むので、作り直すまで大きさが
        // 変わらない。ドラッグが落ち着いたら stepEditor が作り直す。
        if (boxes_[index].desc.hasSoft) boxes_[index].softRebuildTimer = 0.35;
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
    for (const std::size_t c : obj.childPhysIds) physics_.disableBody(c);

    std::shared_ptr<const wizengine::softlattice::Lattice> lattice;
    std::vector<std::size_t> children;
    ed::Vec3d hullCenter;
    const std::size_t physId =
        createBody(obj.desc, obj.meshIndex, lattice, children, hullCenter);

    std::lock_guard<std::mutex> lk(objectsMutex_);
    obj.physId = physId;
    obj.childPhysIds = children;
    obj.lattice = lattice;
    obj.hullCenter = hullCenter;
    obj.softRebuildTimer = 0.0;
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
    // ボディ削除は危ないので、オブジェクトの作り直しと同じ流儀）。新しい床は
    // 実行中の追加なので、衝突系への登録は addBox（registerBody →
    // bindCollision）が面倒を見る。
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
            ground.tint.b != ground_.tint.b ||
            ground.roughness != ground_.roughness ||
            ground.metallic != ground_.metallic;
        groundBodyChanged = ground.half != ground_.half;
        if (groundChanged) {
            ground_ = ground;
            groundDirty_ = true;  // 見た目は RENDER スレッドが作り直す
        }
        // 環境光は差し替えが重い（HDR デコード + GPU プリフィルタ）ので、
        // 実際に変わったときだけ dirty を立てる。
        if (env.hdr != environment_.hdr ||
            env.intensity != environment_.intensity ||
            env.skybox != environment_.skybox) {
            environment_ = env;
            envDirty_ = true;
        }
    }
    if (groundBodyChanged) rebuildGroundBody();
}

// 描画設定（文書の <visual>）。実体（Filament の View / Camera /
// ColorGrading）を触るのは RENDER スレッドなので、ここは設計値を置いて
// dirty を立てるだけ - 地面・環境光とまったく同じ扱い。
void Scene::setRenderDesc(const ed::RenderDesc& render) {
    std::lock_guard<std::mutex> lk(objectsMutex_);
    render_ = render;
    renderDirty_ = true;
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
                                  next.color.b != before.color.b ||
                                  next.material != before.material;
        // ソフトボディの切替と設定は格子の作り直し（粒子数・ばねが変わる）。
        const bool softChanged = next.hasSoft != before.hasSoft ||
                                 (next.hasSoft && next.soft != before.soft);
        if (next.hasSoft &&
            (next.shape == ed::ShapeKind::Model || next.shape == ed::ShapeKind::None)) {
            // 格子は箱か球。メッシュ形状・geom 無しのままソフトにはできない。
            next.shape = ed::ShapeKind::Box;
            next.collision = ed::ShapeKind::Box;
            next.mesh.clear();
        }
        // 物性は材質を持っていればその場で、無ければ作り直し（形状に焼き込み）。
        // レイヤは衝突モデルの作り直し。重力と速度はその場で書ける。
        const bool surfaceChanged = next.surface != before.surface;
        const bool layerChanged = next.layer != before.layer ||
                                  next.nocollide != before.nocollide;
        const bool gravityChanged = next.gravity != before.gravity;
        const bool velocityChanged =
            next.velocity.x != before.velocity.x || next.velocity.y != before.velocity.y ||
            next.velocity.z != before.velocity.z ||
            next.angularVelocity.x != before.angularVelocity.x ||
            next.angularVelocity.y != before.angularVelocity.y ||
            next.angularVelocity.z != before.angularVelocity.z;
        bool surfaceNeedsRebuild = false;
        if (surfaceChanged && obj.physId != GameObject::kInvalidId) {
            surfaceNeedsRebuild = !physics_.setBodySurface(obj.physId, toBodyOptions(next));
            for (const std::size_t c : obj.childPhysIds) {
                physics_.setBodySurface(c, toBodyOptions(next));
            }
        }
        if (gravityChanged && obj.physId != GameObject::kInvalidId) {
            physics_.setBodyGravity(obj.physId, next.gravity);
        }
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            obj.desc = next;
            if (colorChanged) obj.colorDirty = true;
            if (shapeChanged || sizeChanged || massChanged || softChanged ||
                layerChanged || surfaceNeedsRebuild) {
                obj.physDirty = true;
            }
            // レンダラブルを作り直すのは、メッシュが別物になるとき（箱↔球）
            // だけ。大きさはスケール行列で毎フレーム効くので作り直さない。
            if (shapeChanged || softChanged) obj.renderDirty = true;
        }
        // ソフトボディは見た目も粒子から組むので、Inspector の変更はその場で
        // 作り直す（スライダーの連投はギズモ側 = resizeObject が遅らせる）。
        const bool softNow = next.hasSoft || before.hasSoft;
        if (obj.physDirty && softNow) rebuildBody(std::size_t(index));
        if (next.fixed != before.fixed && obj.physId != GameObject::kInvalidId) {
            physics_.setBodyFixed(obj.physId, next.fixed);
        }
        if (poseChanged && obj.physId != GameObject::kInvalidId) {
            physics_.placeBody(obj.physId,
                               ChVector3d(next.position.x, next.position.y,
                                          next.position.z),
                               quatFromEuler(next.rotation));
            if (!obj.childPhysIds.empty()) obj.physDirty = true;  // 部品も付いていく
        }
        // 初速はシミュレート中なら即座に効かせる（エディタ中は開始時に
        // restoreAuthoredPoses が与える）。
        if (velocityChanged && editor_.mode() == ed::AppMode::Simulate &&
            obj.physId != GameObject::kInvalidId) {
            const ed::Vec3d& v = next.velocity;
            const ed::Vec3d& w = next.angularVelocity;
            physics_.setBodyVelocity(
                obj.physId, ChVector3d(v.x, v.y, v.z),
                ChVector3d(w.x * kDegToRad, w.y * kDegToRad, w.z * kDegToRad));
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
        pruneJointNodes(index);
        if (editor_.mode() == ed::AppMode::Simulate) buildJoints();
        editor_.setStatus("ジョイントを削除: #" + std::to_string(index));
        return;
    }

    // 既存のジョイントの値を書き換える（送られたキーだけ）。シミュレート中
    // なら全部作り直して即反映（拘束は作り直すしかない）。
    if (op.kind == "joint.set") {
        const int index = a.value("index", -1);
        auto joints = editor_.joints();
        if (index < 0 || std::size_t(index) >= joints.size()) return;
        ed::JointDesc j = ed::jointFromJson(a, joints[std::size_t(index)]);
        if (j.bodyA == j.bodyB) {
            editor_.setStatus("ジョイント: 同じオブジェクト同士は繋げません");
            return;
        }
        joints[std::size_t(index)] = j;
        editor_.setJoints(std::move(joints));
        if (editor_.mode() == ed::AppMode::Simulate) buildJoints();
        editor_.setStatus("ジョイントを更新: #" + std::to_string(index));
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

    // ---- 計算式アセット ------------------------------------------------------
    if (op.kind == "formula.add") {
        const std::string name = a.value("name", "");
        if (name.empty()) return;
        wizengine::vehicle::FormulaGraphDesc g;
        if (a.value("template", "") == "tire") {
            g = wizengine::vehicle::defaultTireFormulaGraph(name);
        } else if (a.value("template", "") == "pressure") {
            g = wizengine::vehicle::defaultPressureFormulaGraph(name);
        } else {
            g.name = name;
        }
        if (!editor_.addFormulaAsset(std::move(g))) {
            editor_.setStatus("同じ名前の計算式があります: " + name);
            return;
        }
        editor_.setStatus("計算式を作成: " + name);
        return;
    }

    if (op.kind == "formula.remove") {
        const std::string name = a.value("name", "");
        if (name.empty() || !editor_.removeFormulaAsset(name)) return;
        // タイヤの参照を外す（消えた名前を指したままにしない = 組み込みへ）。
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            for (auto& o : boxes_) {
                if (!o.desc.hasVehicle) continue;
                for (auto& ax : o.desc.vehicle.axles) {
                    if (ax.tire.formula == name) ax.tire.formula.clear();
                    if (ax.tire.pressureFormula == name) ax.tire.pressureFormula.clear();
                }
            }
        }
        editor_.bumpFormulaVersion();
        editor_.setStatus("計算式を削除: " + name);
        return;
    }

    if (op.kind == "fnode.add") {
        const std::string asset = a.value("asset", "");
        wizengine::vehicle::FormulaNodeDesc n =
            ed::clampFormulaNode(ed::formulaNodeFromJson(a, wizengine::vehicle::FormulaNodeDesc{}));
        n.kind = a.value("type", "");
        const int id = editor_.addFormulaNode(asset, n);
        if (id < 0) {
            editor_.setStatus("計算式が見つかりません: " + asset);
            return;
        }
        editor_.setStatus("ノードを追加: " + n.kind + " #" + std::to_string(id) +
                          "（" + asset + "）");
        return;
    }

    if (op.kind == "fnode.set") {
        const std::string asset = a.value("asset", "");
        const int id = ed::jsonInt(a, "id", -1);
        if (id < 0) return;
        editor_.updateFormulaNode(asset, id, a);  // ドラッグの連投なので無言
        return;
    }

    if (op.kind == "fnode.remove") {
        const std::string asset = a.value("asset", "");
        const int id = ed::jsonInt(a, "id", -1);
        if (id < 0 || !editor_.removeFormulaNode(asset, id)) return;
        editor_.setStatus("ノードを削除: #" + std::to_string(id));
        return;
    }

    if (op.kind == "fwire.add" || op.kind == "fwire.remove") {
        const std::string asset = a.value("asset", "");
        const wizengine::vehicle::FormulaWireDesc w = ed::formulaWireFromJson(a);
        if (op.kind == "fwire.add") {
            if (editor_.addFormulaWire(asset, w)) {
                editor_.setStatus("ノードを接続: #" + std::to_string(w.from) + " → #" +
                                  std::to_string(w.to));
            } else {
                editor_.setStatus("接続できません（ポートが無い・循環になる）");
            }
        } else if (editor_.removeFormulaWire(asset, w)) {
            editor_.setStatus("接続を解除: #" + std::to_string(w.from) + " → #" +
                              std::to_string(w.to));
        }
        return;
    }

    // ---- プレハブ ------------------------------------------------------------
    if (op.kind == "prefab.open") {
        const int index = ed::jsonInt(a, "index", -1);
        if (index < 0 || std::size_t(index) >= boxes_.size() ||
            !boxes_[std::size_t(index)].alive) {
            return;
        }
        GameObject& obj = boxes_[std::size_t(index)];
        if (obj.desc.prefab.empty()) {
            // まだプレハブが無ければ、いま見えている組み込みの見た目から作る
            // （Unity の「プレハブ化」）。名前はオブジェクト名から。
            std::string base = ed::sanitizeEventName(obj.desc.name);
            if (base.empty()) base = "object" + std::to_string(index);
            std::string name = base + "_prefab";
            for (int n = 2; editor_.hasPrefabAsset(name) && n < 1000; ++n) {
                name = base + "_prefab" + std::to_string(n);
            }
            ed::PrefabDesc d = ed::builtinCarPrefab(obj.desc, name);
            editor_.addPrefabAsset(d);
            std::lock_guard<std::mutex> lk(objectsMutex_);
            obj.desc.prefab = name;
        }
        editor_.setPrefabEditObject(index);
        for (auto& c : controllers_) c->setSelected(BoxController::kNone);
        editor_.clearSel();
        editor_.setStatus("プレハブを編集: " + obj.desc.prefab);
        return;
    }

    if (op.kind == "prefab.close") {
        editor_.setPrefabEditObject(-1);
        if (editor_.selKind() == EditorState::SelKind::Part) editor_.clearSel();
        editor_.setStatus("プレハブの編集を終了");
        return;
    }

    if (op.kind == "prefab.attach" || op.kind == "prefab.detach") {
        const int index = ed::jsonInt(a, "index", -1);
        if (index < 0 || std::size_t(index) >= boxes_.size() ||
            !boxes_[std::size_t(index)].alive) {
            return;
        }
        const std::string name = a.value("name", "");
        if (op.kind == "prefab.attach" && !editor_.hasPrefabAsset(name)) {
            editor_.setStatus("プレハブが見つかりません: " + name);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            boxes_[std::size_t(index)].desc.prefab =
                op.kind == "prefab.attach" ? name : std::string();
            // collide 部品のぶん当たり形状が変わる（無くても剛体の作り直しは
            // 安い。エディタ中はシミュレート開始でまとめて）。
            boxes_[std::size_t(index)].physDirty = true;
        }
        if (editor_.mode() == ed::AppMode::Simulate) rebuildBody(std::size_t(index));
        editor_.setStatus("#" + std::to_string(index) +
                          (op.kind == "prefab.attach" ? " に " + name + " を付けました"
                                                       : " のプレハブを外しました"));
        return;
    }

    if (op.kind == "prefab.add") {
        const std::string name = a.value("name", "");
        ed::PrefabDesc d;
        d.name = name;
        // 中身付きで作る口（🪜 Stairs = builtinStairsPrefab）。省略時は空。
        if (a.contains("parts") && a["parts"].is_array()) {
            for (const auto& pj : a["parts"]) {
                if (d.parts.size() >= 256) break;
                ed::PartDesc p = ed::clampPart(ed::partFromJson(pj, ed::PartDesc{}));
                if (p.name.empty()) p.name = std::string(ed::partKindName(p.kind));
                d.parts.push_back(std::move(p));
            }
        }
        if (name.empty() || !editor_.addPrefabAsset(d)) {
            editor_.setStatus("同じ名前のプレハブがあります: " + name);
            return;
        }
        editor_.setStatus("プレハブを作成: " + name +
                          (d.parts.empty() ? "" : "（" + std::to_string(d.parts.size()) + " 部品）"));
        return;
    }

    if (op.kind == "prefab.remove") {
        const std::string name = a.value("name", "");
        if (name.empty() || !editor_.removePrefabAsset(name)) return;
        markPrefabUsersDirty(name);  // 外す前に（名前で探すので）
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            for (auto& o : boxes_) {
                if (o.desc.prefab == name) o.desc.prefab.clear();
            }
        }
        if (editingPrefabName().empty()) {
            editor_.setPrefabEditObject(-1);
            if (editor_.selKind() == EditorState::SelKind::Part) editor_.clearSel();
        }
        editor_.setStatus("プレハブを削除: " + name);
        return;
    }

    if (op.kind == "part.add" || op.kind == "part.set" || op.kind == "part.remove") {
        // 省略時は編集中のプレハブ。
        std::string prefab = a.value("prefab", "");
        if (prefab.empty()) prefab = editingPrefabName();
        if (prefab.empty()) {
            editor_.setStatus("編集中のプレハブがありません");
            return;
        }
        if (op.kind == "part.add") {
            ed::PartDesc p = ed::clampPart(ed::partFromJson(a, ed::PartDesc{}));
            if (p.name.empty()) p.name = std::string(ed::partKindName(p.kind));
            const int idx = editor_.addPart(prefab, p);
            if (idx < 0) return;
            if (p.collide) markPrefabUsersDirty(prefab);
            editor_.setSel(EditorState::SelKind::Part, idx);
            editor_.setStatus("部品を追加: " + p.name);
            return;
        }
        const int part = ed::jsonInt(a, "part", -1);
        // 当たり判定を持つ部品（前後どちらかで collide）が変われば、持ち主の
        // 剛体の複合形状を作り直す。Chrono は形を後から変えられない。
        ed::PartDesc before;
        const bool had = editor_.partDesc(prefab, part, before) && before.collide;
        if (op.kind == "part.set") {
            if (!editor_.updatePart(prefab, part, a)) return;  // 連投なので無言
            ed::PartDesc after;
            const bool has = editor_.partDesc(prefab, part, after) && after.collide;
            if (had || has) markPrefabUsersDirty(prefab);
            return;
        }
        if (editor_.removePart(prefab, part)) {
            if (had) markPrefabUsersDirty(prefab);
            if (editor_.selKind() == EditorState::SelKind::Part) editor_.clearSel();
            editor_.setStatus("部品を削除: #" + std::to_string(part));
        }
        return;
    }

    // ---- 車両 -----------------------------------------------------------------
    if (op.kind == "vehicle.enable" || op.kind == "vehicle.tire") {
        const int index = ed::jsonInt(a, "index", -1);
        if (index < 0 || std::size_t(index) >= boxes_.size() ||
            !boxes_[std::size_t(index)].alive) {
            return;
        }
        GameObject& obj = boxes_[std::size_t(index)];
        std::string status;
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            if (op.kind == "vehicle.enable") {
                const bool on = a.value("on", true);
                obj.desc.hasVehicle = on;
                if (on && obj.desc.vehicle.axles.empty()) {
                    obj.desc.vehicle = wizengine::vehicle::defaultVehicleDesc();
                }
                status = on ? "#" + std::to_string(index) + " を車両にしました"
                            : "#" + std::to_string(index) + " の車両を外しました";
            } else {
                // 軸（-1 = 全軸）のタイヤ設定。送られてきたキーだけ変える:
                // formula（計算式）・soft（ソフトタイヤ）・stiffness（径方向剛性）。
                const int axle = ed::jsonInt(a, "axle", -1);
                const bool hasFormula = a.contains("formula") && a["formula"].is_string();
                const bool hasSoft = a.contains("soft") && a["soft"].is_boolean();
                const bool hasStiffness = a.contains("stiffness") && a["stiffness"].is_number();
                const bool hasPressure = a.contains("pressure") && a["pressure"].is_number();
                const bool hasPressureFormula =
                    a.contains("pressureFormula") && a["pressureFormula"].is_string();
                const std::string formula = hasFormula ? a["formula"].get<std::string>() : "";
                const std::string pressureFormula =
                    hasPressureFormula ? a["pressureFormula"].get<std::string>() : "";
                if (hasFormula && !formula.empty() && !editor_.hasFormulaAsset(formula)) {
                    status = "計算式が見つかりません: " + formula;
                } else if (hasPressureFormula && !pressureFormula.empty() &&
                           !editor_.hasFormulaAsset(pressureFormula)) {
                    status = "計算式が見つかりません: " + pressureFormula;
                } else if (!obj.desc.hasVehicle) {
                    status = "#" + std::to_string(index) + " は車両ではありません";
                } else {
                    auto& axles = obj.desc.vehicle.axles;
                    for (std::size_t i = 0; i < axles.size(); ++i) {
                        if (axle >= 0 && std::size_t(axle) != i) continue;
                        if (hasFormula) axles[i].tire.formula = formula;
                        if (hasSoft) axles[i].tire.soft = a["soft"].get<bool>();
                        if (hasStiffness) {
                            axles[i].tire.stiffness = a["stiffness"].get<double>();
                        }
                        if (hasPressure) axles[i].tire.pressure = a["pressure"].get<double>();
                        if (hasPressureFormula) axles[i].tire.pressureFormula = pressureFormula;
                    }
                    obj.desc.vehicle = wizengine::vehicle::clampVehicle(obj.desc.vehicle);
                    if (hasFormula) {
                        status = "#" + std::to_string(index) + " のタイヤ式: " +
                                 (formula.empty() ? "(組み込み)" : formula);
                    } else if (hasPressureFormula) {
                        status = "#" + std::to_string(index) + " の空気圧式: " +
                                 (pressureFormula.empty() ? "(組み込み・等温変化)"
                                                          : pressureFormula);
                    } else if (hasPressure) {
                        status = "#" + std::to_string(index) + " の空気圧を更新";
                    } else if (hasSoft) {
                        status = "#" + std::to_string(index) + " のタイヤ: " +
                                 (a["soft"].get<bool>() ? "ソフト（変形メッシュ）" : "剛");
                    } else {
                        status = "#" + std::to_string(index) + " のタイヤ剛性を更新";
                    }
                }
            }
        }
        editor_.bumpFormulaVersion();  // 車両モデルを作り直させる
        editor_.setStatus(status);
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

    if (op.kind == "render") {
        // 送られてきたキーだけ上書き（edit.ground と同じ部分更新）。
        // "preset" が入っていれば renderFromJson がそこから組み立てる。
        // 読むときにロックが要らないのは、render_ を書くのがこのスレッド
        // だけだから（地面・環境光と同じ約束）。
        setRenderDesc(ed::renderFromJson(a, render_));
        editor_.setStatus("描画設定を更新");
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
        // 地面・環境光・描画設定も初期値へ（ライト・カメラと同じ扱い）。
        setGroundAndEnvironment(ed::GroundDesc{}, ed::EnvironmentDesc{});
        setRenderDesc(ed::RenderDesc{});
        for (auto& c : controllers_) c->setSelected(BoxController::kNone);
        // ライトとカメラも初期状態へ（真っ暗なシーンから始めさせない）。
        // イベントアセットも同じ扱いで既定へ - マウスで掴んでも何も起きない
        // シーンから始めさせない（resetEventsToDefaults が実行状態も捨てる）。
        resetLightsToDefaults();
        resetCamerasToDefaults();
        resetEventsToDefaults();
        editor_.setFormulaAssets({});  // 計算式もシーンの一部
        editor_.setPrefabAssets({});
        editor_.setPrefabEditObject(-1);
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

std::string Scene::editingPrefabName() const {
    const int obj = editor_.prefabEditObject();
    if (obj < 0 || std::size_t(obj) >= boxes_.size() || !boxes_[std::size_t(obj)].alive) {
        return std::string();
    }
    return boxes_[std::size_t(obj)].desc.prefab;
}

void Scene::movePart(int part, double x, double y, double z) {
    const int obj = editor_.prefabEditObject();
    const std::string prefab = editingPrefabName();
    ed::PartDesc p;
    if (prefab.empty() || !editor_.partDesc(prefab, part, p)) return;
    ed::Vec3d localPos, localRot;
    scenemath::Vec3 wp;
    scenemath::Quat wr;
    prefabframe::partWorld(boxes_[std::size_t(obj)].desc, p, wp, wr);
    prefabframe::worldToLocal(boxes_[std::size_t(obj)].desc, p, scenemath::Vec3(x, y, z), wr,
                              localPos, localRot);
    editor_.setPartTransform(prefab, part, &localPos, nullptr, nullptr);
}

void Scene::rotatePartWorld(int part, double qw, double qx, double qy, double qz) {
    const int obj = editor_.prefabEditObject();
    const std::string prefab = editingPrefabName();
    ed::PartDesc p;
    if (prefab.empty() || !editor_.partDesc(prefab, part, p)) return;
    ed::Vec3d localPos, localRot;
    scenemath::Vec3 wp;
    scenemath::Quat wr;
    prefabframe::partWorld(boxes_[std::size_t(obj)].desc, p, wp, wr);
    prefabframe::worldToLocal(boxes_[std::size_t(obj)].desc, p, wp,
                              scenemath::Quat(qw, qx, qy, qz), localPos, localRot);
    editor_.setPartTransform(prefab, part, nullptr, &localRot, nullptr);
}

void Scene::resizePart(int part, double sx, double sy, double sz) {
    const std::string prefab = editingPrefabName();
    if (prefab.empty()) return;
    const ed::Vec3d size{sx, sy, sz};
    editor_.setPartTransform(prefab, part, nullptr, nullptr, &size);
}
