#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "core/Hex.h"

namespace wizengine {

// 背番号（16 バイト）と 32 桁の hex 表記の相互変換。規則はここ 1 か所:
// 読むときは大文字も受け、書くときは小文字。terrain 層（TerrainGuid）と
// document 層（TerrainDesc::guid の正規化）がどちらもこれを使う。
using GuidBytes = std::array<std::uint8_t, 16>;

// 32 桁の hex から。長さ違い・hex 以外の文字は false で out は触らない。
inline bool guidBytesFromHex(const std::string& hex, GuidBytes& out) {
    if (hex.size() != 32) return false;
    GuidBytes b{};
    for (std::size_t k = 0; k < 16; ++k) {
        const int hi = hexDigitValue(hex[2 * k]);
        const int lo = hexDigitValue(hex[2 * k + 1]);
        if (hi < 0 || lo < 0) return false;
        b[k] = std::uint8_t((hi << 4) | lo);
    }
    out = b;
    return true;
}

// 全部 0 = 空（背番号を持たない）。
inline bool guidBytesEmpty(const GuidBytes& bytes) { return bytes == GuidBytes{}; }

// 32 桁の小文字 hex。
inline std::string guidBytesToHex(const GuidBytes& bytes) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (const std::uint8_t b : bytes) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 0xf]);
    }
    return s;
}

// hex 表記をそろえる: 読めて空でなければ小文字の 32 桁、不正（長さ違い・
// hex 以外の文字）か全部 0 なら空文字列。
inline std::string normalizedGuidHex(const std::string& hex) {
    GuidBytes bytes{};
    if (!guidBytesFromHex(hex, bytes) || guidBytesEmpty(bytes)) return std::string();
    return guidBytesToHex(bytes);
}

}  // namespace wizengine
