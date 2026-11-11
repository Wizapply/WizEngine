#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <algorithm>

#include "core/SafeName.h"

// 共用ファイル（シーンをまたいで使うアセット = assets/<サブフォルダ>/<名前>.<拡張子>）
// の原始関数。イベントライブラリ（assets/events/*.xml）と共用 UI
// （assets/ui/*.xml）が使う。置き場の規則（assets/ 相対のパスだけ・名前は
// sanitizeSafeName で変わらないものだけ・ごみ箱へ移す）はここ 1 か所。
// 共通化・読み込み・削除といった流れ（何をいつ書くか、ステータスの文）は
// 使う側（SceneEvents.cpp / 後の UiFiles・UiOps）に残す。
namespace wizengine {
namespace sharedasset {

// テキストファイル（path はフルパス）の読み書き（唯一の定義。シーンの保存・読込、
// 旧 JSON の読み込み、共用ファイルが使う）。書くときはフォルダが無ければ作る。
// 失敗時は false と英語の reason。中身が XML か JSON かはここでは見ない。
bool readTextFile(const std::string& path, std::string& text, std::string& reason);
bool writeTextFile(const std::string& path, const std::string& text, std::string& reason);

// assets/ 相対の file を読む。assets/ の外を指すパス・読めないファイルは false と
// 英語の reason。
bool readAssetText(const std::string& file, std::string& text, std::string& reason);
// assets/ 相対の file へ書く（フォルダが無ければ作る）。
bool writeAssetText(const std::string& file, const std::string& text, std::string& reason);
// ライブラリのファイル（assets/ 相対の "<subdir>/<名前><ext>"）。名前が不正なら空。
std::string libraryFile(const std::string& subdir, const std::string& name,
                        const std::string& ext);
// assets/<subdir> にある <名前><ext> の一覧（拡張子を落とした名前、並べ替え済み）。
std::vector<std::string> listLibrary(const std::string& subdir, const std::string& ext);
// ライブラリのファイル（assets/ 相対）を OS のごみ箱へ。
bool trashLibraryFile(const std::string& file, std::string& reason);

// dir の中の <名前><ext> の一覧（listLibrary の中身。フォルダを直接受けるので
// 単体で検査できる）。名前に使えない文字のファイル（手で置いた物）は出さない
// （参照名 = ファイル名の語幹、の約束が崩れるため）。
inline std::vector<std::string> listLibraryIn(const std::filesystem::path& dir,
                                              const std::string& ext) {
    std::vector<std::string> found;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return found;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension().string() != ext) continue;
        const std::string stem = entry.path().stem().string();
        if (!isSafeName(stem)) continue;
        found.push_back(stem);
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace sharedasset
}  // namespace wizengine
