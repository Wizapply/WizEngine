#pragma once

#include <cstdint>
#include <vector>

// 字の SDF（輪郭までの距離の画像）の決まりと作り方（唯一の定義）。
//
// 字の置き場（FontAtlas）が作り、描画側の材質（ui.mat）が読む。値は「輪郭で
// kSdfOnEdge、外へ 1 画素（基準の字の大きさの画素）ごとに kSdfDistScale ずつ
// 下がり、にじみ幅の外で 0。内側は同じ割合で上がり 255 で頭打ち」（0..255）。
// 頂点の縁取りの幅（UiMeshFormat.h の UiVertex::outline）もこの決まりで読む。
// Chrono / Filament / stb を知らない純粋な層（tests/ui で検査する）。
namespace wizengine {
namespace ui {

//   基準の字の大きさ 40 px: 画数の多い漢字の細部を残すための見当（実機で
//     調整する前提）。にじみ幅込みで 1 字およそ 56 px 角 = 2048 の置き場に
//     約 1300 字（UI で使う字の種類には十分）。
//   にじみ幅 8 px（= 0.2 em）: 縁取りに使える距離の上限。
inline constexpr float kSdfBasePx = 40.0f;
inline constexpr int kSdfSpreadPx = 8;
inline constexpr int kSdfOnEdge = 128;  // 輪郭の値（0..255）
// 輪郭から 1 画素離れるごとの値の変化（にじみ幅の外で 0 になる）。
inline constexpr float kSdfDistScale = float(kSdfOnEdge) / float(kSdfSpreadPx);
// 縁取りの幅の上限（em に対する割合 = にじみ幅 / 基準の字の大きさ）。
inline constexpr double kSdfOutlineMaxEm = double(kSdfSpreadPx) / double(kSdfBasePx);
// 距離を求めるために字を塗る細かさ（基準の画素 1 つを何画素で塗るか）。4 で
// 輪郭の位置の誤差は 1/8 画素ほど（値の 2 段ぶん）。
inline constexpr int kSdfOversample = 4;

// 輪郭までの距離（基準の画素、外が正）→ SDF の値（上の決まりの唯一の式）。
// 0..255 に収めてから四捨五入する（距離が極端でも範囲外にならない）。
std::uint8_t sdfValue(float distPx);

// 白黒の塗り（覆い率 0..255、width × height、1 行 width バイト）から SDF を作る。
// 塗りは基準の画素 1 つを scale × scale 画素で塗ったもの（width と height は scale の
// 倍数で、周りににじみ幅 × scale の余白を含める）。戻り値は (width / scale) ×
// (height / scale) の値（上の決まり）。覆い率 128 以上を「字の内側」とし、内外の
// 印からの正確なユークリッド距離（距離変換）で輪郭までの距離を出す。
std::vector<std::uint8_t> sdfFromCoverage(const std::uint8_t* coverage, int width, int height,
                                          int scale);

}  // namespace ui
}  // namespace wizengine
