// マウスの掴み（対象とカーソルの指す点）と、イベントアセットの実行。
// すべて PHYSICS スレッド。グラフ本体は EditorState、ここは実行するだけ。
#include "scene/SceneInternal.h"

using namespace chrono;
using namespace scene_detail;

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
