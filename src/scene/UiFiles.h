#pragma once

#include <string>
#include <vector>

#include "scene/LibraryFlow.h"  // LibraryBaseline
#include "scene/UiState.h"
#include "ui/UiTypes.h"

// 共用 UI のファイル（assets/ui/<名前>.xml。中身はシーンの <asset> の <ui> と同じ
// 書式で、ルートが <ui name>）の読み書き。置き場の規則は scene/SharedAssetFile、
// 書式は ui/UiXml。いつ書くか（保存で変わったものだけ・共用にしたとき）は使う側
// （Scene::saveLibraryAssets と scene/LibraryFlow）。一覧の持ち主は EditorState
// （refreshLibraries が listLibrary を呼ぶ）。
namespace wizengine {
namespace uifiles {

// 共用 UI の置き場（assets/ 相対のフォルダ）と拡張子。
inline constexpr const char* kUiLibraryDir = "ui";
inline constexpr const char* kUiLibraryExt = ".xml";
// ライブラリの種類の名前（edit.lib.* の kind・ログ・覚え = LibraryBaseline の kind）。
inline constexpr const char* kUiLibraryKind = "ui";

// file のある宣言（asset.file）の中身をファイルから入れる。読めなければ中身は
// 空のまま（参照は残す = ファイルを戻して読み直せば元に戻る）で false と英語の
// reason。中身の警告は LOGW に出す。名前はシーンの側（asset.name）が正。
bool readSharedUi(ui::UiAssetDesc& asset, std::string& reason);

// 名前 name の共用 UI のファイル（assets/ 相対の "ui/<名前>.xml"）。名前が不正なら空。
std::string libraryFile(const std::string& name);
// 共用 UI のファイルに書く中身（ui::uiAssetToXmlText。保存で「変わったか」を比べる
// テキストもこれ = 読んだとき・書いたときと同じ作り方で比べる）。
std::string libraryText(const ui::UiAssetDesc& asset);
// 共用 UI を読んだ・書いた後に、その中身を変更の目印に覚える（唯一の入れ口。シーンの
// 読込・取込と、ライブラリから読み込む操作が通る）。
void rememberBaseline(LibraryBaseline& baseline, const ui::UiAssetDesc& asset);
// 共用 UI の「読めなかった」の印（UiState）を変更の目印から写す（印の持ち主は目印 = 記録が
// 無ければ読めない。計算式・プレハブと同じ規則）。目印を変えたら呼ぶ（読込・取込・
// ライブラリの操作の後）。
void syncReadable(UiState& state, const LibraryBaseline& baseline);
// assets/ui にある共用 UI の名前の一覧（拡張子を落とした名前、並べ替え済み）。
std::vector<std::string> listLibrary();

}  // namespace uifiles
}  // namespace wizengine
