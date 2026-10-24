// テレインの筆。宣言と単位の約束は TerrainBrush.h。
#include "terrain/TerrainBrush.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace wizengine {
namespace terrain {
namespace {

// 1 スタンプで当てる円の数の上限（安全弁）。円の間隔は半径の 1/4 なので、
// 線分が半径の 64 倍を超えるときだけ間隔が広がる（隙間が出るのは 512 倍超）。
constexpr int kMaxDiscsPerStamp = 256;

// 当ててよいスタンプか（使う値が全部有限、半径が正、上限が負でない =
// std::clamp の lo <= hi を守る）。maxHeight を渡し忘れた（既定の NaN）
// スタンプもここで弾く。target は Flatten のときだけ見る。
bool usableStamp(const BrushStamp& s) {
    return std::isfinite(s.x0) && std::isfinite(s.z0) && std::isfinite(s.x1) &&
           std::isfinite(s.z1) && std::isfinite(s.radius) && std::isfinite(s.strength) &&
           std::isfinite(s.dt) && std::isfinite(s.maxHeight) &&
           (s.tool != BrushTool::Flatten || std::isfinite(s.target)) &&
           s.radius > 0.0 && s.maxHeight >= 0.0;
}

// 当てる円 1 個（格子の座標）。内外は二乗距離で比べ、平方根は重みを出すときだけ。
struct Disc {
    double cx, cz, r;
    double r2;  // r * r
    double distanceSq(double x, double z) const {
        const double dx = x - cx, dz = z - cz;
        return dx * dx + dz * dz;
    }
    bool contains(double x, double z) const { return distanceSq(x, z) < r2; }
    // 減衰の重み。半径の外は 0。
    double weight(double x, double z) const {
        const double d2 = distanceSq(x, z);
        return d2 < r2 ? brushFalloff(std::sqrt(d2), r) : 0.0;
    }
};

// 格子の座標の矩形 [xMin, xMax] x [zMin, zMax] に入るサンプル k を
// 角に持つセル（k − 1 と k）と、中心が入るセルを包むセルの矩形（切り詰め済み）。
// 格子の単位で lo..hi なら、サンプルは ceil(lo)..floor(hi) → セル ceil(lo) − 1..
// floor(hi)、中心が入るセルは ceil(lo − 0.5)..floor(hi − 0.5)。どちらも
// [floor(lo) − 1, floor(hi) + 1) に入る。
GridRect cellsOfBox(const HeightField& f, double xMin, double zMin, double xMax,
                    double zMax) {
    const double cell = f.cellSize();
    const double res = f.resolution();
    // int にする前に格子の少し外へ丸める（遠い座標で int があふれない）。
    auto toGrid = [&](double v, double origin) {
        return std::clamp((v - origin) / cell, -2.0, res + 2.0);
    };
    const GridRect r{int(std::floor(toGrid(xMin, f.minX()))) - 1,
                     int(std::floor(toGrid(zMin, f.minZ()))) - 1,
                     int(std::floor(toGrid(xMax, f.minX()))) + 1,
                     int(std::floor(toGrid(zMax, f.minZ()))) + 1};
    return r.clamped(f.resolution());
}

// 書き換えた高さを [0, maxHeight] に丸めて書く。変わったら true。元から範囲の
// 外にある値は、その向きへは押し戻さない（範囲を [min(0, 元), max(maxHeight, 元)]
// に広げる = 上限より高い既存の山を Raise / Smooth でなぞっても下がらない）。
bool writeHeight(HeightField& f, int i, int j, double h, double maxHeight) {
    const double old = f.sample(i, j);
    const float v = float(std::clamp(h, std::min(0.0, old), std::max(maxHeight, old)));
    if (v == f.sample(i, j)) return false;
    f.setSample(i, j, v);
    return true;
}

// 円の中のサンプル（cells の角 = [i0, i1] x [j0, j1]）ごとに fn(i, j, 番号, 重み)。
// 番号は矩形内の行優先の通し番号（Smooth の写しの添字）。重み 0 は呼ばない。
template <class Fn>
void forEachWeighted(const HeightField& f, const Disc& disc, const GridRect& cells, Fn&& fn) {
    const double cell = f.cellSize();
    std::size_t k = 0;
    for (int j = cells.j0; j <= cells.j1; ++j) {
        const double z = double(f.minZ()) + double(j) * cell;
        for (int i = cells.i0; i <= cells.i1; ++i, ++k) {
            const double w = disc.weight(double(f.minX()) + double(i) * cell, z);
            if (w > 0.0) fn(i, j, k, w);
        }
    }
}

// Raise / Lower / Flatten。隣を読まないのでその場で書く。amount = strength × dt。
bool raiseLowerFlatten(HeightField& f, const BrushStamp& s, const Disc& disc,
                       const GridRect& cells, double amount) {
    const double rate = std::min(1.0, amount);  // Flatten の寄せ率
    const double flatTarget = s.target - double(f.baseY());
    bool changed = false;
    forEachWeighted(f, disc, cells, [&](int i, int j, std::size_t, double w) {
        const double h = f.sample(i, j);
        double next = h;
        switch (s.tool) {
            case BrushTool::Raise: next = h + amount * w; break;
            case BrushTool::Lower: next = h - amount * w; break;
            case BrushTool::Flatten: next = h + (flatTarget - h) * rate * w; break;
            default: break;
        }
        changed = writeHeight(f, i, j, next, s.maxHeight) || changed;
    });
    return changed;
}

// Smooth（周り 3x3 の平均へ寄せる）。書き換え途中の隣を読んで片寄らないよう、
// 新しい値を写し（scratch、呼び手が使い回す）へ全部出してから書く。
bool smooth(HeightField& f, const BrushStamp& s, const Disc& disc, const GridRect& cells,
            double amount, std::vector<float>& scratch) {
    const double rate = std::min(1.0, amount);
    constexpr float kUntouched = std::numeric_limits<float>::quiet_NaN();
    scratch.assign(std::size_t(cells.i1 - cells.i0 + 1) * std::size_t(cells.j1 - cells.j0 + 1),
                   kUntouched);
    forEachWeighted(f, disc, cells, [&](int i, int j, std::size_t k, double w) {
        double sum = 0.0;  // 端は sample が端へ丸める
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) sum += f.sample(i + di, j + dj);
        const double h = f.sample(i, j);
        scratch[k] = float(h + (sum / 9.0 - h) * rate * w);
    });
    bool changed = false;
    std::size_t k = 0;
    for (int j = cells.j0; j <= cells.j1; ++j)
        for (int i = cells.i0; i <= cells.i1; ++i, ++k)
            if (!std::isnan(scratch[k]))
                changed = writeHeight(f, i, j, scratch[k], s.maxHeight) || changed;
    return changed;
}

// Hole / Unhole。中心が円の中のセルを書き換える。
bool holes(HeightField& f, bool hole, const Disc& disc, const GridRect& cells) {
    const double cell = f.cellSize();
    bool changed = false;
    for (int j = cells.j0; j < cells.j1; ++j) {
        const double z = double(f.minZ()) + (double(j) + 0.5) * cell;
        for (int i = cells.i0; i < cells.i1; ++i) {
            if (!disc.contains(double(f.minX()) + (double(i) + 0.5) * cell, z)) continue;
            // 変わったかを知るため、同じ値なら書かない。
            if (f.holeAt(i, j) == hole) continue;
            f.setHole(i, j, hole);
            changed = true;
        }
    }
    return changed;
}

bool applyDisc(HeightField& f, const BrushStamp& s, const Disc& disc, double dt,
               std::vector<float>& scratch) {
    const GridRect cells =
        cellsOfBox(f, disc.cx - disc.r, disc.cz - disc.r, disc.cx + disc.r, disc.cz + disc.r);
    if (cells.empty()) return false;
    if (s.tool == BrushTool::Hole || s.tool == BrushTool::Unhole)
        return holes(f, s.tool == BrushTool::Hole, disc, cells);
    // 強さか dt が 0 = 高さを動かさない（範囲外の値を丸めるだけの書き込みもしない）。
    const double amount = s.strength * dt;
    if (!(amount > 0.0)) return false;
    if (s.tool == BrushTool::Smooth) return smooth(f, s, disc, cells, amount, scratch);
    return raiseLowerFlatten(f, s, disc, cells, amount);
}

}  // namespace

double brushFalloff(double d, double r) {
    const double t = std::clamp(d / r, 0.0, 1.0);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

BrushTool brushToolFromName(const std::string& name, BrushTool fallback) {
    if (name == "raise") return BrushTool::Raise;
    if (name == "lower") return BrushTool::Lower;
    if (name == "flatten") return BrushTool::Flatten;
    if (name == "smooth") return BrushTool::Smooth;
    if (name == "hole") return BrushTool::Hole;
    if (name == "unhole") return BrushTool::Unhole;
    return fallback;
}

BrushStamp clampBrush(BrushStamp s) {
    // std::clamp / std::max は NaN をそのまま返す = NaN は残る（ヘッダの約束）。
    s.radius = std::clamp(s.radius, kBrushMinRadius, kBrushMaxRadius);
    s.strength = std::clamp(s.strength, 0.0, kBrushMaxStrength);
    s.dt = std::clamp(s.dt, 0.0, kBrushMaxDt);
    s.maxHeight = std::max(s.maxHeight, 0.0);
    return s;
}

GridRect stampRect(const HeightField& field, const BrushStamp& stamp) {
    if (!field.valid() || !usableStamp(stamp)) return {};
    const double r = stamp.radius;
    return cellsOfBox(field, std::min(stamp.x0, stamp.x1) - r, std::min(stamp.z0, stamp.z1) - r,
                      std::max(stamp.x0, stamp.x1) + r, std::max(stamp.z0, stamp.z1) + r);
}

bool applyStamp(HeightField& field, const BrushStamp& stamp) {
    if (!field.valid() || !usableStamp(stamp)) return false;
    const double dx = stamp.x1 - stamp.x0;
    const double dz = stamp.z1 - stamp.z0;
    const double length = std::hypot(dx, dz);
    // 始点は前回のスタンプが当て済み = 1..n 番目の点（終点を含む）に当てる。
    const double spacing = stamp.radius * 0.25;
    const int discs =
        std::clamp(int(std::ceil(std::min(length / spacing, double(kMaxDiscsPerStamp)))), 1,
                   kMaxDiscsPerStamp);
    const double dt = stamp.dt / double(discs);
    std::vector<float> scratch;  // Smooth の写し（円ごとに使い回す）
    bool changed = false;
    for (int k = 1; k <= discs; ++k) {
        const double t = double(k) / double(discs);
        const Disc disc{stamp.x0 + dx * t, stamp.z0 + dz * t, stamp.radius,
                        stamp.radius * stamp.radius};
        changed = applyDisc(field, stamp, disc, dt, scratch) || changed;
    }
    return changed;
}

}  // namespace terrain
}  // namespace wizengine
