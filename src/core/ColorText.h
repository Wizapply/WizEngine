#pragma once

#include <string>

#include "core/HexColor.h"
#include "core/Xml.h"

// 色の文字列の書き方（シーン文書と UI の文書が共用する唯一の定義）。"#rrggbb" の
// 読み方は core/HexColor.h。
namespace wizengine {

// 色を書くときの有効桁（float 由来なので 6 桁で十分）。
inline constexpr int kColorPrecision = 6;

// 色の表記の規則（唯一の定義）: 空白区切りの数値が 3 個以上なら r g b [a]
// （rgba のリニア値。a を書かなければ 1）、そうでなければブラウザ
// API と同じ "#rrggbb"（a = 1。手書きしやすい）。どちらでもなければ false で
// out は変えない。数値の読み方は xml::parseNumbers（Element の数値列と同じ）。
inline bool parseColorText(const std::string& text, double out[4]) {
    double a[4] = {0.0, 0.0, 0.0, 1.0};
    if (xml::parseNumbers(text, a, 4) < 3) {
        float rgb[3];
        if (!parseHexColor(text, rgb)) return false;
        a[0] = rgb[0];
        a[1] = rgb[1];
        a[2] = rgb[2];
        a[3] = 1.0;
    }
    for (int k = 0; k < 4; ++k) out[k] = a[k];
    return true;
}

}  // namespace wizengine
