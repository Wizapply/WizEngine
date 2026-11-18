#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

// ファイルのバイト列の読み書き（.terrain・ハイトマップの RAW / PNG・UI のフォントが
// 共用）。readFileBytes は maxBytes を超えるファイルを読まずに断る（壊れた・別の
// 大きなファイルを丸ごと読まない安全弁）。writeFileBytes は一時ファイルに書いてから
// 置き換え（途中で失敗しても元のファイルは壊れない）、親フォルダが無ければ作る。
// readFileHead は先頭から maxBytes バイトまでを読む（短いファイルは全部。画像の
// 形式の見分けや見出しだけを読むとき）。読みはどちらも大きさを先に取り、その
// 長さが読めなければ失敗（読む途中で置き換わったファイル）。空のファイルは
// 「成功・0 バイト」なので、空を断るのは呼ぶ側。失敗したとき out は触らない
// （前の中身が残る。同じ配列を使い回す呼ぶ側は自分で消す）。
// std::string の UTF-8 のパスは utf8Path で直して渡す（存在の確かめも同じ）。
// reason には英語の理由とパスが付く。
namespace wizengine {

// 「上限なし」（readFileBytes / readFileHead の maxBytes）。
inline constexpr std::uintmax_t kNoByteLimit = std::numeric_limits<std::uintmax_t>::max();

// UTF-8 の文字列のパスを std::filesystem::path に直す（Windows でも日本語などを
// 含むパスが開ける）。定義を FileBytes.cpp（C++17）に置くのは、C++20 で非推奨の
// std::filesystem::u8path の警告を呼ぶ側の翻訳単位に広げないため。
std::filesystem::path utf8Path(const std::string& path);

bool readFileBytes(const std::filesystem::path& path, std::uintmax_t maxBytes,
                   std::vector<std::uint8_t>& out, std::string& reason);
bool readFileHead(const std::filesystem::path& path, std::uintmax_t maxBytes,
                  std::vector<std::uint8_t>& out, std::string& reason);
bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                    std::string& reason);

}  // namespace wizengine
