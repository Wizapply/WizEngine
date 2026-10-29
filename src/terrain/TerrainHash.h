#pragma once

#include <cstdint>

// テレインの計算で使う 32 ビットの整数ハッシュ（ノイズの種の散らし・筆の塗りの
// くじ引き）。乱数の状態を持たず、同じ入力から同じ値が出る（テストで再現できる）。
namespace wizengine {
namespace terrain {

// 32 ビットの整数ハッシュ（lowbias32）。
inline uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

}  // namespace terrain
}  // namespace wizengine
