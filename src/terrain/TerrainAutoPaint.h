#pragma once

#include <array>

#include "terrain/HeightField.h"

// テレインの自動塗り: 層ごとの「傾きと高さの帯」から全セルの塗り分けの重みを
// 決める純粋な計算（HeightField しか知らない。tests/terrain で単体で回す）。
//
// セルごとに、中心の傾き（度。0 = 水平、90 = 垂直）と高さ（格子の基準 baseY =
// テレインの置き場所の高さからの m。最低点ではない = 筆で全体を持ち上げても
// 基準は動かない）を見て、有効な層ごとに「傾きの帯に入っている度合い × 高さの帯に入っている度合い」
// を重みにする。帯の縁は softness でなめらかにつなぐ（smoothstep）。重みの
// 正規化と量子化は HeightField::setSplatWeights に任せる（どの層の帯にも
// 入らないセルは層 0 になる）。
namespace wizengine {
namespace terrain {

// 高さの帯の範囲の安全弁（m、基準 baseY からの ±）。帯の既定の上限もこれ
// （= 高さでは絞らない）。
constexpr double kAutoPaintMaxHeight = 10000.0;

// 1 つの層の帯。範囲は両端を含む。値の既定（層ごとの帯）は
// defaultAutoPaintRules が持ち、ここは「どの層にも当てない」中立の値。
struct AutoPaintBand {
    bool enabled = false;
    double slopeMinDeg = 0.0;   // 傾きの下限（度）
    double slopeMaxDeg = 90.0;  // 傾きの上限（度）
    double heightMin = 0.0;     // 高さの下限（基準 baseY からの m）
    double heightMax = kAutoPaintMaxHeight;  // 高さの上限（同）
};

// 自動塗りの規則（1 回の操作の指示。テレインには覚えない）。
struct AutoPaintRules {
    std::array<AutoPaintBand, kSplatChannels> layer;
    // 帯の縁のぼかし（0 = くっきり、1 = 帯の幅いっぱいにぼかす）。縁の両側に
    // 帯の幅 × softness / 2 ずつ広がる。帯の幅は値の取りうる範囲に切り詰めて
    // から測り、縁から範囲の端までの距離より広くはしない（applyAutoPaint）。
    double softness = 0.25;
};

// 既定の規則（**既定の帯の唯一の定義**。ブラウザは既定を持たず、/scene で
// これを受け取る）: 層 0 = 傾き 0〜30°、層 1 = 30〜50°、層 2 = 50〜90°、
// 層 3 = 無効。高さでは絞らない。
AutoPaintRules defaultAutoPaintRules();

// 規則を常識的な範囲へ: 傾きは 0〜90°、高さは ±kAutoPaintMaxHeight、softness は
// 0〜1。NaN・無限は既定（defaultAutoPaintRules の同じ層の値）へ倒し、下限が
// 上限を超えていれば入れ替える（min ≤ max）。
AutoPaintRules clampAutoPaintRules(AutoPaintRules rules);

// 全セルの重みを rules で書き直す（穴のセルは塗らない = setSplatWeights が飛ばす）。傾きはセルの 2 三角形の
// 面法線（HeightField::normalAt）の平均、高さはセルの中心の heightAt − baseY。
// 値の取りうる範囲（傾き 0〜90°、高さは格子の最低〜最高）に対して:
//   * 帯の端が範囲の端か外にあれば、その側はぼかさない（縁の外に値が無いのに
//     ぼかすと帯の端が薄くなる）。
//   * ぼかしの幅は、範囲に切り詰めた帯の幅から決め、縁から範囲の端までの距離で
//     頭打ちにする（片側だけ書いた帯 = 上限が既定の 10000 m でも、縁が全域へ
//     広がらない。範囲の少し内側の縁も範囲の外へはみ出さない）。
// rules は clampAutoPaintRules 済みであること。重みが 1 セルでも変われば true。
// 共有中の格子（shared_ptr<const>）には呼べない = コピーに対して呼ぶ。
bool applyAutoPaint(HeightField& field, const AutoPaintRules& rules);

}  // namespace terrain
}  // namespace wizengine
