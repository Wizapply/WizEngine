#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ファイルのバイト列の読み書き（.terrain・ハイトマップの RAW / PNG・UI のフォントが
// 共用）。readFileBytes は maxBytes を超えるファイルを読まずに断る（壊れた・別の
// 大きなファイルを丸ごと読まない安全弁）。writeFileBytes は一時ファイルに書いてから
// 置き換え（途中で失敗しても元のファイルは壊れない）、親フォルダが無ければ作る。
// reason には英語の理由とパスが付く。
namespace wizengine {

bool readFileBytes(const std::filesystem::path& path, std::uintmax_t maxBytes,
                   std::vector<std::uint8_t>& out, std::string& reason);
bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                    std::string& reason);

}  // namespace wizengine
