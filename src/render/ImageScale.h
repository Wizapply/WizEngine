#pragma once

// RGBA8 の画素の大きさを変える純粋な計算（Filament を知らない）: ミップの段数・
// 1 段ぶんのボックスフィルタ・任意の大きさへの伸縮と、UI の画像の乗算済みアルファ
// （sRGB ⇄ リニアの表・乗算・リニアで平均する縮小）。テクスチャの読込
// （RendererTextures.cpp）と環境マップ（EnvironmentLoader.cpp）が使い、
// tests/terrain が単体で検査する。RGBA8 のボックスフィルタの並べ方はここ 1 つだけ
// （HDR の縮小は ImageLoader.cpp の別物）。

#include <algorithm>
#include <array>
#include <cmath>
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

// src（w × h の RGBA8）を、halveX / halveY の軸だけ半分にした縮小を dst へ書く
// （2×2 / 2×1 / 1×2 をまとめる。辺が 1 の軸・halve が false の軸はそのまま、奇数の
// 辺は最後の列 / 行を捨てる）。縮めた大きさを nw / nh に返す。まとめ方は average
// （average(const uint8_t* const* px, int n, uint8_t* out) = n 個の画素 → 1 画素）。
template <class Average>
inline void halveRGBAWith(const uint8_t* src, int w, int h, bool halveX, bool halveY,
                          std::vector<uint8_t>& dst, int& nw, int& nh, Average average) {
    const bool hx = halveX && w > 1;
    const bool hy = halveY && h > 1;
    nw = hx ? w / 2 : w;
    nh = hy ? h / 2 : h;
    dst.resize(std::size_t(nw) * std::size_t(nh) * 4);
    for (int y = 0; y < nh; ++y) {
        const int y0 = hy ? 2 * y : y;
        for (int x = 0; x < nw; ++x) {
            const int x0 = hx ? 2 * x : x;
            const uint8_t* px[4];
            int n = 0;
            for (int dy = 0; dy < (hy ? 2 : 1); ++dy) {
                for (int dx = 0; dx < (hx ? 2 : 1); ++dx) {
                    px[n++] = src + (std::size_t(y0 + dy) * w + std::size_t(x0 + dx)) * 4;
                }
            }
            average(px, n, dst.data() + (std::size_t(y) * nw + std::size_t(x)) * 4);
        }
    }
}

// バイトのまま平均するボックスフィルタ。ミップ列（uploadMipChain）と大きな縮小の
// 前段（resizeRGBA）が同じものを使う。
inline void halveRGBA(const uint8_t* src, int w, int h, bool halveX, bool halveY,
                      std::vector<uint8_t>& dst, int& nw, int& nh) {
    halveRGBAWith(src, w, h, halveX, halveY, dst, nw, nh,
                  [](const uint8_t* const* px, int n, uint8_t* out) {
                      for (int c = 0; c < 4; ++c) {
                          int sum = 0;
                          for (int k = 0; k < n; ++k) sum += px[k][c];
                          out[c] = uint8_t(sum / n);
                      }
                  });
}

// ---- sRGB の乗算済みアルファ（UI の画像）------------------------------------------
// sRGB と リニアの変換の式はこの 2 つの表だけ（GPU 側は SRGB8_A8 のテクスチャが
// 戻し、ui.mat の linearToSrgb が書き出しで符号化する）。
// sRGB の 8 ビット → リニア（0..1）の 256 段の表。
inline const std::array<float, 256>& srgbToLinearTable() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = float(i) / 255.0f;
            t[std::size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

// リニア（0..1）→ sRGB の 8 ビットの 4096 段の表（段の幅 1/4095。最も暗い所でも
// 1 段 ≒ sRGB の 1 未満）。
inline constexpr int kLinearToSrgbSteps = 4096;
using LinearToSrgbTable = std::array<uint8_t, kLinearToSrgbSteps>;
inline const LinearToSrgbTable& linearToSrgbTable() {
    static const LinearToSrgbTable table = [] {
        LinearToSrgbTable t{};
        for (int i = 0; i < kLinearToSrgbSteps; ++i) {
            const float l = float(i) / float(kLinearToSrgbSteps - 1);
            const float c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            t[std::size_t(i)] = uint8_t(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
        }
        return t;
    }();
    return table;
}

// 表を引く（ループの中では表の参照を外で 1 回取って渡す）。リニアは 0..1 の外を端へ、
// NaN は 0 段へ。
inline float srgbByteToLinear(uint8_t v) { return srgbToLinearTable()[v]; }
inline uint8_t linearToSrgbByte(const LinearToSrgbTable& table, float v) {
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    const int index = clamped > 0.0f ? int(clamped * float(kLinearToSrgbSteps - 1) + 0.5f) : 0;
    return table[std::size_t(index)];
}
inline uint8_t linearToSrgbByte(float v) { return linearToSrgbByte(linearToSrgbTable(), v); }

// 乗算済みの 1 成分（リニア）を sRGB の 8 ビットへ。丸めで α を超えないよう、戻した
// 値が alpha（0..1）を超える間は 1 段下げる（乗算済みの約束 rgb ≦ α を保つ）。
inline uint8_t premultipliedSrgbByte(const std::array<float, 256>& toLinear,
                                     const LinearToSrgbTable& toSrgb, float linear, float alpha) {
    uint8_t b = linearToSrgbByte(toSrgb, linear);
    while (b > 0 && toLinear[b] > alpha) --b;
    return b;
}

// sRGB の RGBA8（ストレート）を、リニアで rgb に α を掛けて sRGB へ戻した乗算済みに
// する（SRGB8_A8 で引くとリニアの乗算済みになる）。
inline void premultiplySrgbRGBA(std::vector<uint8_t>& rgba) {
    const auto& toLinear = srgbToLinearTable();
    const auto& toSrgb = linearToSrgbTable();
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const uint8_t a = rgba[i + 3];
        if (a == 255) continue;
        const float alpha = float(a) / 255.0f;
        for (std::size_t c = 0; c < 3; ++c) {
            rgba[i + c] = premultipliedSrgbByte(toLinear, toSrgb, toLinear[rgba[i + c]] * alpha, alpha);
        }
    }
}

// 乗算済みの sRGB の RGBA8 を半分にする（rgb はリニアに戻して平均してから sRGB へ。
// α は平均を四捨五入 = rgb と同じ丸め。切り捨てると段ごとに α だけが下がり、
// rgb が α を超える。rgb も丸めで α を超えないよう premultipliedSrgbByte で抑える）。
// バイトのまま平均すると、不透明な画素と透明な画素の間が暗く沈む。
inline void halvePremultipliedSrgbRGBA(const uint8_t* src, int w, int h, bool halveX,
                                       bool halveY, std::vector<uint8_t>& dst, int& nw,
                                       int& nh) {
    const auto& toLinear = srgbToLinearTable();
    const auto& toSrgb = linearToSrgbTable();
    halveRGBAWith(src, w, h, halveX, halveY, dst, nw, nh,
                  [&toLinear, &toSrgb](const uint8_t* const* px, int n, uint8_t* out) {
                      int alpha = 0;
                      for (int k = 0; k < n; ++k) alpha += px[k][3];
                      out[3] = uint8_t((alpha + n / 2) / n);
                      const float limit = float(out[3]) / 255.0f;
                      for (int c = 0; c < 3; ++c) {
                          float sum = 0.0f;
                          for (int k = 0; k < n; ++k) sum += toLinear[px[k][c]];
                          out[c] = premultipliedSrgbByte(toLinear, toSrgb, sum / float(n), limit);
                      }
                  });
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
