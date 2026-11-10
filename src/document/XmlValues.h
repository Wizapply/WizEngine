#pragma once

#include <string>

#include "document/EditorTypes.h"
#include "document/SceneXml.h"

// シーン文書の値の表記（属性文字列 ⇄ 値）のうち、シーン文書（SceneDocument.cpp）
// と UI の性質（ui/UiValue.cpp）が共用するもの。表記の規則はここ 1 か所に書く。
namespace wizengine {
namespace editor {

// 色を書くときの有効桁（float 由来なので 6 桁で十分）。
inline constexpr int kColorPrecision = 6;

// 色の表記の規則（唯一の定義）: 空白区切りの数値が 3 個以上なら r g b [a]
// （MuJoCo と同じ rgba、リニア値。a を書かなければ 1）、そうでなければブラウザ
// API と同じ "#rrggbb"（a = 1。手書きしやすい）。どちらでもなければ false で
// out は変えない。数値の読み方は xml::Element の数値列と同じ。
inline bool parseColorText(const std::string& text, double out[4]) {
    xml::Element e;
    e.set("v", text);
    double a[4] = {0.0, 0.0, 0.0, 1.0};
    if (e.numbers("v", a, 4) < 3) {
        const Color3 bad{-1.0f, -1.0f, -1.0f};
        const Color3 c = colorFromHex(text, bad);
        if (c.r < 0.0f) return false;
        a[0] = c.r;
        a[1] = c.g;
        a[2] = c.b;
        a[3] = 1.0;
    }
    for (int k = 0; k < 4; ++k) out[k] = a[k];
    return true;
}

// 色は MuJoCo と同じ rgba="r g b a"（リニア値）。a は今は使わないが、
// MJCF を見慣れた目に自然な形にしておく。
inline void setColor(xml::Element& e, const char* key, const Color3& c) {
    const double a[4] = {c.r, c.g, c.b, 1.0};
    e.setNumbers(key, a, 4, kColorPrecision);
}

inline Color3 getColor(const xml::Element& e, const char* key, const Color3& fallback) {
    double a[4];
    if (!e.has(key) || !parseColorText(e.attr(key), a)) return fallback;
    Color3 c;
    c.r = float(a[0]);
    c.g = float(a[1]);
    c.b = float(a[2]);
    return c;
}

}  // namespace editor
}  // namespace wizengine
