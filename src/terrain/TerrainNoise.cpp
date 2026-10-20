// テレインの高さをノイズ（stb_perlin の fbm）で作る。
//
// stb_perlin の実装はこの翻訳単位にだけ置く（STB_PERLIN_IMPLEMENTATION の
// 定義はここ 1 か所。他の場所で定義するとリンクで重複する）。
#include "terrain/HeightField.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#define STB_PERLIN_IMPLEMENTATION
#include "stb_perlin.h"

namespace wizengine {
namespace terrain {
namespace {

// 32 ビットの整数ハッシュ（lowbias32）。種を散らすためだけに使う。
uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// ハッシュから [0, 256) の実数。stb_perlin のノイズは 256 で一周するので、
// それより大きなずらしは意味が無い。
float offsetFrom(uint32_t h) {
    return float(h & 0xffffffu) / 16777216.0f * 256.0f;
}

}  // namespace

// 種の反映: stb_perlin_noise3_seed は種の下位 8 ビットしか使わない（256 通り）
// ので、それだけだと違う種が同じ地形になる。そこで種をハッシュして
//   * オクターブごとの順列の選択（下位 8 ビット、オクターブごとに別）
//   * 平面内のずらし (ox, oz) と、3 次元ノイズを切る高さ（slice）
// の両方に使う。ずらしと slice は実数なので、32 ビットの種がほぼ全部
// 別の地形になる。fbm の式（周波数 × lacunarity、振幅 × gain）は
// stb_perlin_fbm_noise3 と同じで、違いはこの種の入れ方だけ。
HeightField generateHeightField(const TerrainParams& params) {
    // 安全弁（本番の丸めは文書側の clampTerrain）。
    const int res = std::clamp(params.resolution, 1, kMaxResolution);
    const float size =
        (std::isfinite(params.size) && params.size > 0.0f) ? params.size : 1.0f;
    const float height =
        (std::isfinite(params.height) && params.height > 0.0f) ? params.height : 0.0f;
    const int octaves = std::clamp(params.octaves, 0, 16);
    const float scale =
        (std::isfinite(params.scale) && params.scale > 1e-3f) ? params.scale : 1e-3f;
    const float gain = std::isfinite(params.gain) ? params.gain : 0.5f;
    const float lacunarity = std::isfinite(params.lacunarity) ? params.lacunarity : 2.0f;
    const float px = std::isfinite(params.posX) ? params.posX : 0.0f;
    const float py = std::isfinite(params.posY) ? params.posY : 0.0f;
    const float pz = std::isfinite(params.posZ) ? params.posZ : 0.0f;

    HeightField field(res, size, px, py, pz);
    if (!field.valid() || octaves == 0 || height <= 0.0f) return field;  // 平ら

    const uint32_t h0 = mix32(params.seed ^ 0x9e3779b9u);
    const float ox = offsetFrom(mix32(h0 + 1u));
    const float oz = offsetFrom(mix32(h0 + 2u));
    const float slice = offsetFrom(mix32(h0 + 3u));
    std::vector<int> octaveSeed(static_cast<std::size_t>(octaves), 0);
    for (int o = 0; o < octaves; ++o) {
        octaveSeed[std::size_t(o)] = int(mix32(h0 + 101u + uint32_t(o)) & 0xffu);
    }

    // まず fbm の生の値を格子に入れ、あとで最小〜最大を 0〜height に伸ばす。
    const int side = res + 1;
    const double cell = double(size) / double(res);
    std::vector<float>& hs = field.samples();
    float lo = 1e30f, hi = -1e30f;
    for (int j = 0; j < side; ++j) {
        // 中心からの相対座標（pos を動かしても形が変わらない）。
        const float lz = float((j * cell - size * 0.5) / scale);
        for (int i = 0; i < side; ++i) {
            const float lx = float((i * cell - size * 0.5) / scale);
            float sum = 0.0f, freq = 1.0f, amp = 1.0f;
            for (int o = 0; o < octaves; ++o) {
                sum += amp * stb_perlin_noise3_seed(lx * freq + ox, slice,
                                                    lz * freq + oz, 0, 0, 0,
                                                    octaveSeed[std::size_t(o)]);
                freq *= lacunarity;
                amp *= gain;
            }
            if (!std::isfinite(sum)) sum = 0.0f;
            hs[std::size_t(j) * std::size_t(side) + std::size_t(i)] = sum;
            lo = std::min(lo, sum);
            hi = std::max(hi, sum);
        }
    }

    const float range = hi - lo;
    if (!(range > 1e-9f)) {
        std::fill(hs.begin(), hs.end(), 0.0f);  // 起伏が無い = 平ら
        return field;
    }
    const float k = height / range;
    for (float& v : hs) v = (v - lo) * k;
    return field;
}

}  // namespace terrain
}  // namespace wizengine
