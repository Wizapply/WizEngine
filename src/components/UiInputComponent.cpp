#include "components/UiInputComponent.h"

#include <string>

#include "core/Json.h"
#include "scene/Scene.h"
#include "scene/UiOps.h"

const wizengine::UiState::Snapshot& UiInputComponent::snapshot(Scene& scene) {
    scene.ui().copyIfChanged(known_, snap_);
    return snap_;
}

bool UiInputComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    const std::string cmd = wizengine::jsonString(msg, "cmd", "");
    if (cmd.rfind(kEditPrefix, 0) == 0) return pushEdit(scene, camIndex, cmd, msg);
    if (cmd != "select.ui") return false;  // not ours

    EditorState& state = scene.editor();
    // 受け取って捨てる = true（他の係へ回さない）。判定はサーバーが持つ（editsUiFrom。
    // 一覧の UI の行もプレハブ編集中は選べない見た目にしてある）。
    if (!scene.editsUiFrom(camIndex)) return true;

    const int canvas = wizengine::jsonInt(msg, "canvas", -1);
    // 要素の id は 0 以上。負は全部「Canvas そのもの」(-1) にそろえる。
    const int raw = wizengine::jsonInt(msg, "element", -1);
    const int element = raw < 0 ? -1 : raw;
    if (canvas < 0) {
        if (state.selKind() == EditorState::SelKind::Canvas) state.clearSel();
        return true;
    }
    // 消えた Canvas・無い要素は選ばせない（読込と行き違ったクリックなど）。
    if (!snapshot(scene).selectionExists(canvas, element)) return true;
    scene.setEditorSel(camIndex, EditorState::SelKind::Canvas, canvas, element);
    return true;
}

bool UiInputComponent::pushEdit(Scene& scene, std::size_t camIndex, const std::string& cmd,
                                const nlohmann::json& msg) {
    EditorState& state = scene.editor();
    // 受け取って黙って捨てる = true（EditorComponent へ回さない）。判定は select.ui と同じ
    // editsUiFrom。受け付けないページの edit.* を黙って捨てるのは EditorComponent と同じ。
    if (!scene.editsUiFrom(camIndex)) return true;
    // Op の種類は "edit." を外した名前（"ui.add" など）。知っている操作かは UiOps の一覧。
    const std::string kind = cmd.substr(std::char_traits<char>::length("edit."));
    if (!wizengine::uiops::isUiOp(kind)) {
        state.setStatus("UI: 知らない操作です: " + cmd);
        return true;
    }
    // 値の読み方と丸め・対象が今もあるかの確かめは PHYSICS の UiOps（ここで読んだ
    // 写しは 1 フレーム古いことがあるので、判断に使わない）。
    state.push({kind, msg, camIndex});
    return true;
}
