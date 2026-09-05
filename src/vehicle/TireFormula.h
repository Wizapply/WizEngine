#pragma once

#include <string>
#include <vector>

#include "vehicle/Formula.h"

// タイヤの「スリップ → 力」をノード式に差し替えるときの約束。
//
// 入力（WheelController::tick が毎ティック渡す。順番は tireFormulaInputs()）:
//   kappa  … スリップ比            alpha  … スリップ角（rad）
//   fz     … 荷重 N                mu     … <tire mu>
//   v      … 分母に使う速度 max(|vx|, 0.5)
//   vx, vy … 接地点の速度（タイヤ座標）   omega … 車輪の ω   radius … 半径
//   bx cx ex by cy ey relax … <tire> の係数（式の中で使っても、無視して
//                            自分の定数を置いてもよい）
//   fxLo fxHi fyLo fyHi … 力の許される範囲。滑りの向きに逆らわず（片側は
//                常に 0）、このティックで滑り速度を反転させない大きさまで。
//                停車付近の振動と「古い符号の力が残って逆走する」を消す
//                上限で、既定グラフは緩和長の**前**にこれでクランプする
//                - 状態が範囲を超えて溜まらないように
// 出力: fx（縦力 N、進む向きが +）、fy（横力 N、右が +）。
// 安全のため WheelController は出力にももう一度この範囲を掛ける。
namespace wizengine {
namespace vehicle {

const std::vector<std::string>& tireFormulaInputs();
const std::vector<std::string>& tireFormulaOutputs();

// 組み込みの Magic Formula + 摩擦楕円 + 緩和長を、そのままノードで組んだ
// グラフ。組み込み実装と同じ数字になる（vehicle_test が検査する）。研究者は
// これを出発点にノードを差し替える。
FormulaGraphDesc defaultTireFormulaGraph(const std::string& name = "tire_mf");

}  // namespace vehicle
}  // namespace wizengine
