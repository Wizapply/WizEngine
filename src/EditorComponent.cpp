#include "EditorComponent.h"

#include <algorithm>
#include <string>

#include "EditorTypes.h"
#include "Log.h"
#include "Scene.h"
#include "scene_math.h"

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
    if (what == "add") {
        ed::BodyDesc d;
        d.shape = ed::shapeFromName(msg.value("shape", "box"), ed::ShapeKind::Box);
        d.collision = d.shape;
        const double size = msg.value("size", 0.5);
        d.size = {size, size, size};
        d.mass = msg.value("mass", 1.0);
        d.fixed = msg.value("fixed", false);
        d.color = ed::colorFromHex(msg.value("color", ""), d.color);
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
                                "color", "size", "position", "rotation"}) {
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

    // ---- イベントグラフ（ノードエディタ）----------------------------------
    // edit.set と同じくモードは問わない: シミュレートを回しながらトリガーや
    // アクションを調整して発火を確かめる、という使い方を許す。
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
        // 対象を省いたら「今選んでいるもの」。Inspector の「＋」ボタンは
        // 選択中の対象に対するノードを作る操作なので、それが既定になる。
        if (!msg.contains("target")) {
            switch (ed::nodeTargetKind(kind)) {
                case ed::NodeTargetKind::Object:
                    args["target"] = selectionOf(scene, camIndex);
                    break;
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
        op.args = {{"id", ed::jsonInt(msg, "id", -1)}};
        op.camera = camIndex;
        state.push(std::move(op));
        return true;
    }

    if (what == "wire.add" || what == "wire.remove") {
        EditorState::Op op;
        op.kind = what;
        op.args = {{"from", ed::jsonInt(msg, "from", -1)},
                   {"to", ed::jsonInt(msg, "to", -1)}};
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
