#pragma once

#include <vector>

#include "vehicle/VehicleTypes.h"

// パワートレイン: エンジン → クラッチ → 変速機 → センターデフ → 軸デフ →
// 車輪、の 1 次元回転系（NWH の Powertrain に相当）。ここには位置も姿勢も
// 無く、各ノードが持つのは慣性と角速度だけ。
//
// 解き方（1 ティック）:
//   1. 角速度を車輪からエンジンへ戻す（駆動輪の平均 × 総減速比 = クラッチの
//      下流の速度）。慣性も比の 2 乗で割って合算する。
//   2. トルクをエンジンから車輪へ流す。クラッチは 2 質点の相対速度に対する
//      **陰解法**の粘性要素（上限 = 伝達容量）なので、どんな剛性でも安定。
//      デフのロックも同じ式（平均速度との差に比例、陰解法、Lsd は上限つき）。
//   3. エンジンの ω を積分する。車輪の ω は VehicleModel が積分する
//      （タイヤの反作用トルクを持っているのがそちらのため）。
//
// 変速の判断（updateShifting）は物理 1 ステップに 1 回。自動モードは回転数で
// 上げ下げし、停車中に後ろのペダルを踏み続けるとリバースに入る。
namespace wizengine {
namespace vehicle {

// 車輪の静的な情報（configure に渡す）。
struct WheelSpec {
    int axle = 0;         // VehicleDesc::axles の番号
    int side = -1;        // -1 = 左、+1 = 右
    double inertia = 1.2; // kg·m²
    bool driven = false;
};

class Powertrain {
public:
    void configure(const VehicleDesc& desc, const std::vector<WheelSpec>& wheels);
    void reset();

    // ステップに 1 回: 変速・方向の判断と、ペダルの役割の決定。
    // speedForward は車体の前向き速度（m/s）、driveOmega は駆動輪の平均 ω。
    void updateShifting(double stepDt, double speedForward, double throttleIn,
                        double brakeIn, double driveOmega);
    // 決定後の実効ペダル（リバース中は入れ替わる）。
    double throttle() const { return throttle_; }
    double brake() const { return brake_; }

    // 1 ティック: 車輪の ω を読み、各車輪への駆動トルク（Nm）を書く。
    // エンジンとクラッチの状態を進める。
    void tick(double dt, const std::vector<double>& wheelOmega,
              std::vector<double>& wheelDriveTorque);

    // 現在の総減速比（変速機 × ファイナル。ニュートラル / 変速中は 0）。
    double ratio() const;
    int gear() const { return gear_; }
    bool shifting() const { return shifting_; }
    double engineRpm() const;
    double engineOmega() const { return engineOmega_; }
    double clutchEngagement() const { return clutch_; }
    double engineTorque() const { return lastEngineTorque_; }

private:
    double engineTorqueAt(double omega, double throttle) const;
    // ロックトルク（陰解法）。dOmega = 目標との差、inertia = 受ける側の慣性。
    static double lockTorque(const DifferentialDesc& d, double dOmega,
                             double inertia, double dt);

    VehicleDesc desc_;
    std::vector<WheelSpec> wheels_;
    std::vector<int> drivenAxles_;  // 駆動軸の番号（重複なし）

    double engineOmega_ = 0.0;
    double clutch_ = 0.0;
    double throttle_ = 0.0;
    double brake_ = 0.0;
    double lastEngineTorque_ = 0.0;
    int gear_ = 0;
    bool shifting_ = false;
    int pendingGear_ = 0;
    double shiftTimer_ = 0.0;
    double stoppedTime_ = 0.0;   // 停車が続いている秒数
    double sinceShift_ = 1.0e6;  // 直前の変速からの秒数
};

}  // namespace vehicle
}  // namespace wizengine
