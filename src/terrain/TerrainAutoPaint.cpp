#include "terrain/TerrainAutoPaint.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace wizengine {
namespace terrain {

namespace {

// ラジアン → 度（数学の定数。傾きを度で比べるためだけに使う）。
constexpr double kDegPerRad = 57.295779513082320876798;
// 傾きの取りうる範囲（度）。
constexpr double kSlopeLo = 0.0;
constexpr double kSlopeHi = 90.0;

// 有限なら v を [lo, hi] へ、そうでなければ fallback。
double clampOr(double v, double lo, double hi, double fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

// 0 → 1 のなめらかな段（e0 < e1 であること）。
double smoothStep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// 値 v が帯 [lo, hi] に入っている度合い（0〜1）。[rangeLo, rangeHi] は v の
// 取りうる範囲。帯の端が範囲の端か外にあればその側はぼかさない（外側も 1 のまま）。
// 縁は、範囲に切り詰めた帯の幅 × softness の範囲（縁を中心に両側へ半分ずつ）で
// なめらかにつなぐ。ただし半幅は縁から範囲の端までの距離で頭打ち（帯の外へ
// 広がりすぎない）。半幅 0（softness 0・幅 0）はくっきり（両端を含む）。
double bandFactor(double v, double lo, double hi, double softness, double rangeLo,
                  double rangeHi) {
    const double width = std::max(0.0, std::min(hi, rangeHi) - std::max(lo, rangeLo));
    const double half = 0.5 * softness * width;
    double f = 1.0;
    if (lo > rangeLo) {
        const double w = std::min(half, lo - rangeLo);
        f *= w > 0.0 ? smoothStep(lo - w, lo + w, v) : (v >= lo ? 1.0 : 0.0);
    }
    if (hi < rangeHi) {
        const double w = std::min(half, rangeHi - hi);
        f *= w > 0.0 ? 1.0 - smoothStep(hi - w, hi + w, v) : (v <= hi ? 1.0 : 0.0);
    }
    return f;
}

}  // namespace

AutoPaintRules defaultAutoPaintRules() {
    AutoPaintRules r;
    // 層 0 = 平地、層 1 = 斜面、層 2 = 崖、層 3 = 使わない（高さでは絞らない）。
    r.layer[0].enabled = true;
    r.layer[0].slopeMinDeg = 0.0;
    r.layer[0].slopeMaxDeg = 30.0;
    r.layer[1].enabled = true;
    r.layer[1].slopeMinDeg = 30.0;
    r.layer[1].slopeMaxDeg = 50.0;
    r.layer[2].enabled = true;
    r.layer[2].slopeMinDeg = 50.0;
    r.layer[2].slopeMaxDeg = 90.0;
    r.layer[3].enabled = false;
    return r;
}

AutoPaintRules clampAutoPaintRules(AutoPaintRules rules) {
    const AutoPaintRules def = defaultAutoPaintRules();
    for (std::size_t k = 0; k < rules.layer.size(); ++k) {
        AutoPaintBand& b = rules.layer[k];
        const AutoPaintBand& d = def.layer[k];
        b.slopeMinDeg = clampOr(b.slopeMinDeg, kSlopeLo, kSlopeHi, d.slopeMinDeg);
        b.slopeMaxDeg = clampOr(b.slopeMaxDeg, kSlopeLo, kSlopeHi, d.slopeMaxDeg);
        b.heightMin = clampOr(b.heightMin, -kAutoPaintMaxHeight, kAutoPaintMaxHeight, d.heightMin);
        b.heightMax = clampOr(b.heightMax, -kAutoPaintMaxHeight, kAutoPaintMaxHeight, d.heightMax);
        if (b.slopeMinDeg > b.slopeMaxDeg) std::swap(b.slopeMinDeg, b.slopeMaxDeg);
        if (b.heightMin > b.heightMax) std::swap(b.heightMin, b.heightMax);
    }
    rules.softness = clampOr(rules.softness, 0.0, 1.0, def.softness);
    return rules;
}

bool applyAutoPaint(HeightField& field, const AutoPaintRules& rules) {
    if (!field.valid()) return false;
    const int res = field.resolution();
    const double cell = field.cellSize();
    const double minX = field.minX();
    const double minZ = field.minZ();
    const double baseY = field.baseY();
    // 高さの取りうる範囲 = 格子の最低〜最高（基準 baseY からの m。傾きは 0〜90°）。
    const double hLo = field.minSample();
    const double hHi = field.maxSample();
    bool changed = false;
    float w[kSplatChannels];
    for (int j = 0; j < res; ++j) {
        const double z0 = minZ + double(j) * cell;
        for (int i = 0; i < res; ++i) {
            const double x0 = minX + double(i) * cell;
            // 傾き: セルの 2 三角形の重心で面法線を取り、平均する（中心は対角線の
            // 上 = 片方の三角形だけになるため）。三角形 A は w > u、B は u > w の側。
            const Vec3 n = field.normalAt(x0 + cell / 3.0, z0 + cell * 2.0 / 3.0) +
                           field.normalAt(x0 + cell * 2.0 / 3.0, z0 + cell / 3.0);
            const double len = n.norm();
            const double ny = len > 0.0 ? std::clamp(n.y() / len, -1.0, 1.0) : 1.0;
            const double slope = std::acos(ny) * kDegPerRad;
            const double height =
                field.heightAt(field.cellCenterX(i), field.cellCenterZ(j)) - baseY;
            for (int k = 0; k < kSplatChannels; ++k) {
                const AutoPaintBand& b = rules.layer[std::size_t(k)];
                if (!b.enabled) {
                    w[k] = 0.0f;
                    continue;
                }
                const double fs = bandFactor(slope, b.slopeMinDeg, b.slopeMaxDeg, rules.softness,
                                             kSlopeLo, kSlopeHi);
                const double fh = bandFactor(height, b.heightMin, b.heightMax, rules.softness,
                                             hLo, hHi);
                w[k] = float(fs * fh);
            }
            // 正規化・量子化（合計 255、全部 0 なら層 0）と穴のセルを飛ばすのは
            // setSplatWeights だけ。
            if (field.setSplatWeights(i, j, w)) changed = true;
        }
    }
    return changed;
}

}  // namespace terrain
}  // namespace wizengine
