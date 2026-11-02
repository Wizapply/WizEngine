#pragma once

#include <algorithm>
#include <cmath>

// src/terrain/ の中で共用する小さな数値の規則。
namespace wizengine {
namespace terrain {

// 有限なら v を [lo, hi] へ、そうでなければ fallback（受け取った設定値の丸め）。
inline double clampOr(double v, double lo, double hi, double fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

}  // namespace terrain
}  // namespace wizengine
