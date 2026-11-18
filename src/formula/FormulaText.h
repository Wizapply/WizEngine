#pragma once

#include <string>
#include <vector>

#include "formula/Formula.h"

// 数式のテキスト ⇄ ノードの計算式（FormulaGraphDesc）。
//
// 研究者はノードを並べる代わりに数式を書ける。書いた式はノードのグラフに
// 起こされ（formulaFromText）、ノードエディタでそのまま見える・直せる。
// 逆にどんなグラフもテキストに起こせる（formulaToText）ので、ノードで組んだ
// 式をテキストで直すこともできる。実行は従来どおりグラフ（C++ / LuaJIT）。
//
// ---- 書式 ---------------------------------------------------------------------
//   # コメント（// も可）。1 行に 1 文、; で区切れば 1 行に複数。
//   e = ref - y                     … 変数（名前を付けた途中の値）
//   out u = pid(e, 2, 1, 0.1)       … 出力（名前 u の out ノード）
//   joint:arm.target = u            … 出力（信号名。: を含む名前はそのまま書ける）
//   "my-box.fy" = u                 … 出力（" " で囲めばどんな名前でも）
//
//   * 定数を代入した変数は**名前付きの定数**（kp = 40）。係数の引数にも使える
//     （pid(e, kp, ki, kd)）。グラフでは数に畳まれる（テキストに起こし直すと
//     数で出る）。
//   * 代入されない名前は**入力**（in ノード）: ref、joint:arm.angle、"x-1" など。
//     time / dt / pi は予約語（時刻・刻み・円周率）。
//   * 変数は後で代入するものも参照できる（フィードバック）。ただし輪の中に
//     delay か integrate が無いとエラー（代数ループ）。
//   * 演算子: + - * / ^（べき）、単項 -、比較 > <（1 / 0）、括弧。
//     定数どうしの計算はその場で畳む（-40 は定数 1 個）。
//   * 関数（右の数字の引数は**定数**でなければならない = ノードの係数）:
//       sin cos tan atan exp log sqrt abs sign(x)   min max pow atan2(a, b)
//       clamp(x, lo, hi)  lerp(a, b, t)  select(c, a, b) = if(c, a, b)
//       lag(x, tau)  derivative(x)  integrate(x[, lo, hi])  delay(x[, init])
//       gain(x, k)  saturate(x, lo, hi)  deadzone(x, lo, hi)
//       ratelimit(x, rise, fall)  pid(e, kp[, ki, kd, lo, hi, tf])
//       curve(x, x0, y0, x1, y1, ...)
//     （clamp と saturate の違い: clamp の範囲は信号でよい、saturate は定数）
//
// 起こしたグラフは compile まで通して確かめる（循環や未知の関数はここで
// エラー = 壊れた式をアセットに入れない）。位置は依存の深さで左から並べる。
namespace wizengine {
namespace formula {

// テキスト → グラフ。構文エラー・実行できないグラフなら false で error に
// "line N: ..."（英語）。warnings は動くけれど怪しい内容（使われない変数など）。
bool formulaFromText(const std::string& text, const std::string& name,
                     FormulaGraphDesc& out, std::string& error,
                     std::vector<std::string>* warnings = nullptr);

// グラフ → テキスト。1 回しか使われない値は式の中へ埋め込み、何度も使う値・
// フィードバックで後から参照される値は変数（v<id>）にする。formulaFromText で
// 読み戻すと同じ計算になる（ノードの id と位置は変わる）。
std::string formulaToText(const FormulaGraphDesc& graph);

}  // namespace formula
}  // namespace wizengine
