#pragma once

#include <string>

#include "core/ColorText.h"
#include "core/Xml.h"
#include "core/XmlWarnings.h"
#include "document/EditorTypes.h"

// シーン文書の値（Vec3d・Color3）の属性の読み書き。表記の規則（色の文字列）と
// 警告の流儀は core（ColorText.h・XmlWarnings.h）が持ち、ここは文書の型への
// 当てはめだけを書く。
namespace wizengine {
namespace editor {

// core へ移した部品の旧名（editor:: の呼び手のため。定義は core の 1 か所だけ）。
// 警告の流儀は core/XmlWarnings.h、色の表記は core/ColorText.h。
using xml::unknownName;
using xml::Warn;
using xml::warnUnknownChildren;
using wizengine::kColorPrecision;
using wizengine::parseColorText;

// ---- 属性の読み書き（Vec3 と色）--------------------------------------------
inline void setVec3(xml::Element& e, const char* key, const Vec3d& v) {
    const double a[3] = {v.x, v.y, v.z};
    e.setNumbers(key, a, 3);
}
inline Vec3d getVec3(const xml::Element& e, const char* key, const Vec3d& fallback) {
    double a[3] = {fallback.x, fallback.y, fallback.z};
    e.numbers(key, a, 3);
    return Vec3d{a[0], a[1], a[2]};
}

// 色は rgba="r g b a"（リニア値）で書く。a は今は使わないが、4 つ並べて
// 書くのが文書の色の書き方なので、1 を入れておく。
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
