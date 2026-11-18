#pragma once

// 字の SDF（輪郭までの距離の画像）の決まり（定数だけ。唯一の定義）。
//
// 字の置き場（FontAtlas）が作り、描画側の材質（ui.mat）が読む。値は「輪郭で
// kSdfOnEdge、外へ 1 画素（基準の字の大きさの画素）ごとに kSdfDistScale ずつ
// 下がり、にじみ幅の外で 0。内側は同じ割合で上がり 255 で頭打ち」（0..255）。
// 頂点の縁取りの幅（UiMeshFormat.h の UiVertex::outline）もこの決まりで読む。
// 作り方（距離の計算）は UiSdf.h。描画側が読むのはこちらだけ（UiMeshFormat.h 経由）。
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

}  // namespace ui
}  // namespace wizengine
