#pragma once

#include <cstddef>
#include <string>

namespace wizengine {

// 表示できない字の代わりに使う字（U+FFFD）。不正なバイト列はこれになる。
inline constexpr char32_t kReplacementChar = 0xFFFD;

// UTF-8 の文字列 s の i バイト目から 1 字読み、そのコードポイントを返して
// i を次の字の先頭へ進める。不正なバイト列（途中で切れた列・続きのバイトが
// 先頭に来た・冗長な表現・サロゲート・U+10FFFF 超）は U+FFFD を返して 1 バイト
// だけ進む = どんな入力でも必ず先へ進み、続く正しい字はそのまま読める。
// 呼ぶ前に i < s.size() であること。
inline char32_t nextCodepoint(const std::string& s, std::size_t& i) {
    const auto byte = [&s](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char b0 = byte(i);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    std::size_t len = 0;
    char32_t cp = 0;
    char32_t minCp = 0;  // この長さで表すべき最小値（冗長な表現を弾く）
    if ((b0 & 0xE0) == 0xC0) {
        len = 2; cp = b0 & 0x1F; minCp = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3; cp = b0 & 0x0F; minCp = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4; cp = b0 & 0x07; minCp = 0x10000;
    } else {
        ++i;  // 続きのバイト（10xxxxxx）が先頭に来た・0xF8 以上
        return kReplacementChar;
    }
    if (i + len > s.size()) {
        ++i;
        return kReplacementChar;
    }
    for (std::size_t k = 1; k < len; ++k) {
        const unsigned char b = byte(i + k);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return kReplacementChar;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    const bool overlong = cp < minCp;
    const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    const bool tooLarge = cp > 0x10FFFF;
    if (overlong || surrogate || tooLarge) {
        ++i;
        return kReplacementChar;
    }
    i += len;
    return cp;
}

// 文字列全体をコードポイントの列にする（不正なバイト列は U+FFFD）。
inline std::u32string decodeUtf8(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) out.push_back(nextCodepoint(s, i));
    return out;
}

}  // namespace wizengine
