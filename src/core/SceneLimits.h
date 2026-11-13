#pragma once

// シーンの値の上限（シーン文書・UI の文書・Scene が同じ値で止める唯一の定義）。
namespace wizengine {

// カメラのスロット数の上限（exe 引数 --max-cameras の上限。Scene がスロット数を
// ここで止め、UI の Canvas の「表示するカメラ」もこの番号の範囲で持つ）。
inline constexpr int kMaxCameraSlots = 16;

// 置き場所の位置の上限（各軸 ±この値 m。地面・テレイン・world の UI の板）。
inline constexpr double kPlacementMaxMeters = 10000.0;

}  // namespace wizengine
