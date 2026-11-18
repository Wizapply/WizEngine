#pragma once

#include <cstddef>
#include <string>

// ステータスの末尾に付ける注意（括弧書きの日本語 1 文。唯一の定義）。scene の担当が
// 共通に使う。Chrono も Filament も読まない（UiOps / UiLibraryOps のような純粋な部品からも
// 読める）。文を変えるきっかけは「注意の言い方を変える」だけ。
namespace wizengine {

// 件数の注意「（<what> N 件<tail> - コンソール参照）」（唯一の形）。0 件なら空。what は何の
// 件数か（"警告" / "共用 UI" など）、tail はその後に続ける述語（"のファイルが読めません" など。
// 空でよい）。
inline std::string countCaveat(const std::string& what, std::size_t count,
                               const std::string& tail = {}) {
    if (count == 0) return {};
    return "（" + what + " " + std::to_string(count) + " 件" + tail + " - コンソール参照）";
}

// 警告の件数「（<label> N 件 - コンソール参照）」。0 件なら空。label は "警告" /
// "テレインの警告" など。
inline std::string warningCountCaveat(const char* label, std::size_t count) {
    return countCaveat(label, count);
}

// 書いた名前が正規化（sanitizeSafeName の規則）で全部落ちたとき（日本語だけなど）。
inline const char* unusableNameCaveat() {
    return "（名前に使えるのは英数字と _ - だけです。その名前は使っていません）";
}

}  // namespace wizengine
