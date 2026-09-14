#include "components/EditorComponent.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "document/EditorTypes.h"
#include "core/Log.h"
#include "physics/ModelImport.h"
#include "scene/PrefabDefaults.h"
#include "scene/Scene.h"
#include "document/SceneDocument.h"
#include "scene/SceneMath.h"
#include "vehicle/Formula.h"

namespace ed = wizengine::editor;

namespace {

// 何も指定されずに「置く」と言われたときの場所: カメラの正面、地面の上。
// 視線が地面と交わらない（空を向いている）ときは 4m 先に置く。
ed::Vec3d placeInFrontOf(Scene& scene, std::size_t camIndex, double halfHeight) {
    const auto basis = scenemath::cameraBasis(scene.camera(camIndex));
    if (!basis.valid) return {0.0, halfHeight, 0.0};

    scenemath::Vec3 p = basis.eye + basis.forward * 4.0;
    const double t = scenemath::rayHitsGround(basis.eye, basis.forward, 0.0);
    if (t > 0.5 && t < 40.0) p = basis.eye + basis.forward * t;
    return {p.x(), std::max(0.0, p.y()) + halfHeight, p.z()};
}

// リクエストが指しているイベントアセットの名前（正規化済み。無ければ空）。
// ノード・ワイヤーの操作は必ずこれを伴う - 名前が空なら Scene 側で
// 「イベントが見つかりません」になって何も起きない。
std::string eventAssetName(const nlohmann::json& msg) {
    if (!msg.contains("asset") || !msg["asset"].is_string()) return std::string();
    return ed::sanitizeEventName(msg["asset"].get<std::string>());
}

// そのカメラが今つかんでいるオブジェクト番号（無ければ -1）。
int selectionOf(Scene& scene, std::size_t camIndex) {
    const std::size_t sel = scene.boxController(camIndex).selected();
    return sel == BoxController::kNone ? -1 : int(sel);
}

}  // namespace

bool EditorComponent::onCommand(Scene& scene, std::size_t camIndex,
                                const nlohmann::json& msg) {
    const std::string cmd = msg.value("cmd", "");
    EditorState& state = scene.editor();
    // シーンを書き換える操作（edit.*、sim を除く）はエディタカメラのページ
    // 専用。UI 側も Inspector タブを隠しているが、判定はサーバーが持つ
    // （リクエストは誰でも作れるため）。受け取って捨てる = true を返して
    // コマンドは消費する。
    const bool isEditorCam = camIndex == scene.editorCamera();

    // ---- モード切替 -------------------------------------------------------
    // モード切替だけは**どのカメラのページからでも**受ける。シーンの中身を
    // 書き換える操作ではないし、Camera 1/2 で観察しながら回す・止めるのは
    // 普通の使い方のため。エディタカメラはあくまで「編集できる」カメラで
    // あって、「モードを握る」カメラではない。
    if (cmd == "mode") {
        const ed::AppMode target =
            ed::modeFromName(msg.value("mode", ""), state.mode());
        // シミュレートに入るときは一時停止を解除する。前回止めた状態が
        // 残っていると「開始したのに動かない」に見えるため。
        if (target == ed::AppMode::Simulate) tuning_.paused.store(false);
        state.requestMode(target);
        return true;
    }

    // ---- ライト / カメラの選択（サイドバーのクリック）----------------------
    // シーンは書き換えないが、エディタ選択はエディタモード×エディタカメラ
    // 専用の状態なので同じ縛りにする。選択の実体は EditorState の atomic。
    // プレハブ編集モードの部品の選択（Inspector の一覧のクリック）。
    if (cmd == "select.part") {
        if (!isEditorCam || !state.isEditor()) return true;
        const int index = msg.value("index", -1);
        if (index < 0 || state.prefabEditObject() < 0) {
            if (state.selKind() == EditorState::SelKind::Part) state.clearSel();
            return true;
        }
        scene.boxController(camIndex).setSelected(BoxController::kNone);
        state.setSel(EditorState::SelKind::Part, index);
        return true;
    }

    if (cmd == "select.light" || cmd == "select.camera") {
        if (!isEditorCam || !state.isEditor()) return true;
        const int index = msg.value("index", -1);
        if (index < 0) {
            state.clearSel();
            return true;
        }
        auto lk = scene.lockObjects();
        if (cmd == "select.light") {
            if (std::size_t(index) < scene.lightCount() &&
                scene.lightAlive(std::size_t(index))) {
                scene.boxController(camIndex).setSelected(BoxController::kNone);
                state.setSel(EditorState::SelKind::Light, index);
            }
        } else {
            // エディタカメラ自身は選ばせない（自分の目は動かせない）。
            if (std::size_t(index) < scene.cameraCount() &&
                scene.cameraActive(std::size_t(index)) &&
                std::size_t(index) != scene.editorCamera()) {
                scene.boxController(camIndex).setSelected(BoxController::kNone);
                state.setSel(EditorState::SelKind::Camera, index);
            }
        }
        return true;
    }

    if (cmd.rfind("edit.", 0) != 0) return false;  // not ours
    const std::string what = cmd.substr(5);
    // シミュレート設定（edit.sim）だけは全カメラから変えられる。Physics タブ
    // は全ページにあり、隣に並ぶ Solver や Rate は誰でも触れるのに、これだけ
    // 効かないのは分かりにくいため。シーンの中身を書き換える操作ではない。
    if (what != "sim" && !isEditorCam) return true;
    // ライトとカメラの編集は**エディタモード専用**。オブジェクトの edit.set
    // などはシミュレート中の物性いじりに使えるが、照明と視点の設計は
    // エディタの仕事、という整理（ユーザー指定）。
    if ((what.rfind("light.", 0) == 0 || what.rfind("camera.", 0) == 0) &&
        !state.isEditor()) {
        return true;
    }

    // ---- 追加 -------------------------------------------------------------
    // 階段: 専用のオブジェクトは持たず、「全段を collide 付きの箱で並べた
    // プレハブ」を geom の無い固定ボディ（原点 = 全体が収まる箱の中心）に
    // 付けて置く。プレハブ "stairs" が無ければ作り（あれば共有 = Unity で
    // 同じプレハブを 2 度置くのと同じ）、カメラの正面に置く。段はプレハブ
    // 編集で動かせる。
    if (what == "add" && msg.value("shape", "") == "stairs") {
        const int steps = std::max(1, std::min(30, ed::jsonInt(msg, "steps", 6)));
        const double rise = std::max(0.02, std::min(2.0, ed::jsonNumber(msg, "rise", 0.12)));
        const double run = std::max(0.05, std::min(5.0, ed::jsonNumber(msg, "run", 0.4)));
        const double width = std::max(0.1, std::min(20.0, ed::jsonNumber(msg, "width", 2.0)));
        const ed::Vec3d base = placeInFrontOf(scene, camIndex, 0.0);
        // 奥行きの向き = カメラの前向きを水平に落としたもの（無ければ -Z）。
        const auto basis = scenemath::cameraBasis(scene.camera(camIndex));
        double fx = 0.0, fz = -1.0;
        if (basis.valid) {
            const double len = std::sqrt(basis.forward.x() * basis.forward.x() +
                                         basis.forward.z() * basis.forward.z());
            if (len > 1e-6) {
                fx = basis.forward.x() / len;
                fz = basis.forward.z() / len;
            }
        }
        // 箱のローカル +Z を fwd に向ける回転（R = Ry(yaw)、(0,0,1) → (sin, 0, cos)）。
        const double yawDeg = std::atan2(fx, fz) * 180.0 / scenemath::kPi;
        const ed::Color3 color = ed::colorFromHex(msg.value("color", ""),
                                                 ed::Color3{0.62f, 0.64f, 0.68f});
        std::string prefab = ed::sanitizeEventName(msg.value("prefab", "stairs"));
        if (prefab.empty()) prefab = "stairs";
        if (!state.hasPrefabAsset(prefab)) {
            EditorState::Op op;
            op.kind = "prefab.add";
            op.args = ed::toJson(ed::builtinStairsPrefab(prefab, steps, rise, run, width, color));
            op.camera = camIndex;
            state.push(std::move(op));
        }
        ed::BodyDesc d;
        d.name = prefab;
        d.shape = ed::ShapeKind::None;   // 見た目も当たりも部品だけ
        d.collision = ed::ShapeKind::None;
        d.size = {width, rise * double(steps), run * double(steps)};  // 外接箱（表示用）
        const double cz = run * double(steps) * 0.5;
        d.position = {base.x + fx * cz, rise * double(steps) * 0.5, base.z + fz * cz};
        d.rotation = {0.0, yawDeg, 0.0};
        d.mass = 50.0;
        d.fixed = true;
        d.color = color;
        d.prefab = prefab;
        EditorState::Op op;
        op.kind = "add";
        op.args = ed::toJson(ed::clampBody(d));
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }
    if (what == "add") {
        ed::BodyDesc d;
        d.shape = ed::shapeFromName(msg.value("shape", "box"), ed::ShapeKind::Box);
        d.collision = d.shape;
        // メッシュ（glTF）を置く場合はアセット名が要る。実在の検証は
        // createObject が行う（見つからなければ球で描く）。
        if (d.shape == ed::ShapeKind::Model && msg.contains("mesh") &&
            msg["mesh"].is_string()) {
            d.mesh = msg["mesh"];
        }
        const double size = msg.value("size", 0.5);
        d.size = {size, size, size};
        d.mass = msg.value("mass", 1.0);
        d.fixed = msg.value("fixed", false);
        d.color = ed::colorFromHex(msg.value("color", ""), d.color);
        // ソフトボディ（アセットパネルの 🫧 タイル）。`soft: true` か
        // `soft: {enabled, res, ...}`。中身は bodyFromJson と同じ読み方。
        if (msg.contains("soft")) {
            nlohmann::json only;
            only["soft"] = msg["soft"];
            d = ed::bodyFromJson(only, d);
            // メッシュのソフトボディは無い（格子は箱か球）。
            if (d.hasSoft && d.shape == ed::ShapeKind::Model) {
                d.shape = ed::ShapeKind::Box;
                d.collision = ed::ShapeKind::Box;
                d.mesh.clear();
            }
        }
        d = ed::clampBody(d);

        if (msg.contains("x") && msg.contains("y") && msg.contains("z")) {
            d.position = {msg.value("x", 0.0), msg.value("y", 1.0),
                          msg.value("z", 0.0)};
        } else {
            d.position = placeInFrontOf(scene, camIndex, d.size.y * 0.5);
        }

        EditorState::Op op;
        op.kind = "add";
        op.args = ed::toJson(d);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- 削除・複製 -------------------------------------------------------
    if (what == "remove" || what == "duplicate") {
        const int index = msg.value("index", selectionOf(scene, camIndex));
        if (index < 0) return true;  // 何も選んでいない: 黙って無視
        EditorState::Op op;
        op.kind = what;
        op.args = {{"index", index}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- プロパティ変更 ---------------------------------------------------
    if (what == "set") {
        const int index = msg.value("index", selectionOf(scene, camIndex));
        if (index < 0) return true;
        // 送られてきたキーだけを積む（bodyFromJson が「無いキーは今の値」と
        // いう作りなので、部分更新がそのまま通る）。
        nlohmann::json args;
        args["index"] = index;
        for (const char* key : {"name", "shape", "collision", "mass", "fixed",
                                "color", "size", "position", "rotation",
                                "soft", "surface", "layer", "nocollide",
                                "gravity", "velocity", "angularVelocity",
                                "force", "torque"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = "set";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- ジョイント -------------------------------------------------------
    if (what == "joint.add") {
        ed::JointDesc j;
        j.kind = ed::jointFromName(msg.value("kind", "revolute"), j.kind);
        // A を省いたらこのカメラの選択、B を省いたら地面。「選んだ物を壁に
        // ちょうつがいで留める」が一番よくある操作なので、それを既定にする。
        j.bodyA = msg.value("a", selectionOf(scene, camIndex));
        j.bodyB = msg.value("b", -1);
        j.name = msg.value("name", std::string());
        j.distance = msg.value("distance", 0.0);
        if (j.bodyA < 0 && j.bodyB < 0) return true;  // 地面同士は無意味

        nlohmann::json args = ed::toJson(j);
        // リミット・モータ・ばね・破断・歯車比など（送られたものだけ。
        // jointFromJson が「無いキーは今の値」なので部分指定がそのまま通る）。
        for (const char* key : {"limited", "limitLo", "limitHi", "motor", "motorTarget",
                                "stiffness", "damping", "breakForce", "ratio", "pitch",
                                "anchor2", "axis2", "rotStiffness", "rotDamping"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        // アンカーと軸: 指定があればそれ、無ければ物理スレッド側で
        // 2 体の中点を使う（"anchor" を落として渡すのが合図）。
        if (msg.contains("anchor")) {
            args["anchor"] = msg["anchor"];
        } else if (msg.contains("x") && msg.contains("y") && msg.contains("z")) {
            args["anchor"] = {{"x", msg.value("x", 0.0)},
                              {"y", msg.value("y", 0.0)},
                              {"z", msg.value("z", 0.0)}};
        } else {
            args.erase("anchor");
        }
        if (msg.contains("axis")) {
            args["axis"] = msg["axis"];
        } else if (msg.contains("ax") || msg.contains("ay") ||
                   msg.contains("az")) {
            args["axis"] = {{"x", msg.value("ax", 0.0)},
                            {"y", msg.value("ay", 1.0)},
                            {"z", msg.value("az", 0.0)}};
        }

        EditorState::Op op;
        op.kind = "joint.add";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "joint.remove") {
        EditorState::Op op;
        op.kind = "joint.remove";
        op.args = {{"index", msg.value("index", -1)}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // 既にあるジョイントの値の変更（種類・軸・リミット・モータ・ばね・破断
    // など。送られたキーだけ）。シミュレート中なら作り直して即反映する。
    if (what == "joint.set") {
        const int index = ed::jsonInt(msg, "index", -1);
        if (index < 0) return true;
        nlohmann::json args;
        args["index"] = index;
        for (const char* key : {"name", "kind", "a", "b", "anchor", "axis", "distance",
                                "limited", "limitLo", "limitHi", "motor", "motorTarget",
                                "stiffness", "damping", "breakForce", "ratio", "pitch",
                                "anchor2", "axis2", "rotStiffness", "rotDamping"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = "joint.set";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- ケーブル（FEA）----------------------------------------------------
    // ジョイントと同じ配管。a を省いたらこのカメラの選択、b を省いたら地面
    // （固定点）。端の位置（anchorA / anchorB）を省いたら物理スレッド側で
    // 留め先の中心（地面なら真上）にする。
    if (what == "cable.add") {
        nlohmann::json args;
        args["a"] = ed::jsonInt(msg, "a", selectionOf(scene, camIndex));
        args["b"] = ed::jsonInt(msg, "b", -1);
        if (ed::jsonInt(args, "a", -1) < -2 || ed::jsonInt(args, "b", -1) < -2) return true;
        for (const char* key : {"name", "anchorA", "anchorB", "segments", "diameter",
                                "density", "young", "damping", "collide", "color"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = "cable.add";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }
    if (what == "cable.remove") {
        EditorState::Op op;
        op.kind = "cable.remove";
        op.args = {{"index", ed::jsonInt(msg, "index", -1)}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }
    if (what == "cable.set") {
        const int index = ed::jsonInt(msg, "index", -1);
        if (index < 0) return true;
        nlohmann::json args;
        args["index"] = index;
        for (const char* key : {"name", "a", "b", "anchorA", "anchorB", "segments",
                                "diameter", "density", "young", "damping", "collide",
                                "color"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = "cable.set";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- 取込（URDF / OpenSim / ADAMS。Chrono::Parsers）------------------
    // ファイル名は assets/ 相対のみ（他の assets 参照と同じ関所）。パース
    // そのものは物理スレッド（一時的な Chrono の系を作るため）。
    if (what == "import") {
        const std::string file = msg.value("file", std::string());
        if (!ed::assetFileAllowed(file)) {
            state.setStatus("取込: ファイル名が不正です（assets/ 相対のみ）");
            return true;
        }
        if (!wizengine::importAvailable()) {
            state.setStatus("取込: このビルドには Chrono::Parsers がありません "
                            "(-DWIZ_WITH_CHRONO_PARSERS=ON)");
            return true;
        }
        EditorState::Op op;
        op.kind = "import";
        op.args = {{"file", file}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- イベントアセット（ノードエディタ）--------------------------------
    // edit.set と同じくモードは問わない: シミュレートを回しながらトリガーや
    // アクションを調整して発火を確かめる、という使い方を許す。
    // ノード / ワイヤーの操作は必ず「どのアセットか」を伴う（同じ id が別の
    // アセットにも居るので、名前が無いと別のノードを触ってしまう）。
    if (what == "event.add" || what == "event.remove") {
        const std::string raw =
            (msg.contains("name") && msg["name"].is_string())
                ? msg["name"].get<std::string>()
                : std::string();
        const std::string name = ed::sanitizeEventName(raw);
        if (name.empty()) {
            state.setStatus("イベント名は英数字と _ - だけです");
            return true;
        }
        EditorState::Op op;
        op.kind = what;
        op.args = {{"name", name}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "event.attach" || what == "event.detach") {
        const std::string name = ed::sanitizeEventName(
            (msg.contains("name") && msg["name"].is_string())
                ? msg["name"].get<std::string>()
                : std::string());
        if (name.empty()) return true;
        // 付け先: -1（既定）= ワールド、0 以上 = オブジェクト番号。省略時は
        // 「いま選んでいるオブジェクト」（Inspector の付けるボタン）。
        int target = ed::jsonInt(msg, "target", -2);
        if (target == -2) target = selectionOf(scene, camIndex);
        EditorState::Op op;
        op.kind = what;
        op.args = {{"name", name}, {"target", target}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "node.add") {
        // kind も型を確かめてから読む（/input は生 JSON。文字列以外が来ても
        // 落とさず既定にする - jsonInt / jsonNumber と同じ理由）。
        const std::string kindName =
            (msg.contains("kind") && msg["kind"].is_string())
                ? msg["kind"].get<std::string>()
                : std::string();
        const ed::NodeKind kind =
            ed::nodeKindFromName(kindName, ed::NodeKind::OnCollision);
        nlohmann::json args;
        for (const char* key :
             {"x", "y", "target", "other", "seconds", "color", "vec", "value"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        args["kind"] = ed::nodeKindName(kind);  // 名前の揺れはここで正規化
        args["asset"] = eventAssetName(msg);
        // 衝突トリガーの「向き」は零（問わない）から始める - NodeDesc の vec の
        // 既定は ApplyImpulse 向けの (0, 5, 0) なので。
        if (kind == ed::NodeKind::OnCollision && !msg.contains("vec")) {
            args["vec"] = {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}};
        }
        // オブジェクトを対象にするノードは、対象を省いたら -1 のまま
        // （= 付いている相手 / トリガーが渡した物）。アセットは付け回す
        // 部品なので、作った瞬間に特定の番号へ縛らない。ライトとカメラは
        // 「付いている相手」を持てないので、選んでいるものを既定にする。
        if (!msg.contains("target")) {
            switch (ed::nodeTargetKind(kind)) {
                case ed::NodeTargetKind::Light:
                    if (state.selKind() == EditorState::SelKind::Light) {
                        args["target"] = state.selIndex();
                    }
                    break;
                case ed::NodeTargetKind::Camera:
                    if (state.selKind() == EditorState::SelKind::Camera) {
                        args["target"] = state.selIndex();
                    }
                    break;
                case ed::NodeTargetKind::Object:
                case ed::NodeTargetKind::Joint:  // -1 = どのジョイントでも
                case ed::NodeTargetKind::None:
                    break;
            }
        }
        EditorState::Op op;
        op.kind = "node.add";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "node.set") {
        const int id = ed::jsonInt(msg, "id", -1);
        if (id < 0) return true;
        // 送られてきたキーだけを積む（updateGraphNode が「無いキーは今の値」）。
        nlohmann::json args;
        args["id"] = id;
        args["asset"] = eventAssetName(msg);
        for (const char* key :
             {"x", "y", "target", "other", "seconds", "color", "vec", "value"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = "node.set";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "node.remove") {
        EditorState::Op op;
        op.kind = "node.remove";
        op.args = {{"id", ed::jsonInt(msg, "id", -1)},
                   {"asset", eventAssetName(msg)}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "wire.add" || what == "wire.remove") {
        EditorState::Op op;
        op.kind = what;
        op.args = {{"from", ed::jsonInt(msg, "from", -1)},
                   {"to", ed::jsonInt(msg, "to", -1)},
                   {"asset", eventAssetName(msg)}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- 計算式アセット（ノード式）------------------------------------------
    // イベントと同じ流儀。名前の規則も同じ（英数字と _ -）。ノードの種類は
    // 語彙表（vehicle/Formula.h）に無ければ積まない。
    if (what == "formula.add" || what == "formula.remove") {
        const std::string name = ed::sanitizeEventName(
            (msg.contains("name") && msg["name"].is_string())
                ? msg["name"].get<std::string>()
                : std::string());
        if (name.empty()) {
            state.setStatus("計算式の名前は英数字と _ - だけです");
            return true;
        }
        EditorState::Op op;
        op.kind = what;
        op.args = {{"name", name}};
        // template = "tire": 既定のタイヤ式（Magic Formula）から始める。
        if (msg.contains("template") && msg["template"].is_string()) {
            op.args["template"] = msg["template"].get<std::string>();
        }
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "fnode.add" || what == "fnode.set" || what == "fnode.remove") {
        nlohmann::json args;
        args["asset"] = eventAssetName(msg);
        args["id"] = ed::jsonInt(msg, "id", -1);
        if (what == "fnode.add") {
            const std::string type =
                (msg.contains("type") && msg["type"].is_string())
                    ? msg["type"].get<std::string>()
                    : std::string();
            if (!wizengine::vehicle::formulaKind(type)) {
                state.setStatus("知らないノードの種類です: " + type);
                return true;
            }
            args["type"] = type;
        }
        for (const char* key : {"x", "y", "name", "params", "value"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = what;
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "fwire.add" || what == "fwire.remove") {
        EditorState::Op op;
        op.kind = what;
        op.args = {{"asset", eventAssetName(msg)},
                   {"from", ed::jsonInt(msg, "from", -1)},
                   {"fromPort", ed::jsonInt(msg, "fromPort", 0)},
                   {"to", ed::jsonInt(msg, "to", -1)},
                   {"port", ed::jsonInt(msg, "port", 0)}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- プレハブ（右クリック「プレハブを編集」と Inspector の部品一覧）----
    if (what == "prefab.open" || what == "prefab.attach" || what == "prefab.detach") {
        int index = ed::jsonInt(msg, "index", -2);
        if (index == -2) index = selectionOf(scene, camIndex);
        if (index < 0) return true;
        EditorState::Op op;
        op.kind = what;
        op.args = {{"index", index}};
        if (what == "prefab.attach") {
            op.args["name"] = ed::sanitizeEventName(
                (msg.contains("name") && msg["name"].is_string())
                    ? msg["name"].get<std::string>()
                    : std::string());
        }
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }
    if (what == "prefab.close") {
        EditorState::Op op;
        op.kind = what;
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }
    if (what == "prefab.add" || what == "prefab.remove") {
        const std::string name = ed::sanitizeEventName(
            (msg.contains("name") && msg["name"].is_string())
                ? msg["name"].get<std::string>()
                : std::string());
        if (name.empty()) {
            state.setStatus("プレハブの名前は英数字と _ - だけです");
            return true;
        }
        EditorState::Op op;
        op.kind = what;
        op.args = {{"name", name}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }
    if (what == "part.add" || what == "part.set" || what == "part.remove") {
        nlohmann::json args;
        if (msg.contains("prefab") && msg["prefab"].is_string()) {
            args["prefab"] = ed::sanitizeEventName(msg["prefab"].get<std::string>());
        }
        args["part"] = ed::jsonInt(msg, "part", -1);
        for (const char* key : {"name", "type", "mesh", "position", "rotation", "size",
                                "color", "socket", "collide"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = what;
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- 車両（Inspector の「車両」節）--------------------------------------
    // vehicle.enable: そのオブジェクトを車両にする / やめる。
    // vehicle.tire: 軸（-1 = 全軸）のタイヤに計算式を付ける（空 = 組み込み）。
    // 対象は index（省略時は選択中のオブジェクト）。
    if (what == "vehicle.enable" || what == "vehicle.tire") {
        int index = ed::jsonInt(msg, "index", -2);
        if (index == -2) index = selectionOf(scene, camIndex);
        if (index < 0) return true;
        EditorState::Op op;
        op.kind = what;
        op.args = {{"index", index}};
        if (what == "vehicle.enable") {
            op.args["on"] = msg.contains("on") && msg["on"].is_boolean()
                                ? msg["on"].get<bool>()
                                : true;
        } else {
            // 送られてきたキーだけ積む（Scene 側も同じ約束で部分更新）。
            op.args["axle"] = ed::jsonInt(msg, "axle", -1);
            if (msg.contains("formula") && msg["formula"].is_string()) {
                op.args["formula"] =
                    ed::sanitizeEventName(msg["formula"].get<std::string>());
            }
            if (msg.contains("soft") && msg["soft"].is_boolean()) {
                op.args["soft"] = msg["soft"].get<bool>();
            }
            if (msg.contains("stiffness") && msg["stiffness"].is_number()) {
                op.args["stiffness"] = msg["stiffness"].get<double>();
            }
            if (msg.contains("pressure") && msg["pressure"].is_number()) {
                op.args["pressure"] = msg["pressure"].get<double>();
            }
            if (msg.contains("pressureFormula") && msg["pressureFormula"].is_string()) {
                op.args["pressureFormula"] =
                    ed::sanitizeEventName(msg["pressureFormula"].get<std::string>());
            }
        }
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- ライト -------------------------------------------------------------
    if (what == "light.add") {
        ed::LightDesc d;
        d.kind = ed::lightKindFromName(msg.value("kind", "point"), d.kind);
        if (d.kind == ed::LightKind::Sun) {
            // 平行光: 位置は光に影響しない（アイコンの置き場）。既定の太陽と
            // 喧嘩しない控えめな強さで、斜め下向きに。
            d.intensity = 50000.0;  // lux
            d.position = {0.0, 4.0, 0.0};
            d.rotation = {35.0, 30.0, 0.0};
        } else {
            // Point / Spot: カメラの正面、少し上に置く。真下向き（回転ゼロ）
            // なので、Spot はそのまま床を照らすスポットになる。
            const ed::Vec3d p = placeInFrontOf(scene, camIndex, 0.0);
            d.position = {p.x, p.y + 2.5, p.z};
            d.intensity = 300000.0;  // lm
        }
        EditorState::Op op;
        op.kind = "light.add";
        op.args = ed::toJson(ed::clampLight(d));
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "light.set") {
        const int index = msg.value("index", -1);
        if (index < 0) return true;
        // 送られてきたキーだけを積む（lightFromJson が「無いキーは今の値」）。
        nlohmann::json args;
        args["index"] = index;
        for (const char* key : {"name", "kind", "position", "rotation", "color",
                                "intensity", "falloff", "spotInnerDeg",
                                "spotOuterDeg", "shadows"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = "light.set";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "light.remove") {
        EditorState::Op op;
        op.kind = "light.remove";
        op.args = {{"index", msg.value("index", -1)}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- カメラ -------------------------------------------------------------
    if (what == "camera.add" || what == "camera.remove") {
        EditorState::Op op;
        op.kind = what;
        if (what == "camera.remove") {
            op.args = {{"index", msg.value("index", -1)}};
        }
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "camera.set") {
        const int index = msg.value("index", -1);
        if (index < 0) return true;
        nlohmann::json args;
        args["index"] = index;
        for (const char* key : {"position", "rotation"}) {
            if (msg.contains(key)) args[key] = msg[key];
        }
        EditorState::Op op;
        op.kind = "camera.set";
        op.args = std::move(args);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- シミュレート設定 -------------------------------------------------
    if (what == "sim") {
        const ed::SimSettings current = state.sim();
        const ed::SimSettings next = ed::clampSim(ed::simFromJson(msg, current));

        // レート系は main の PhysicsTuning が持っている（物理ループが毎パス
        // 見る値）。System タブと同じ口に書くので、どちらから変えても同じ。
        if (next.hz != current.hz) tuning_.physicsHz.store(next.hz);
        if (next.substeps != current.substeps) {
            tuning_.substeps.store(next.substeps);
        }
        if (next.iterations != current.iterations) {
            tuning_.pendingIterations.store(next.iterations);
        }
        if (next.envelope != current.envelope) {
            tuning_.pendingEnvelope.store(next.envelope);
        }
        if (next.recovery != current.recovery) {
            tuning_.pendingRecovery.store(next.recovery);
        }

        EditorState::Op op;
        op.kind = "sim";
        op.args = ed::toJson(next);
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- ギズモ設定 -------------------------------------------------------
    // 描画とドラッグの両方が毎フレーム読むだけの値なので、キューに積まず
    // その場で入れ替える（次のフレームから効く）。
    if (what == "gizmo") {
        state.setGizmo(ed::gizmoFromJson(msg, state.gizmo()));
        return true;
    }

    // ---- 地面・環境光・描画設定 -------------------------------------------
    // Inspector の World 節と「描画」節から。値の解釈（部分更新・クランプ・
    // パス検証・プリセットの展開）は物理スレッド側の適用時に行うが、キーの
    // 取り出しだけ済ませて素通しする（/input は誰でも叩けるので、JSON を
    // そのまま運ぶ他の Op と同じ流儀）。
    if (what == "ground" || what == "environment" || what == "render") {
        EditorState::Op op;
        op.kind = what;
        op.args = msg;
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- XML の直接適用 ---------------------------------------------------
    // モーダルの XML エディタから。パースは**ここ（INPUT スレッド）で検証**
    // する - 壊れた XML はキューに積まず、行番号付きの理由をステータスへ
    // 出す（適用は非同期なので、レスポンスでは返せない）。通ったテキストを
    // そのまま Op で運び、物理スレッドがもう一度パースして適用する
    // （SceneDocument を JSON に往復させるより、二度目の軽いパースの方が
    // 単純で間違いが無い）。
    if (what == "xml") {
        const std::string text = msg.value("text", std::string());
        if (text.empty() || text.size() > 1024 * 1024) {
            state.setStatus(text.empty() ? "XML が空です"
                                         : "XML が大きすぎます（1MB まで）");
            return true;
        }
        ed::SceneDocument doc;
        std::string error;
        if (!ed::parseXml(text, doc, error)) {
            state.setStatus("XML が読めません: " + error);
            return true;
        }
        EditorState::Op op;
        op.kind = "xml";
        op.args = {{"text", text}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    // ---- シーンファイル ---------------------------------------------------
    if (what == "save" || what == "load") {
        EditorState::Op op;
        op.kind = what;
        op.args = {{"name", msg.value("name", std::string())}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "clear") {
        EditorState::Op op;
        op.kind = "clear";
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    LOGW("editor", "unknown command '%s'", cmd.c_str());
    return true;  // edit.* は全部ここで受け止める（他の係に回さない）
}
