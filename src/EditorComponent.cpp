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
    // シーンを書き換える操作はエディタカメラのページ専用。UI 側もタブと
    // ボタンを隠す/無効化しているが、判定はサーバーが持つ（リクエストは
    // 誰でも作れるため）。受け取って捨てる = true を返してコマンドは消費する。
    const bool isEditorCam = camIndex == scene.editorCamera();

    // ---- モード切替 -------------------------------------------------------
    if (cmd == "mode") {
        if (!isEditorCam) return true;
        const ed::AppMode target =
            ed::modeFromName(msg.value("mode", ""), state.mode());
        // シミュレートに入るときは一時停止を解除する。前回止めた状態が
        // 残っていると「開始したのに動かない」に見えるため。
        if (target == ed::AppMode::Simulate) tuning_.paused.store(false);
        state.requestMode(target);
        return true;
    }

    if (cmd.rfind("edit.", 0) != 0) return false;  // not ours
    const std::string what = cmd.substr(5);
    // シミュレート設定（edit.sim）だけは全カメラから変えられる。Physics タブ
    // は全ページにあり、隣に並ぶ Solver や Rate は誰でも触れるのに、これだけ
    // 効かないのは分かりにくいため。シーンの中身を書き換える操作ではない。
    if (what != "sim" && !isEditorCam) return true;

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
