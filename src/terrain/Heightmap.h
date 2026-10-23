#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "terrain/HeightField.h"

// ハイトマップ（16 ビットの高さ画像）のバイト列と 16 ビット標本の往復、
// そして標本と格子（HeightField）の往復。ファイルの読み書きはしない（バイト列
// だけ。ファイルは TerrainFile の readFileBytes / writeFileBytes）。
//
// 標本の約束（.terrain の HGHT 節と同じ）:
//   * 1 辺 side 個（= 格子の resolution + 1）の正方形。行優先で、行 = +Z、
//     行内 = +X（HeightField::samples() と同じ並び）。
//   * 値 = quantizeHeight(h, height) = round(clamp(h / height, 0, 1) × 65535)。
//     0 が最低点（格子値 0）、65535 が高さの上限 height。
//   * 画像として見ると、1 行目（-Z の端）が画像の上、+X が右 = 上（+Y）から
//     見下ろした地図の向き。逆の向きで作られた画像は flipRows で読む。
//   * **穴は持たない**。書き出しは穴のセルの角の高さもそのまま書き（穴は
//     失われる）、取り込みは穴の無い格子を返す（今の格子の穴を保つかは
//     呼び手が決める）。
//
// 失敗の理由（reason）はすべて英語（コンソールのログへ流れるため）。
namespace wizengine {
namespace terrain {

// ハイトマップの 1 辺の標本数の上限（= 格子の解像度の上限 + 1）と、RAW16 の
// 最大バイト数（2·n²、n = kMaxHeightmapSide）。下の関数の範囲の検査と、読む前に
// 断る呼び手（ファイルの大きさ・画像の見出し）が同じ値を使う。
constexpr int kMaxHeightmapSide = kMaxResolution + 1;
constexpr uintmax_t kMaxRaw16Bytes =
    2ull * uintmax_t(kMaxHeightmapSide) * uintmax_t(kMaxHeightmapSide);

// 高さ (m、格子値) ⇄ 16 ビット標本。height は高さの上限 (m) で、**正の有限値で
// あること**（呼び手が保証する。検査はしない）。[0, height] の外は端に丸める
// （NaN は 0）。
uint16_t quantizeHeight(float h, float height);
float dequantizeHeight(uint16_t q, float height);

// RAW16（見出しの無い u16 リトルエンディアンの正方形）→ 標本。大きさが
// 2·n² バイト（n = 2〜kMaxHeightmapSide）でなければ理由付きで false
// （8 ビットや正方形でない画像はここで断る）。成功すれば samples と side を書く。
bool decodeRaw16(const uint8_t* data, std::size_t size, std::vector<uint16_t>& samples,
                 int& side, std::string& reason);
// 標本 → RAW16（u16 リトルエンディアン、標本の並びのまま）。
std::vector<uint8_t> encodeRaw16(const std::vector<uint16_t>& samples);

// 標本 → PNG（16 ビットのグレースケール、1 辺 side の正方形、無圧縮）。
// side が 2〜kMaxHeightmapSide の外・samples.size() が side² でないなら空を返す
// （decodeRaw16 / heightFieldFromSamples と同じ範囲）。
std::vector<uint8_t> encodePng16Gray(const std::vector<uint16_t>& samples, int side);

// 格子 → 標本（並びは samples() のまま、height で量子化）。height は正の有限値で
// あること（quantizeHeight と同じ約束）。空の格子は空。
std::vector<uint16_t> heightmapSamples(const HeightField& field, float height);
// 標本 → 原点中心・baseY 0 の格子（1 辺 resolution セル）。標本の 1 辺 side を
// そのまま格子にしてから、resolution と違えば resampled で合わせる。flipRows は
// 行（Z）の向きを逆にして読む。side が 2〜kMaxHeightmapSide の外・
// samples.size() が side² でない・size / height が正の有限値でない・
// resolution が 1〜kMaxResolution の外なら、理由付きで空の格子を返す。
HeightField heightFieldFromSamples(const std::vector<uint16_t>& samples, int side,
                                   float size, float height, int resolution,
                                   bool flipRows, std::string& reason);

}  // namespace terrain
}  // namespace wizengine
