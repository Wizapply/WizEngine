// HeightField の「形を変えた写し」を作る操作（大きさ・位置の変更、解像度の
// 変更、高さの比例伸縮と切り詰め）と、編集が描画に及ぶ範囲（chunksTouched）。
// 宣言は HeightField.h。HeightField.cpp を問い合わせと
// メッシュ化に絞っておくため、ファイルを分けてある。
//
// 共有中の格子（Scene が持つ shared_ptr<const HeightField>）は書き換えない約束
// なので、withGeometry / resampled は新しい値を返し、scaleHeights は呼び手が
// 作ったコピーに対して呼ぶ。
#include "terrain/HeightField.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace wizengine {
namespace terrain {
namespace {

// 解像度を変えたとき、新しいセル n の中心を含む元のセル（1 軸ぶん、最近傍）。
// ratio = 元のセル数 / 新しいセル数。穴の拡大と重みの再サンプル（縮小・拡大
// とも）が使う。
int nearestCell(int n, double ratio, int srcRes) {
    return std::clamp(int(std::floor((n + 0.5) * ratio)), 0, srcRes - 1);
}

// 解像度を変えたとき、新しいセル n が元のどのセル [lo, hi] から穴を受けるか
// （1 軸ぶん）。ratio = 元のセル数 / 新しいセル数。
//   縮小（ratio > 1）: 新しいセルと重なる元のセル全部（OR を取る = 1 セルの
//                     穴も消えない）。
//   拡大（ratio <= 1）: 新しいセルの中心を含む元のセル 1 つ（最近傍）。
// 境界ちょうどの重なり（面積 0）を拾わないよう、わずかに内側で丸める。
void sourceCells(int n, double ratio, int srcRes, int& lo, int& hi) {
    constexpr double kEps = 1e-9;
    if (ratio > 1.0) {
        lo = int(std::floor(n * ratio + kEps));
        hi = int(std::ceil((n + 1) * ratio - kEps)) - 1;
    } else {
        lo = hi = nearestCell(n, ratio, srcRes);
    }
    lo = std::clamp(lo, 0, srcRes - 1);
    hi = std::clamp(hi, lo, srcRes - 1);
}

}  // namespace

void HeightField::scaleHeights(float k) {
    // 0 や負で地形が潰れたり裏返ったりしないよう、正の有限値だけ受ける。
    if (!std::isfinite(k) || !(k > 0.0f)) return;
    for (float& h : heights_) h *= k;
}

std::size_t HeightField::clampHeights(float maxHeight) {
    // scaleHeights と同じく、正の有限値だけ受ける（0 で全部を平らに潰さない）。
    if (!std::isfinite(maxHeight) || !(maxHeight > 0.0f)) return 0;
    const std::size_t clipped = countAbove(maxHeight);
    if (clipped == 0) return 0;
    for (float& h : heights_) h = std::min(h, maxHeight);
    return clipped;
}

std::size_t HeightField::countAbove(float maxHeight) const {
    return std::size_t(std::count_if(heights_.begin(), heights_.end(),
                                     [maxHeight](float h) { return h > maxHeight; }));
}

HeightField HeightField::withGeometry(float size, float centerX, float baseY,
                                      float centerZ) const {
    if (!valid()) return {};
    HeightField out(resolution_, size, centerX, baseY, centerZ);
    if (!out.valid()) return out;  // 引数が不正 = 空
    out.heights_ = heights_;
    out.holes_ = holes_;
    out.splat_ = splat_;
    return out;
}

HeightField HeightField::resampled(int resolution) const {
    if (!valid()) return {};
    // 同じ解像度ならそのまま写す（heightAt を通すと丸めで最後の桁が変わりうる）。
    if (resolution == resolution_) return *this;
    // 中心はコンストラクタに渡さず、元の min / baseY をそのまま写す
    // （中心から min を出し直すと float の丸めで 1 ulp ずれうる）。
    HeightField out(resolution, size_, 0.0f, baseY_, 0.0f);
    if (!out.valid()) return out;
    out.minX_ = minX_;
    out.minZ_ = minZ_;

    // 高さは新しい格子点のワールド座標で heightAt に聞く = 地表と同じ 2 三角形の
    // 補間なので、拡大しても見えている形が変わらない。heightAt は範囲外を端へ
    // 丸めるので、丸め誤差で最後の頂点が端をわずかに越えても外挿にならない。
    const double cell = double(size_) / double(resolution);
    const int side = resolution + 1;
    for (int j = 0; j < side; ++j) {
        const double z = double(minZ_) + double(j) * cell;
        for (int i = 0; i < side; ++i) {
            const double x = double(minX_) + double(i) * cell;
            out.heights_[std::size_t(j) * std::size_t(side) + std::size_t(i)] =
                float(heightAt(x, z) - double(baseY_));
        }
    }

    if (!holes_.empty()) {
        const double ratio = double(resolution_) / double(resolution);
        std::vector<uint8_t> mask(std::size_t(resolution) * std::size_t(resolution), 0);
        bool any = false;
        for (int j = 0; j < resolution; ++j) {
            int sj0 = 0, sj1 = 0;
            sourceCells(j, ratio, resolution_, sj0, sj1);
            for (int i = 0; i < resolution; ++i) {
                int si0 = 0, si1 = 0;
                sourceCells(i, ratio, resolution_, si0, si1);
                bool hole = false;
                for (int sj = sj0; sj <= sj1 && !hole; ++sj)
                    for (int si = si0; si <= si1 && !hole; ++si) hole = holeAt(si, sj);
                mask[std::size_t(j) * std::size_t(resolution) + std::size_t(i)] = hole ? 1 : 0;
                any = any || hole;
            }
        }
        if (any) out.holes_ = std::move(mask);  // 穴が残らなければ空のまま
    }

    // 重みは最近傍（重みを混ぜると合計 255 の量子化をやり直すことになるため、
    // テクセルをそのまま拾う）。塗っていなければ配列を持たないまま。
    if (!splat_.empty()) {
        const double ratio = double(resolution_) / double(resolution);
        out.allocateSplat();
        for (int j = 0; j < resolution; ++j) {
            const int sj = nearestCell(j, ratio, resolution_);
            for (int i = 0; i < resolution; ++i) {
                const int si = nearestCell(i, ratio, resolution_);
                splatAt(si, sj, &out.splat_[out.splatOffset(i, j)]);
            }
        }
    }
    return out;
}

GridRect HeightField::chunksTouched(const GridRect& cells, int quadsPerChunk) const {
    const GridRect c = cells.clamped(resolution_);
    if (!valid() || quadsPerChunk < 1 || c.empty()) return {};
    const int q = quadsPerChunk;
    // 書き換えうるサンプルは [i0, i1]、法線が変わる頂点はその ±1 = [i0 − 1, i1 + 1]。
    // チャンク n は頂点 [n·q, n·q + q] を持つので、頂点 v を持つチャンクの最小は
    // ceil(v / q) − 1 = (v − 1) / q（v ≥ 1。v ≤ 0 は 0）、最大は v / q。
    auto lowChunk = [q](int v) { return v <= 0 ? 0 : (v - 1) / q; };
    const GridRect v = c.expanded(1);  // 頂点の範囲（両端を含む）
    const GridRect r{lowChunk(v.i0), lowChunk(v.j0), v.i1 / q + 1, v.j1 / q + 1};
    return r.clamped(chunksPerSide(q));
}

}  // namespace terrain
}  // namespace wizengine
