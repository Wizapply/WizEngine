#include "ui/UiSdf.h"

#include <algorithm>
#include <cmath>

namespace wizengine {
namespace ui {
namespace {

// 「印が無い」の距離（2 乗）。足し引きで溢れない大きさ（画素の 2 乗よりずっと大きい）。
constexpr float kInf = 1.0e20f;

// 1 次元の距離変換（下側の放物線の包み。f は 0 = 印、kInf = 印でない）。
// d[q] = min_p ((q - p)^2 + f[p])。v / z は作業領域（n と n + 1）。
void distance1d(const float* f, int n, float* d, int* v, float* z) {
    auto cross = [f](int q, int p) {
        return ((f[q] + float(q) * float(q)) - (f[p] + float(p) * float(p))) /
               (2.0f * float(q) - 2.0f * float(p));
    };
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        float s = cross(q, v[k]);
        while (s <= z[k]) {  // z[0] = -kInf なので k は 0 より下がらない
            --k;
            s = cross(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < float(q)) ++k;
        const float dq = float(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

// 距離変換の作業領域（RENDER スレッドが字ごとに呼ぶので、確保し直さず使い回す）。
struct Scratch {
    std::vector<float> f, d, z;
    std::vector<int> v;
    std::vector<char> inside, outside;
    std::vector<float> toInside, toOutside;
};
Scratch& scratch() {
    thread_local Scratch s;
    return s;
}

// 2 次元の距離変換（各画素から、印のある画素の中心までの距離の 2 乗）を grid へ。
void distance2d(const std::vector<char>& mark, int w, int h, std::vector<float>& grid,
                Scratch& work) {
    grid.resize(std::size_t(w) * std::size_t(h));
    for (std::size_t i = 0; i < grid.size(); ++i) grid[i] = mark[i] ? 0.0f : kInf;
    const std::size_t n = static_cast<std::size_t>(std::max(w, h));
    std::vector<float>& f = work.f;
    std::vector<float>& d = work.d;
    std::vector<float>& z = work.z;
    std::vector<int>& v = work.v;
    f.resize(n);
    d.resize(n);
    z.resize(n + 1);
    v.resize(n);
    for (int x = 0; x < w; ++x) {  // 列
        for (int y = 0; y < h; ++y) f[std::size_t(y)] = grid[std::size_t(y) * w + x];
        distance1d(f.data(), h, d.data(), v.data(), z.data());
        for (int y = 0; y < h; ++y) grid[std::size_t(y) * w + x] = d[std::size_t(y)];
    }
    for (int y = 0; y < h; ++y) {  // 行
        float* row = grid.data() + std::size_t(y) * w;
        std::copy(row, row + w, f.begin());
        distance1d(f.data(), w, d.data(), v.data(), z.data());
        std::copy(d.begin(), d.begin() + w, row);
    }
}

}  // namespace

std::uint8_t sdfValue(float distPx) {
    const float value = std::clamp(float(kSdfOnEdge) - distPx * kSdfDistScale, 0.0f, 255.0f);
    return static_cast<std::uint8_t>(std::lround(value));  // NaN は来ない（距離は有限か 1e10 程度）
}

std::vector<std::uint8_t> sdfFromCoverage(const std::uint8_t* coverage, int width, int height,
                                          int scale) {
    const bool shapeOk = scale >= 1 && width > 0 && height > 0 && width % scale == 0 &&
                         height % scale == 0;
    if (!shapeOk || !coverage) return {};
    const int ow = width / scale;
    const int oh = height / scale;
    const std::size_t count = std::size_t(width) * std::size_t(height);
    Scratch& work = scratch();
    std::vector<char>& inside = work.inside;
    std::vector<char>& outside = work.outside;
    inside.resize(count);
    outside.resize(count);
    bool anyInside = false;
    for (std::size_t i = 0; i < count; ++i) {
        inside[i] = coverage[i] >= 128 ? 1 : 0;
        outside[i] = inside[i] ? 0 : 1;
        anyInside = anyInside || inside[i];
    }
    if (!anyInside) return {};  // 内側が 1 画素も無い = 絵の無い字
    std::vector<std::uint8_t> out(std::size_t(ow) * std::size_t(oh), 0);
    // 外の画素は内側の印まで、内側の画素は外の印までの距離。輪郭は隣り合う 2 つの
    // 画素の中心の間なので、半画素を引く（細かい画素で測る = 誤差は 1/(2 scale) 以下）。
    std::vector<float>& toInside = work.toInside;
    std::vector<float>& toOutside = work.toOutside;
    distance2d(inside, width, height, toInside, work);
    distance2d(outside, width, height, toOutside, work);
    auto signedAt = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        const std::size_t i = std::size_t(y) * std::size_t(width) + std::size_t(x);
        // 正 = 外。距離は細かい画素の単位。
        return inside[i] ? -(std::sqrt(toOutside[i]) - 0.5f) : (std::sqrt(toInside[i]) - 0.5f);
    };
    // 出力の画素の中心（細かい画素の座標）での値をバイリニアで引く。中心の
    // 小数部（重み）はどの画素でも同じ（scale / 2 − 0.5）なので先に求める。
    const float centre = float(scale) * 0.5f - 0.5f;
    const int offset = int(std::floor(centre));
    const float tx = centre - float(offset), ty = tx;
    for (int oy = 0; oy < oh; ++oy) {
        for (int ox = 0; ox < ow; ++ox) {
            const int x0 = ox * scale + offset, y0 = oy * scale + offset;
            const float top = signedAt(x0, y0) * (1.0f - tx) + signedAt(x0 + 1, y0) * tx;
            const float bottom = signedAt(x0, y0 + 1) * (1.0f - tx) + signedAt(x0 + 1, y0 + 1) * tx;
            const float dist = (top * (1.0f - ty) + bottom * ty) / float(scale);  // 基準の画素
            out[std::size_t(oy) * std::size_t(ow) + std::size_t(ox)] = sdfValue(dist);
        }
    }
    return out;
}

}  // namespace ui
}  // namespace wizengine
