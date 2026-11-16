#include "components/UiInputComponent.h"

#include <string>

#include "core/Json.h"
#include "scene/Scene.h"

const wizengine::UiState::Snapshot& UiInputComponent::snapshot(Scene& scene) {
    scene.ui().copyIfChanged(known_, snap_);
    return snap_;
}

bool UiInputComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    const std::string cmd = wizengine::jsonString(msg, "cmd", "");
    if (cmd != "select.ui") return false;  // not ours

    EditorState& state = scene.editor();
    // 受け取って捨てる = true（他の係へ回さない）。判定はサーバーが持つ。プレハブ
    // 編集中は部品だけを選ぶ（一覧の UI の行も選べない見た目にしてある）。
    if (!scene.editsFrom(camIndex) || state.prefabEditObject() >= 0) return true;

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
