#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/UiState.h"
#include "ui/UiTypes.h"

// UI のエディタ操作（ブラウザの edit.ui.* → EditorState の Op → PHYSICS スレッド）を
// UiState の設計値へ当てる。直すきっかけは「UI の編集の操作を足す・変える」だけ。
//
//   ui.add            {name?, attrs?, ui?}               Canvas を足す（追加した物を選ぶ。
//                                                        ui = 宣言した UI を表示する Canvas）
//   ui.remove         {canvas}                           Canvas を消す（番号は詰めない）
//   ui.set            {canvas, name?, attrs?}            Canvas の名前・属性の部分更新
//   ui.element.add    {canvas, kind, parent?, name?, props?}  要素を足す（選ぶ）
//   ui.element.remove {canvas, element}                  要素とその子孫を消す
//   ui.element.set    {canvas, element, name?, props?, reset?}  要素の名前・性質の部分更新
//   ui.xml            {canvas, text}                     Canvas の中身（要素とアニメ）を XML
//                                                        （<ui name> の書式）で置き換える
//
// attrs / props は /scene の ui.selected と同じ形（{名前: 値}）。属性ごと・性質ごとの
// キーはここに書き並べない: 値は Canvas の属性の表（kCanvasAttrs）と性質の表
// （ui::fromJson）を走って読み、表の範囲へ丸める（clampValue / clampProps。ブラウザの
// 欄の lo / hi は見た目だけ）。型の合わない・知らない・その種類に無い名前は書かずに
// ステータスで知らせる。reset は既定値に戻す性質の名前の並び。
// 要素の操作が書き換えるのは Canvas の中身の持ち主（UiState::copyCanvasContents）:
// <canvas ui> なら使っている UI の中身（同じ UI を使う全 Canvas に効き、共用 UI なら
// 保存でファイルへ書く = Scene::saveLibraryAssets）。ファイルが読めなかった・宣言の
// 無い UI の中身は編集しない（elementsEditable）。
//
// ファイルは読み書きしない（ui.xml も受けたテキストを読むだけ）。共用 UI のファイル・
// グローバル（ライブラリ）の操作（edit.lib.* の kind = ui）は scene/UiLibraryOps。
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
    // 消した Canvas の番号（-1 = 消していない）。呼び手（Scene）がそれを指すノードを掃除する。
    int removedCanvas = -1;
};

// kind が UI の編集の操作か（上の一覧の唯一の定義。INPUT が積む前の確かめにも使う）。
bool isUiOp(const std::string& kind);
// 足す world の Canvas の置き場所を決める口（呼び手 = Scene がカメラから決める。sizeY = 表で
// 丸めた板の高さ。決められなければ false = 既定の置き場所のまま）。
using WorldPlacement = std::function<bool(double sizeY, Vec3d& pos, Vec3d& euler)>;
// 操作を当てる（PHYSICS スレッド）。args は型を信じない。placeWorld は ui.add が world の
// Canvas を足し、attrs に pos も euler も書いていないときだけ呼ぶ（アセットパネルの World
// Canvas。空なら呼ばない）。
Result apply(UiState& state, const std::string& kind, const nlohmann::json& args,
             const WorldPlacement& placeWorld = {});
// Canvas の要素をエディタで編集してよいか（唯一の定義。Inspector の操作・映像の上の
// ドラッグ・選択枠のつまみ・ブラウザの editable が引く）: 自分の中身と、読めている UI の
// 中身は編集する。宣言の無い UI・ファイルが読めなかった共用 UI の中身は編集しない
// （書いても保存でファイルへ書かれず、黙って消えるため）。info は Snapshot::contentsInfo か
// copyCanvasContents から。
bool elementsEditable(const UiState::ContentsInfo& info);

// ui.xml の text を確かめて読む（唯一の確かめ）。空・大きすぎる
// （editor::kMaxXmlEditBytes）・XML として壊れていれば、その理由のステータス（日本語）を
// 返す。正しければ空を返し、out に中身、warnings に中身の警告（PHYSICS が当てるときは
// これ 1 回だけ読む）。
std::string readXmlText(const std::string& text, ui::UiAssetDesc& out,
                        std::vector<std::string>& warnings);
// readXmlText の確かめだけ（INPUT が積む前。中身は捨てる）。
std::string xmlTextProblem(const std::string& text);

// args の name の読み方（唯一の定義。UiOps の名前の変更と、UiLibraryOps の共用にする名前が
// 使う）: 無い・文字列でない = Absent、normalize（名前の正規化）で全部落ちた（日本語だけ
// など = 消したいのではない）= Unusable、それ以外は Ok で out に正規化した名前。
enum class NameArg { Absent, Ok, Unusable };
NameArg readNameArg(const nlohmann::json& args,
                    const std::function<std::string(const std::string&)>& normalize,
                    std::string& out);

// ---- 部品（apply が使う。tests/document が直接検査する）---------------------
// Canvas の属性の部分更新。attrs の中の、表にあって書き換えてよい属性だけを丸めて
// 書く。戻り値 = 書かなかった名前（型が合わない・知らない・書き換えられない）。
std::vector<std::string> setCanvasAttrs(const nlohmann::json& attrs, ui::CanvasDesc& desc);
// world の板の置き場所（pos / euler。nullptr = 変えない）を書く（ギズモ。Inspector の欄と
// 同じ表の丸めを通す。ステータスは出さない）。Canvas が無ければ false。
bool setCanvasPlacement(UiState& state, int canvas, const Vec3d* pos, const Vec3d* euler);
// 要素の性質の部分更新。props の中の、その種類が持つ性質だけを丸めて書き、reset の
// 名前の性質を既定値へ戻す（書いていない状態にする）。戻り値 = 書かなかった名前。
std::vector<std::string> setElementProps(const nlohmann::json& props,
                                         const nlohmann::json& reset, ui::ElementDesc& e);
// 要素 e が持つ性質（種類の表から）だけを丸めて重ねる（唯一の手順。Inspector の欄と
// 映像の上のドラッグが通る）。戻り値 = その種類に無いので書かなかった性質の名前。
std::vector<std::string> writeOwnProps(ui::ElementDesc& e, ui::PropSet incoming);
// Canvas canvas の要素 id を書き換える手順（唯一の定義）: 今の Canvas を中身の持ち主ごと
// 1 枚写し（UiState::copyCanvasContents）、要素を編集してよい（elementsEditable）か・要素が
// あるかを確かめ、edit(写した Canvas, 要素の添字) が true を返したら持ち主へ戻す
// （replaceCanvasContents。false = 書かない）。written = false なら何もしていない
// （refusal は編集してよくないために断ったときの理由のステータス。消えていただけなら空）。
struct ElementWrite {
    bool written = false;
    std::string refusal;
};
ElementWrite editElement(UiState& state, int canvas, int id,
                         const std::function<bool(ui::CanvasDesc&, std::size_t)>& edit);
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
