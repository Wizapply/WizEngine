#pragma once

#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "scene/SceneComponent.h"
#include "scene/UiState.h"

// UI の入力（INPUT スレッドの onCommand）。いまは選択だけ:
//
//   select.ui {canvas, element} … Canvas（element = -1）か、その中の要素を選ぶ。
//                                 canvas < 0 で解除。
//
// 選択の実体は EditorState の editorSel（SelKind::Canvas、index = Canvas の番号、
// sub = 要素の id）。受け付けるページの判定（Scene::editsFrom = エディタモード ×
// エディタカメラ）と、オブジェクトの選択との排他（Scene::setEditorSel）は Scene の口を
// 使う。プレハブ編集中は捨てる（そのあいだ選べるのは部品だけ）。
//
// 当たり判定・ドラッグ（後のタスク）も含め、UiState の設計値は版の写し（copyIfChanged）
// だけを読む（1 フレーム古くてよい。PHYSICS の書き換えとロックで競らない）。
// ギズモより前に登録する（UI の上を押したら、後ろのハンドルや物を掴まない）。
class UiInputComponent : public SceneComponent {
public:
    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override;

private:
    // UiState の写し（INPUT スレッド専用。版が変わったときだけ取り直す）。
    const wizengine::UiState::Snapshot& snapshot(Scene& scene);

    std::uint64_t known_ = 0;
    wizengine::UiState::Snapshot snap_;
};
