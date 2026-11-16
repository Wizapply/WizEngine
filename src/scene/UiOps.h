#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/UiState.h"
#include "ui/UiTypes.h"

// UI のエディタ操作（ブラウザの edit.ui.* → EditorState の Op → PHYSICS スレッド）を
// UiState の設計値へ当てる。直すきっかけは「UI の編集の操作を足す・変える」だけ。
//
//   ui.add            {name?, attrs?}                    Canvas を足す（追加した物を選ぶ）
//   ui.remove         {canvas}                           Canvas を消す（番号は詰めない）
//   ui.set            {canvas, name?, attrs?}            Canvas の名前・属性の部分更新
//   ui.element.add    {canvas, kind, parent?, name?, props?}  要素を足す（選ぶ）
//   ui.element.remove {canvas, element}                  要素とその子孫を消す
//   ui.element.set    {canvas, element, name?, props?, reset?}  要素の名前・性質の部分更新
//
// attrs / props は /scene の ui.selected と同じ形（{名前: 値}）。属性ごと・性質ごとの
// キーはここに書き並べない: 値は Canvas の属性の表（kCanvasAttrs）と性質の表
// （ui::fromJson）を走って読み、表の範囲へ丸める（clampValue / clampProps。ブラウザの
// 欄の lo / hi は見た目だけ）。型の合わない・知らない・その種類に無い名前は書かずに
// ステータスで知らせる。reset は既定値に戻す性質の名前の並び。
// 共用 UI（<canvas ui>）の要素はここでは編集しない（ファイルへ書く側が無い間は、
// 編集が保存で黙って消えるため）。Canvas の属性（表示方法・カメラ…）は編集できる。
//
// Chrono / Filament / Scene を知らない（tests/document で単体で検査する）。選択と
// ステータスは結果（Result）として返し、呼び手（Scene）が写す。
namespace wizengine {
namespace uiops {

struct Result {
    std::string status;  // ブラウザのステータス（日本語。空 = 出さない）
    bool select = false;  // true = 下の物（追加した物）を選ぶ
    int canvas = -1;
    int element = -1;     // -1 = Canvas そのもの
};

// kind が UI の編集の操作か（上の一覧の唯一の定義。INPUT が積む前の確かめにも使う）。
bool isUiOp(const std::string& kind);
// 操作を当てる（PHYSICS スレッド。設計値の書き手はここだけ）。args は型を信じない。
Result apply(UiState& state, const std::string& kind, const nlohmann::json& args);

// ---- 部品（apply が使う。tests/document が直接検査する）---------------------
// Canvas の属性の部分更新。attrs の中の、表にあって書き換えてよい属性だけを丸めて
// 書く。戻り値 = 書かなかった名前（型が合わない・知らない・書き換えられない）。
std::vector<std::string> setCanvasAttrs(const nlohmann::json& attrs, ui::CanvasDesc& desc);
// 要素の性質の部分更新。props の中の、その種類が持つ性質だけを丸めて書き、reset の
// 名前の性質を既定値へ戻す（書いていない状態にする）。戻り値 = 書かなかった名前。
std::vector<std::string> setElementProps(const nlohmann::json& props,
                                         const nlohmann::json& reset, ui::ElementDesc& e);
// 要素を末尾へ足す（文書順 = 描く順の一番手前。親は自分より前、の約束も保つ）。
// id は呼び手が渡す（UiState が Canvas ごとに持つ増える一方の番号 = 消した番号を
// 使い回さない）。parent は並びに無ければ -1（Canvas の直下）。
void addElement(std::vector<ui::ElementDesc>& elements, ui::ElementDesc e, int id);
// 消した数（要素と、その要素を名前で指していたアニメのキー）。
struct RemoveCount {
    std::size_t elements = 0;
    std::size_t keys = 0;
};
// 要素 id とその子孫を消す（親の消えた要素を残さない）。消えた要素を指していたアニメの
// キーも外す（残すと保存で無い名前を指し、後で同じ名前の要素を作ると別の要素が動く）。
// ほかの要素の id と parent は変えない。id が無ければ何もしない（0 件）。
RemoveCount removeElement(ui::CanvasDesc& desc, int id);
// 要素 id の名前を name にする（重なりの確かめは呼び手）。キーがその要素を指していれば
// （ui::findElementByName の規則で）付け替え、name が空ならそのキーを外す。戻り値 =
// 付け替えた・外したキーの数。
std::size_t renameElement(ui::CanvasDesc& desc, int id, const std::string& name);

}  // namespace uiops
}  // namespace wizengine
