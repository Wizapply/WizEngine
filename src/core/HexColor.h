#pragma once

#include <cstddef>
#include <string>

#include "core/Hex.h"

// "#rrggbb" の色の読み方（唯一の定義）。XML を知らない部品なので、JSON の色
// （ブラウザ API）と XML の色の文字列（core/ColorText.h）の両方が使える。
namespace wizengine {

// "#rrggbb" を 0〜1 の rgb に読む（sRGB ではなくリニア値をそのまま 0-255 から
// 写す）。形が違えば false で rgb は変えない。
inline bool parseHexColor(const std::string& hex, float rgb[3]) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = hexDigitValue(hex[std::size_t(i) + 1]);
        if (v[i] < 0) return false;
    }
    rgb[0] = float(v[0] * 16 + v[1]) / 255.0f;
    rgb[1] = float(v[2] * 16 + v[3]) / 255.0f;
    rgb[2] = float(v[4] * 16 + v[5]) / 255.0f;
    return true;
}

}  // namespace wizengine
