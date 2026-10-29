#pragma once

// RGBA8 の画素の大きさを変える純粋な計算（Filament を知らない）: ミップの段数・
// 1 段ぶんのボックスフィルタ・任意の大きさへの伸縮。テクスチャの読込
// （RendererTextures.cpp）と環境マップ（EnvironmentLoader.cpp）が使い、
// tests/terrain が単体で検査する。RGBA8 のボックスフィルタはここ 1 つだけ
// （HDR の縮小は ImageLoader.cpp の別物）。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace render_detail {

// width × height のテクスチャのミップの段数（大きい方の辺から 1×1 まで）。
inline uint8_t mipLevelCount(int width, int height) {
    uint8_t levels = 1;
    for (int m = (width > height ? width : height); m > 1; m >>= 1) ++levels;
    return levels;
}

// src（w × h の RGBA8）を、halveX / halveY の軸だけ半分にしたボックスフィルタの
// 縮小を dst へ書く（2×2 / 2×1 / 1×2 の平均。辺が 1 の軸・halve が false の軸は
// そのまま、奇数の辺は最後の列 / 行を捨てる）。縮めた大きさを nw / nh に返す。
// ミップ列（uploadMipChain）と大きな縮小の前段（resizeRGBA）が同じものを使う。
inline void halveRGBA(const uint8_t* src, int w, int h, bool halveX, bool halveY,
                      std::vector<uint8_t>& dst, int& nw, int& nh) {
    const bool hx = halveX && w > 1;
    const bool hy = halveY && h > 1;
    nw = hx ? w / 2 : w;
    nh = hy ? h / 2 : h;
    dst.resize(std::size_t(nw) * std::size_t(nh) * 4);
    const int n = (hx ? 2 : 1) * (hy ? 2 : 1);
    for (int y = 0; y < nh; ++y) {
        const int y0 = hy ? 2 * y : y;
        const int y1 = hy ? y0 + 1 : y0;
        for (int x = 0; x < nw; ++x) {
            const int x0 = hx ? 2 * x : x;
            const int x1 = hx ? x0 + 1 : x0;
            for (int c = 0; c < 4; ++c) {
                int sum = src[(std::size_t(y0) * w + x0) * 4 + c];
                if (hx) sum += src[(std::size_t(y0) * w + x1) * 4 + c];
                if (hy) sum += src[(std::size_t(y1) * w + x0) * 4 + c];
                if (hx && hy) sum += src[(std::size_t(y1) * w + x1) * 4 + c];
                dst[(std::size_t(y) * nw + std::size_t(x)) * 4 + c] = uint8_t(sum / n);
            }
        }
    }
}

// 辺 src を dst へ縮めるとき、双一次の前に半分にする回数（src が dst の 2 倍を
// 超える間。双一次は 2 倍までなら近い 2×2 を混ぜるので取りこぼさないが、それを
// 超えると間の画素を飛ばしてちらつく）。縮小でなければ 0。
inline int halvingsBeforeResize(int src, int dst) {
    int steps = 0;
    for (int s = src; dst > 0 && s > 2 * dst; s /= 2) ++steps;
    return steps;
}

// RGBA8 の画素 src（srcW × srcH）を dstW × dstH へ伸縮して dst へ書く（縦横比が
// 違えば引き伸ばす）。2 倍を超える縮小は軸ごとに halveRGBA で半分ずつ縮めて
// から、最後に双一次補間（出力の画素の中心を入力の座標へ写し、近い 4 画素を
// 混ぜる。端は端の画素を伸ばす）。大きさが同じなら写すだけ。
inline void resizeRGBA(const uint8_t* src, int srcW, int srcH, int dstW, int dstH,
                       std::vector<uint8_t>& dst) {
    dst.resize(std::size_t(dstW) * std::size_t(dstH) * 4);
    if (srcW == dstW && srcH == dstH) {
        std::memcpy(dst.data(), src, dst.size());
        return;
    }
    std::vector<uint8_t> work;
    std::vector<uint8_t> next;
    int stepsX = halvingsBeforeResize(srcW, dstW);
    int stepsY = halvingsBeforeResize(srcH, dstH);
    while (stepsX > 0 || stepsY > 0) {
        const uint8_t* from = work.empty() ? src : work.data();
        int nw = 0;
        int nh = 0;
        halveRGBA(from, srcW, srcH, stepsX > 0, stepsY > 0, next, nw, nh);
        work.swap(next);
        srcW = nw;
        srcH = nh;
        if (stepsX > 0) --stepsX;
        if (stepsY > 0) --stepsY;
    }
    const uint8_t* in = work.empty() ? src : work.data();
    if (srcW == dstW && srcH == dstH) {
        std::memcpy(dst.data(), in, dst.size());
        return;
    }
    const float sx = float(srcW) / float(dstW);
    const float sy = float(srcH) / float(dstH);
    for (int y = 0; y < dstH; ++y) {
        const float fy = std::clamp((float(y) + 0.5f) * sy - 0.5f, 0.0f, float(srcH - 1));
        const int y0 = int(fy);
        const int y1 = std::min(y0 + 1, srcH - 1);
        const float ty = fy - float(y0);
        for (int x = 0; x < dstW; ++x) {
            const float fx =
                std::clamp((float(x) + 0.5f) * sx - 0.5f, 0.0f, float(srcW - 1));
            const int x0 = int(fx);
            const int x1 = std::min(x0 + 1, srcW - 1);
            const float tx = fx - float(x0);
            const uint8_t* p00 = in + (std::size_t(y0) * srcW + std::size_t(x0)) * 4;
            const uint8_t* p10 = in + (std::size_t(y0) * srcW + std::size_t(x1)) * 4;
            const uint8_t* p01 = in + (std::size_t(y1) * srcW + std::size_t(x0)) * 4;
            const uint8_t* p11 = in + (std::size_t(y1) * srcW + std::size_t(x1)) * 4;
            uint8_t* out = dst.data() + (std::size_t(y) * dstW + std::size_t(x)) * 4;
            for (int c = 0; c < 4; ++c) {
                const float top = float(p00[c]) + (float(p10[c]) - float(p00[c])) * tx;
                const float bottom = float(p01[c]) + (float(p11[c]) - float(p01[c])) * tx;
                out[c] = uint8_t(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
}

}  // namespace render_detail
