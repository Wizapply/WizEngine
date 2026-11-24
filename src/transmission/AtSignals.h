#pragma once

#include <string>
#include <vector>

#include "transmission/AtControl.h"
#include "transmission/AtTypes.h"

// AT の計測値と、制御器の信号名（trans:<名前>.<量>）の表。純粋な値型。
// 値を詰めるのは動力学の側（components/AtDriveline が Chrono のシャフトから）。
namespace wizengine {
namespace transmission {

struct AtReadout {
    double engineRpm = 0, turbineRpm = 0, outputRpm = 0, wheelRpm = 0;
    double engineTorque = 0;   // エンジンが出したトルク（input = torque は絞りの後、他は load と同じ）
    double loadTorque = 0;     // AT がエンジン軸から取ったトルク（流体 + ロックアップ + ポンプ）
    double turbineTorque = 0;  // トルコンがタービンへ渡した流体のトルク
    double inputTorque = 0;    // 歯車列の入力軸へ入ったトルク（流体 + ダンパ）
    double outputTorque = 0;   // 出力軸のトルク（終減速の前）
    double axleTorque = 0;     // 車輪のトルク（終減速の後、左右合計）
    int gear = 0;              // 1..9、-1 = R、0 = N / P / 変速の準備
    int targetGear = 0;
    bool shifting = false;
    int phase = 0;             // 0 なし、1 準備、2 トルク相、3 イナーシャ相、4 同期
    double ratio = 0;          // 実際の ω_turbine / ω_output
    double speedRatio = 0, torqueRatio = 0;  // トルコンの SR / TR
    double tcSlipRpm = 0;      // エンジン − タービン
    double lockup = 0;         // ロックアップの油圧 0..1
    double lockupSlipRpm = 0;
    double speed = 0;          // m/s（車両）
    double accel = 0;          // m/s²
    double efficiency = 0;     // 出力 / 入力（なまし）
    double torqueRequest = 1;  // TCU のエンジントルク要求（絞りの倍率）
    double linePressure = 0;   // bar
    double powerIn = 0, powerOut = 0;  // kW
    // 要素ごと（desc.elements の並び）: トルク N·m（軸 a に掛かる反トルク）・油圧 0..1・
    // すべり rpm（a − b）。軸ごと（desc.shafts の並び）: 角速度 rad/s。
    std::vector<double> elementTorque, elementPressure, elementSlipRpm, shaftW;
};

// 読める量の名前 → 番号（無ければ -1）。要素ごと torque_<要素> pressure_<要素>
// slip_<要素>・軸ごと rpm_<軸> w_<軸> を含む。
int atSensorId(const AtDesc& desc, const std::string& quantity);
double atSensor(const AtReadout& r, int id);
std::vector<std::string> atSensorNames(const AtDesc& desc);
// 要素・軸を含まない共通の名前（/scene の候補、ControlSignals.h と同じ並び）。
const std::vector<std::string>& atCommonSensorNames();

// 書ける量（AtInputs の項目）。
int atInputId(const std::string& quantity);
const std::vector<std::string>& atInputNames();
void atSetInput(AtInputs& in, int id, double value);

}  // namespace transmission
}  // namespace wizengine
