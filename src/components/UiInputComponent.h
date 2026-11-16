#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "scene/SceneComponent.h"
#include "scene/UiState.h"

// UI の入力（INPUT スレッドの onCommand）。いまは選択と編集の受け付け:
//
//   select.ui {canvas, element} … Canvas（element = -1）か、その中の要素を選ぶ。
//                                 canvas < 0 で解除。
//   edit.ui.*                   … 編集（一覧と中身は scene/UiOps.h）。ここでは受け付ける
//                                 ページかだけを見て、EditorState の Op（種類 = "ui.*"）
//                                 として積む。当てるのは PHYSICS（Scene::applyEditorOp →
//                                 UiOps）。EditorComponent より先に登録されているので、
//                                 edit.ui.* はそちらへ届かない。
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
    // 編集のコマンドの前置き（edit.ui.*）。
    static constexpr const char* kEditPrefix = "edit.ui.";
    // edit.ui.* を Op として積む（受け付けないページ・知らない操作は捨てる）。
    bool pushEdit(Scene& scene, std::size_t camIndex, const std::string& cmd,
                  const nlohmann::json& msg);
    // UiState の写し（INPUT スレッド専用。版が変わったときだけ取り直す）。
    const wizengine::UiState::Snapshot& snapshot(Scene& scene);

    std::uint64_t known_ = 0;
    wizengine::UiState::Snapshot snap_;
};
