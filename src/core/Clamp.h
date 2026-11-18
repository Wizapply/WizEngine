#pragma once

// 数値を範囲に収める・整数へ丸める（どの層からも使える小さな部品）。

#include <climits>
#include <cmath>

namespace wizengine {

// 範囲へ丸める。NaN は fallback へ戻す（XML の strtod は "nan" も読むので、比較だけの
// 丸めでは素通りしてしまう）。テレインの値・UI の性質の値などが共用する。
inline double clampOrFallback(double v, double lo, double hi, double fallback) {
    if (!(v == v)) return fallback;
    return v < lo ? lo : (v > hi ? hi : v);
}

// 整数へ丸める（0.5 は 0 から遠い方）。int に入らない値と NaN は fallback（double から
// int への変換は範囲外で未定義）。XML の整数属性と UI の整数の値が共用する唯一の定義。
inline int roundToInt(double v, int fallback) {
    const double r = std::round(v);
    const bool inRange = r >= double(INT_MIN) && r <= double(INT_MAX);  // NaN は偽
    return inRange ? int(r) : fallback;
}

}  // namespace wizengine
