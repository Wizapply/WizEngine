#pragma once

#include <cstdint>
#include <vector>

#include "ui/UiSdfParams.h"

// 字の SDF（輪郭までの距離の画像）の作り方。値の決まり（定数）は UiSdfParams.h。
// Chrono / Filament / stb を知らない純粋な層（tests/ui で検査する）。
namespace wizengine {
namespace ui {

// 輪郭までの距離（基準の画素、外が正）→ SDF の値（UiSdfParams.h の決まりの唯一の式）。
// 0..255 に収めてから四捨五入する（距離が極端でも範囲外にならない）。
std::uint8_t sdfValue(float distPx);

// 白黒の塗り（覆い率 0..255、width × height、1 行 width バイト）から SDF を作る。
// 塗りは基準の画素 1 つを scale × scale 画素で塗ったもの（width と height は scale の
// 倍数で、周りににじみ幅 × scale の余白を含める）。戻り値は (width / scale) ×
// (height / scale) の値（UiSdfParams.h の決まり）。覆い率 128 以上を「字の内側」とし
// （この決まりはここだけ）、内外の印からの正確なユークリッド距離（距離変換）で輪郭
// までの距離を出す。内側が 1 画素も無ければ空を返す（絵の無い字）。幅・高さが
// scale の倍数でない・scale が 1 未満でも空を返す（呼び手の誤り）。
// 細かい塗りで覆い率が 128 に届かない極細の線や点は消える（覆い率で輪郭の位置を
// 補正しない）: 4 倍で塗るので、消えるのは基準の 1/8 画素より細い線だけで、表示の
// 最小の 14 px でもそれは 0.05 画素未満 = 見えない太さのため。
std::vector<std::uint8_t> sdfFromCoverage(const std::uint8_t* coverage, int width, int height,
                                          int scale);

}  // namespace ui
}  // namespace wizengine
