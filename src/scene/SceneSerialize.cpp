// シーン文書（SceneDocument ⇄ Scene の実体）と、ブラウザ向け階層 JSON。
// 保存は document()、読込は loadDocument()、/scene は hierarchyJson()。
#include "scene/SceneInternal.h"

using namespace chrono;
using namespace scene_detail;

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
                    : selKind == EditorState::SelKind::Part   ? "part"
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
    // いまの物理の系（バックエンド / 接触の解き方）と、Core を要した理由。
    // このビルドが持つ B の機能も送る（UI が選択肢を灰色にする）。
    j["engine"] = physics_.backendName();
    j["contact"] = physics_.contactName();
    j["engineNote"] = backendNote();
    j["solverUsed"] = physics_.solverName();
    {
        const PhysicsFeatures f = physicsFeatures();
        j["features"] = {{"multicore", multicoreAvailable()},
                         {"fea", f.fea},
                         {"loads", f.loads},
                         {"directSolvers", f.directSolvers},
                         {"pardiso", f.pardiso},
                         {"mumps", f.mumps},
                         {"modal", f.modal},
                         {"parsers", wizengine::importAvailable()}};
    }
    j["modal"] = modalFrequencies();
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
    // 描画設定（Inspector の「描画」節）。文書の <visual> と同じ値。
    j["render"] = ed::toJson(render_);
    j["objects"] = nlohmann::json::array();
    j["joints"] = nlohmann::json::array();
    {
        const auto joints = editor_.joints();
        // 計測値（反力・破断）。ロック順 objects → editor → poses のまま。
        const std::vector<JointStat> stats = jointStats();
        for (std::size_t i = 0; i < joints.size(); ++i) {
            nlohmann::json e = ed::toJson(joints[i]);
            e["index"] = int(i);
            if (i < stats.size()) {
                e["force"] = stats[i].force;
                e["torque"] = stats[i].torque;
                e["broken"] = stats[i].broken;
            }
            j["joints"].push_back(e);
        }
    }
    // ケーブル（FEA）。張力はシミュレート中だけ（端の拘束の反力）。
    j["cables"] = nlohmann::json::array();
    {
        const auto cables = editor_.cables();
        std::vector<double> tension;
        {
            std::lock_guard<std::mutex> pl(poseMutex_);
            tension = cableTension_;
        }
        for (std::size_t i = 0; i < cables.size(); ++i) {
            nlohmann::json e = ed::toJson(cables[i]);
            e["index"] = int(i);
            if (i < tension.size()) e["tension"] = tension[i];
            j["cables"].push_back(e);
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
    // プレハブ（ASSETS パネルの 🧩 タイル）と、プレハブ編集モードの中身
    // （Inspector の部品一覧・映像ヘッダーの表示）。
    {
        nlohmann::json pf;
        pf["assets"] = nlohmann::json::array();
        for (const auto& d : editor_.prefabAssets()) {
            nlohmann::json e;
            e["name"] = d.name;
            e["parts"] = int(d.parts.size());
            pf["assets"].push_back(e);
        }
        j["prefabs"] = pf;
        const int obj = editor_.prefabEditObject();
        if (obj >= 0 && std::size_t(obj) < boxes_.size() && boxes_[std::size_t(obj)].alive) {
            const ed::BodyDesc& d = boxes_[std::size_t(obj)].desc;
            nlohmann::json e;
            e["index"] = obj;
            e["name"] = d.prefab;
            e["objectName"] = d.name;
            e["vehicle"] = d.hasVehicle;
            e["axles"] = int(d.hasVehicle ? d.vehicle.axles.size() : 0);
            ed::PrefabDesc pd;
            if (editor_.prefabAsset(d.prefab, pd)) e["parts"] = ed::toJson(pd)["parts"];
            else e["parts"] = nlohmann::json::array();
            j["prefabEdit"] = e;
        }
    }
    // 計算式アセット（ASSETS パネルの 🧮 タイル・ノードエディタ・Inspector
    // の車両節が使う）。タイヤ式の入出力の名前も一緒に渡す（in / out ノード
    // の選択肢）。
    {
        nlohmann::json f;
        f["assets"] = nlohmann::json::array();
        for (const auto& g : editor_.formulaAssets()) f["assets"].push_back(ed::toJson(g));
        f["tireInputs"] = wizengine::vehicle::tireFormulaInputs();
        f["tireOutputs"] = wizengine::vehicle::tireFormulaOutputs();
        // 空気圧式（体積比 → 圧力）の入出力名。ノードエディタの in / out の
        // 選択肢に、タイヤ式のぶんと並べて出す。
        f["pressureInputs"] = wizengine::vehicle::pressureFormulaInputs();
        f["pressureOutputs"] = wizengine::vehicle::pressureFormulaOutputs();
        j["formulas"] = f;
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
        if (!boxes_[i].desc.prefab.empty()) e["prefab"] = boxes_[i].desc.prefab;
        if (boxes_[i].desc.hasVehicle) e["vehicle"] = true;
        if (boxes_[i].desc.hasSoft) e["soft"] = true;  // 階層一覧の 🫧
        j["objects"].push_back(e);
    }
    j["aliveCount"] = alive;

    // インスペクタ用に、このカメラが選んでいる物だけ設計値を丸ごと送る。
    const std::size_t sel = controllers_[cameraIndex]->selected();
    if (sel < boxes_.size() && boxes_[sel].alive) {
        nlohmann::json d = ed::toJson(boxes_[sel].desc);
        d["index"] = int(sel);
        // 車両なら軸ごとの要点（Inspector の車両節: タイヤ式の付け先）。
        if (boxes_[sel].desc.hasVehicle) {
            nlohmann::json axles = nlohmann::json::array();
            for (const auto& a : boxes_[sel].desc.vehicle.axles) {
                axles.push_back({{"z", a.z},
                                 {"steer", a.steerDeg},
                                 {"driven", a.driven},
                                 {"formula", a.tire.formula},
                                 {"soft", a.tire.soft},
                                 {"stiffness", a.tire.stiffness},
                                 {"pressure", a.tire.pressure},
                                 {"pressureFormula", a.tire.pressureFormula}});
            }
            d["vehicleAxles"] = axles;
        }
        // ソフトボディなら実際の粒子数・ばね数（Inspector の説明に出す）。
        if (boxes_[sel].lattice) {
            d["softParticles"] = int(boxes_[sel].lattice->particleCount());
            d["softSprings"] = int(boxes_[sel].lattice->springs.size());
        }
        if (sel < poses.size()) {
            d["px"] = poses[sel].px;
            d["py"] = poses[sel].py;
            d["pz"] = poses[sel].pz;
        }
        j["selected"] = d;
    }
    return j.dump();
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
    doc.render = render_;
    doc.hasVisual = true;

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

    // ジョイントも番号で参照される（onJointBreak / setMotor）ので、端点が
    // 消えて落ちたぶんを詰めた対応表を作る。
    const auto allJoints = editor_.joints();
    std::vector<int> jointRemap(allJoints.size(), -1);
    for (std::size_t k = 0; k < allJoints.size(); ++k) {
        ed::JointDesc copy = allJoints[k];
        auto fix = [&](int& ref) {
            if (ref < 0) return true;  // 地面はそのまま
            if (std::size_t(ref) >= remap.size() || remap[std::size_t(ref)] < 0)
                return false;
            ref = remap[std::size_t(ref)];
            return true;
        };
        if (!fix(copy.bodyA) || !fix(copy.bodyB)) continue;
        jointRemap[k] = int(doc.joints.size());
        doc.joints.push_back(copy);
    }
    // ケーブル。端の物が消えていたら固定点（-1）にする（アンカーは残る）。
    for (const auto& cIn : editor_.cables()) {
        ed::CableDesc c = cIn;
        auto fix = [&](int& ref) {
            if (ref < 0) return;  // 地面 / 自由端はそのまま
            ref = (std::size_t(ref) < remap.size() && remap[std::size_t(ref)] >= 0)
                      ? remap[std::size_t(ref)]
                      : -1;
        };
        fix(c.bodyA);
        fix(c.bodyB);
        doc.cables.push_back(c);
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
                if (ok && tk == ed::NodeTargetKind::Joint && n.target >= 0) {
                    if (std::size_t(n.target) >= jointRemap.size() ||
                        jointRemap[std::size_t(n.target)] < 0) {
                        ok = false;
                    } else {
                        n.target = jointRemap[std::size_t(n.target)];
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
    doc.formulas = editor_.formulaAssets();
    doc.prefabs = editor_.prefabAssets();
    return doc;
}

std::string Scene::documentXml() {
    return ed::toXmlText(document());
}

void Scene::loadDocument(const ed::SceneDocument& doc) {
    // いま在るものを全部畳んでから作り直す。番号は 0 から振り直されるので、
    // 掴んでいる選択も落とす。
    removeAllCables();
    physics_.removeAllJoints();
    editor_.setJoints({});
    editor_.setCables({});
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
    // 描画設定（<visual>）も同じ。書いていない文書は既定値 = 従来の絵。
    setRenderDesc(doc.hasVisual ? doc.render : ed::RenderDesc{});

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
    // 計算式アセット。<vehicle> の中に書かれた古い置き場（<formula>）は
    // シーンのアセットへ移す（同じ名前があればそちらを優先）。次に保存
    // したときは <asset> に出る。
    {
        std::vector<wizengine::vehicle::FormulaGraphDesc> formulas = doc.formulas;
        auto has = [&formulas](const std::string& name) {
            for (const auto& f : formulas) {
                if (f.name == name) return true;
            }
            return false;
        };
        for (const auto& b : doc.bodies) {
            if (!b.hasVehicle) continue;
            for (const auto& f : b.vehicle.formulas) {
                if (!f.name.empty() && !has(f.name)) formulas.push_back(f);
            }
        }
        editor_.setFormulaAssets(std::move(formulas));
    }
    editor_.setPrefabAssets(doc.prefabs);
    editor_.setPrefabEditObject(-1);
    for (const auto& bIn : doc.bodies) {
        ed::BodyDesc b = ed::clampBody(bIn);
        b.vehicle.formulas.clear();  // 置き場はシーンのアセットに統一
        createObject(b);
    }

    std::vector<ed::JointDesc> joints;
    for (const auto& jIn : doc.joints) {
        ed::JointDesc j = jIn;
        // 文書の中では 0 起点。実際の番号は既存ぶんだけずれる。
        if (j.bodyA >= 0) j.bodyA += int(base);
        if (j.bodyB >= 0) j.bodyB += int(base);
        joints.push_back(j);
    }
    editor_.setJoints(std::move(joints));
    {
        std::vector<ed::CableDesc> cables;
        for (const auto& cIn : doc.cables) {
            ed::CableDesc c = cIn;
            if (c.bodyA >= 0) c.bodyA += int(base);
            if (c.bodyB >= 0) c.bodyB += int(base);
            cables.push_back(c);
        }
        editor_.setCables(std::move(cables));
    }

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

    // 文書が Core 専用の機能を使っていれば系を切り替える（使わなくなって
    // いれば既定へ戻す）。シミュレート中ならジョイントとケーブルも張り直す。
    syncBackend();
    if (editor_.mode() == ed::AppMode::Simulate) {
        buildJoints();
        buildCables();
    }
    snapshot();
}

// 文書の中身を今のシーンへ足す（取込用）。読込（loadDocument）と違って
// 何も消さない: メッシュ・プレハブは名前が重ならないものだけ足し、ボディは
// 後ろに追加、ジョイント・ケーブルの参照は追加ぶんの先頭番号だけずらす。
// 設定（option / visual / ground / ライト / カメラ / イベント）は文書に
// あっても無視する（取り込む物の「構造」だけが欲しいので）。
void Scene::appendDocument(const ed::SceneDocument& doc) {
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        for (const auto& m : doc.meshes) {
            bool dup = false;
            for (const auto& have : meshes_) dup = dup || have.desc.name == m.name;
            if (dup) continue;
            MeshAsset a;
            a.desc = m;
            meshes_.push_back(std::move(a));
        }
    }
    {
        auto prefabs = editor_.prefabAssets();
        for (const auto& p : doc.prefabs) {
            bool dup = false;
            for (const auto& have : prefabs) dup = dup || have.name == p.name;
            if (!dup) prefabs.push_back(p);
        }
        editor_.setPrefabAssets(std::move(prefabs));
    }
    const std::size_t base = boxes_.size();
    for (const auto& bIn : doc.bodies) {
        ed::BodyDesc b = ed::clampBody(bIn);
        b.vehicle.formulas.clear();
        createObject(b);
    }
    {
        auto joints = editor_.joints();
        for (const auto& jIn : doc.joints) {
            ed::JointDesc j = jIn;
            if (j.bodyA >= 0) j.bodyA += int(base);
            if (j.bodyB >= 0) j.bodyB += int(base);
            joints.push_back(j);
        }
        editor_.setJoints(std::move(joints));
    }
    {
        auto cables = editor_.cables();
        for (const auto& cIn : doc.cables) {
            ed::CableDesc c = cIn;
            if (c.bodyA >= 0) c.bodyA += int(base);
            if (c.bodyB >= 0) c.bodyB += int(base);
            cables.push_back(c);
        }
        editor_.setCables(std::move(cables));
    }
    editor_.bumpGraphVersion();
    syncBackend();
    if (editor_.mode() == ed::AppMode::Simulate) {
        buildJoints();
        buildCables();
    }
    snapshot();
}
