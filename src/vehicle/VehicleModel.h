#pragma once

#include <string>
#include <vector>

#include "vehicle/LuaFormula.h"
#include "vehicle/Powertrain.h"
#include "vehicle/VehicleMath.h"
#include "vehicle/VehicleTypes.h"
#include "vehicle/WheelController.h"

// 1 台の車両（NWH の VehicleController に相当）。車体の状態を受け取り、
// パワートレインを N ティック回し、車輪ごとの WheelController から「車体へ
// 掛ける力の一覧」を作る。Chrono を知らないので、単体で回してテストできる
// （tests/vehicle/vehicle_test.cpp）。
//
// 1 ステップの流れ:
//   1. 入力の追従（舵・ペダルの速さ制限）
//   2. 変速と方向の判断（Powertrain::updateShifting）
//   3. 各輪の接地・サス力（WheelController::updateContact）、アンチロールバー
//   4. パワートレイン N ティック: 駆動トルク → 車輪の ω を積分 → ブレーキ・
//      転がり抵抗のクランプ → タイヤ力の更新（反作用は次のティックへ）
//   5. 各輪の力を平均して、サス力（取り付け点）とタイヤ力（接地点）を出す
//   6. 空気抵抗（重心）
namespace wizengine {
namespace vehicle {

struct ForceAtPoint {
    Vec3 force;   // N、ワールド
    Vec3 point;   // 作用点、ワールド
};

// 見た目用の車輪の姿勢（ワールド）。
struct WheelPose {
    Vec3 center;
    Quat rotation;
    double radius = 0.3;
    double width = 0.2;
};

// 見た目用の車輪の姿勢（車体ローカル）。描画側は「描いている車体の姿勢」に
// これを重ねるので、物理スナップショットとの 1 ステップのずれが出ない。
//   中心 = attach - (0, drop, 0)、回転 = Ry(steer) * Rx(-spin)
struct WheelLocalPose {
    Vec3 attach;             // 取り付け点（車体ローカル）
    double drop = 0.0;       // 取り付け点から車輪中心までの下がり（m）
    double steerRad = 0.0;
    double spinAngle = 0.0;  // rad、前へ転がるほど増える
    double radius = 0.3;
    double width = 0.2;
};

class VehicleModel {
public:
    // lua があればノード式（<tire formula>）を LuaJIT で回す。無ければ C++ の
    // 参照インタプリタ。どちらも同じ命令列なので結果は同じ（速さが違う）。
    // library はシーンの計算式アセット（名前で引く。desc.formulas より優先）。
    // 生成時にコンパイルするので、生成後は参照しない。
    explicit VehicleModel(const VehicleDesc& desc, LuaRuntime* lua = nullptr,
                          const std::vector<FormulaGraphDesc>* library = nullptr);

    // 生成時の診断（式のコンパイル結果・警告・どのバックエンドか）。
    // 呼び出し側（VehicleComponent）がログへ出す。
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }
    // 式が失敗して組み込みで代用したティックの総数（全輪）。
    int formulaFailures() const;

    void reset();
    void setInput(const VehicleInput& input) { target_ = input; }
    const VehicleInput& input() const { return target_; }

    // 物理 1 ステップ。返す力は「この dt のあいだ掛け続ける力」。
    const std::vector<ForceAtPoint>& step(const ChassisState& chassis, double dt,
                                          const GroundQuery& ground);

    const VehicleTelemetry& telemetry() const { return telemetry_; }
    const VehicleDesc& desc() const { return desc_; }
    std::size_t wheelCount() const { return wheels_.size(); }
    std::vector<WheelPose> wheelPoses(const ChassisState& chassis) const;
    std::vector<WheelLocalPose> wheelLocalPoses() const;
    // 設計値だけから（走らせる前・エディタ中の表示用）。静的な沈み込みを
    // 車体質量から見積もって drop に入れる。
    static std::vector<WheelLocalPose> designWheelPoses(const VehicleDesc& desc,
                                                        double chassisMass);

private:
    struct Wheel {
        WheelController controller;
        WheelSpec spec;
        double omega = 0.0;
        double brakeTorque = 0.0;
        double driveTorque = 0.0;
    };

    // この軸の舵角（rad、左右別）。アッカーマンは後軸までの距離から。
    double wheelSteer(const AxleDesc& axle, int side) const;

    VehicleDesc desc_;
    Powertrain powertrain_;
    std::vector<Wheel> wheels_;
    std::vector<std::string> diagnostics_;
    VehicleInput target_;
    VehicleInput smoothed_;
    double wheelbase_ = 2.6;
    double speed_ = 0.0;    // 前向き速度（舵角の絞りに使う）
    double tcsCut_ = 0.0;   // トラクションコントロールが絞っている割合 0..1
    std::vector<ForceAtPoint> forces_;
    VehicleTelemetry telemetry_;
    std::vector<double> scratchOmega_;
    std::vector<double> scratchTorque_;
};

}  // namespace vehicle
}  // namespace wizengine
