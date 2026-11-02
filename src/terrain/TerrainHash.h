#pragma once

#include <cmath>
#include <cstdint>

// テレインの計算で使う 32 ビットの整数ハッシュと、それを使うくじ（ノイズの種の
// 散らし・筆の塗りのくじ引き・散布の筆の位置 / 向き / 倍率 / 個数のくじ引き）。
// 乱数の状態を持たず、同じ入力から同じ値が出る（テストで再現できる）。
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

// ハッシュ値 → くじの [0, 1)（上位 24 ビット / 2^24。float に入れても 1 に丸まらない
// 刻み）。筆の塗りの端数・散布の位置 / 向き / 倍率 / 個数のくじが使う。
inline double hashUnit(uint32_t h) { return double(h >> 8) / 16777216.0; }

// x の端数をくじで丸める: floor(x) に、くじ unit（[0, 1)）が端数より小さければ 1 を
// 足す（1 回ぶんの平均が x = 端数を捨てない・切り上げない）。筆の塗りの増分と
// 散布の円ごとの目標の個数が使う。
inline double roundByLottery(double x, double unit) {
    const double whole = std::floor(x);
    return unit < x - whole ? whole + 1.0 : whole;
}

}  // namespace terrain
}  // namespace wizengine
