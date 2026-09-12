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

// ---- ソフトタイヤの空気圧（体積 → 圧力）------------------------------------
// SoftTire が毎ステップ、トレッドが囲む体積から「ゲージ圧の増分」を求める式。
// 入力（順番は pressureFormulaInputs()）:
//   ratio      … 体積比 V / V0（静止で 1、潰れると小さくなる）
//   rate       … 体積の変化率 (dV/dt) / V0（1/s、縮む向きが負）
//   p0         … <tire pressure>（絶対圧 Pa。既定 320000 = 2.2 bar ゲージ）
//   load       … 荷重 Fz（N）    deflection … 物理側の潰れ (m)   radius … 半径
// 出力: pressure（Pa。静止からの増分で、正がトレッドを外へ押す。SoftTire は
//   各粒子の面積ベクトル × pressure を力として掛ける）。
// 既定グラフは等温変化 p = p0 (V0/V − 1)（下限 −p0 = 真空）。潰すほど急に
// 硬くなり、片側を潰すと反対側が膨らむ。
const std::vector<std::string>& pressureFormulaInputs();
const std::vector<std::string>& pressureFormulaOutputs();
FormulaGraphDesc defaultPressureFormulaGraph(const std::string& name = "tire_air");
// 組み込みの空気圧（既定グラフと同じ式）。式が無い / 失敗したときの代用。
double builtinTirePressure(double ratio, double p0);

}  // namespace vehicle
}  // namespace wizengine
