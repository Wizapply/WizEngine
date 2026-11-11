#pragma once

#include <string>

#include "ui/UiTypes.h"

// 共用 UI のファイル（assets/ui/<名前>.xml。中身はシーンの <asset> の <ui> と同じ
// 書式で、ルートが <ui name>）の読み書き。置き場の規則は scene/SharedAssetFile、
// 書式は ui/UiXml。今は読む側だけ（書く側・ライブラリの一覧は後の PR）。
namespace wizengine {
namespace uifiles {

// 共用 UI の置き場（assets/ 相対のフォルダ）と拡張子。
inline constexpr const char* kUiLibraryDir = "ui";
inline constexpr const char* kUiLibraryExt = ".xml";

// file のある宣言（asset.file）の中身をファイルから入れる。読めなければ中身は
// 空のまま（参照は残す = ファイルを戻して読み直せば元に戻る）で false と英語の
// reason。中身の警告は LOGW に出す。名前はシーンの側（asset.name）が正。
bool readSharedUi(ui::UiAssetDesc& asset, std::string& reason);

}  // namespace uifiles
}  // namespace wizengine
