#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "scene/LibraryFlow.h"
#include "scene/UiState.h"

// グローバル（ライブラリ）の UI の、種類ごとに違うところ（edit.lib.* の kind = ui）。
// 共用にする・ローカルにする・読み込む・削除の判断の流れは計算式・プレハブと同じ 1 本
// （scene/LibraryFlow の applyLibraryFlow）で、ここはその流れに渡す UI の部品だけを持つ
// （libraryKind が組む）: どの UI か・中身のファイルの書式・UiState への書き込み。
// 直すきっかけは「共用 UI のファイルとの行き来を変える」だけ（要素の編集は UiOps）。
//
// 共用にする対象は 2 通り: 名前で指した宣言（<ui name>）か、canvas（args の canvas）。
// canvas が UI を使っていればその UI、自分の中身を持つ Canvas なら、その中身を新しい
// UI にする（Canvas はその UI を使う形に変わる。要素の id はそのまま = 選択と映像の
// 上のドラッグは同じ要素を指し続ける）。canvas を渡したのにその Canvas が消えていれば
// 何もしない（名前で探し直さない = 打った名前の別の UI を共用にしない）。
//
// Chrono / Filament / Scene を知らない（tests/document で単体で検査する）。
namespace wizengine {
namespace uilibrary {

// ステータスの種類の呼び名（scene/LibraryStatus.h の label）と、読み込んだ後の案内。
inline constexpr const char* kLabel = "UI";
inline constexpr const char* kImportHint = "タイルをクリックすると Canvas に表示します";

// 判断の流れ（applyLibraryFlow）に渡す UI の部品の組。args は送られた操作の引数（canvas を
// 読む）、baseline は読み込んだときに覚えを入れる先。state と baseline は返した組を使い
// 終わるまで生きていること。
LibraryKind libraryKind(UiState& state, const nlohmann::json& args, LibraryBaseline& baseline);

// ---- 部品（libraryKind が組む。tests/document が直接検査する）----------------
// 共用にする対象を決める。name は送られた名前（正規化済み。空もありうる）で、canvas を
// 渡されたときは決めた名前に書き換える。見つかれば true と今の file（空 = ローカル）。
// 断るときは false と refusal（ステータス）: 自分の中身の Canvas から作る名前が既に宣言に
// ある・送った名前が正規化で全部落ちた。canvas を渡したのに消えていれば false（refusal は空）。
bool findForShare(const UiState& state, const nlohmann::json& args, std::string& name,
                  std::string& file, std::string& refusal);
// 宣言に name の UI があれば true と今の file。
bool find(const UiState& state, const std::string& name, std::string& file);
// 共用にしたときにファイルへ書く中身（file を付けた形。findForShare と同じ対象）。
std::string sharedText(const UiState& state, const nlohmann::json& args,
                       const std::string& name, const std::string& file);
// 共用にした（ファイルは書いた）ことを UiState へ入れる（findForShare と同じ対象）。
// 入れられなければ false（対象が消えた）。
bool commitShare(UiState& state, const nlohmann::json& args, const std::string& name,
                 const std::string& file);
// ローカルにする（参照をやめ、中身はシーンに残す）。読めなかった UI を断るのは判断の流れ
// （applyLibraryFlow。3 種類とも同じ規則）。
bool unshare(UiState& state, const std::string& name);
// ライブラリの file を読んで宣言に足し、覚え（baseline）を入れる。読めなければ false と
// 英語の reason。
bool importFromLibrary(UiState& state, const std::string& name, const std::string& file,
                       LibraryBaseline& baseline, std::string& reason);

}  // namespace uilibrary
}  // namespace wizengine
