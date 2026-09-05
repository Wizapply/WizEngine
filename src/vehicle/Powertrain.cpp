#include "vehicle/Powertrain.h"

#include <algorithm>
#include <cmath>

#include "vehicle/VehicleMath.h"

namespace wizengine {
namespace vehicle {

namespace {
// リバースへ入れる / 前進へ戻す判断をする「停車」の速度（m/s）。
constexpr double kStandstill = 0.6;
// ロックデフに使う剛性（Nm per rad/s）。陰解法なので大きくてよい。
constexpr double kLockedStiffness = 1.0e6;
// クラッチが「つながった」とみなす粘性剛性の倍率（× 容量）。
constexpr double kClutchStiffnessPerCapacity = 50.0;
// 自動クラッチのつながる速さ（1/s）。速すぎると発進で回転が上下に暴れる。
constexpr double kClutchRateUp = 4.0;
constexpr double kClutchRateDown = 12.0;
// 前進 ⇄ 後退を切り替えるのに要る停車時間（s）。
constexpr double kDirectionDwell = 0.3;
// 変速の判断をするのに要るクラッチのつながり具合と、変速後の待ち時間（s）。
constexpr double kShiftClutchMin = 0.9;
constexpr double kShiftCooldown = 0.5;
}  // namespace

void Powertrain::configure(const VehicleDesc& desc,
                           const std::vector<WheelSpec>& wheels) {
    desc_ = desc;
    wheels_ = wheels;
    drivenAxles_.clear();
    for (const WheelSpec& w : wheels_) {
        if (!w.driven) continue;
        if (std::find(drivenAxles_.begin(), drivenAxles_.end(), w.axle) ==
            drivenAxles_.end()) {
            drivenAxles_.push_back(w.axle);
        }
    }
    reset();
}

void Powertrain::reset() {
    engineOmega_ = rpmToRad(desc_.engine.idleRpm);
    clutch_ = 0.0;
    throttle_ = brake_ = 0.0;
    lastEngineTorque_ = 0.0;
    gear_ = 0;
    shifting_ = false;
    pendingGear_ = 0;
    shiftTimer_ = 0.0;
    stoppedTime_ = 0.0;
    sinceShift_ = 1.0e6;
}

double Powertrain::ratio() const {
    if (shifting_ || gear_ == 0) return 0.0;
    const double g = gear_ < 0
        ? -desc_.gearbox.reverse
        : desc_.gearbox.gears[std::size_t(std::min<int>(
              gear_, int(desc_.gearbox.gears.size())) - 1)];
    return g * desc_.gearbox.finalDrive;
}

double Powertrain::engineRpm() const { return radToRpm(engineOmega_); }

double Powertrain::engineTorqueAt(double omega, double throttle) const {
    const EngineDesc& e = desc_.engine;
    const double rpm = radToRpm(omega);
    double thr = clampd(throttle, 0.0, 1.0);
    if (rpm > e.maxRpm) thr = 0.0;  // レブリミッタ
    const double curve = e.torqueCurve.empty()
        ? sampleCurve(defaultTorqueCurve(), rpm)
        : sampleCurve(e.torqueCurve, rpm);
    double t = thr * e.maxTorque * curve - (e.frictionConst + e.friction * omega);
    // アイドル制御: 目標より下がった分に比例して押し上げる（上限は最大
    // トルクの 35%）。クラッチが切れている停車中に止まらないための最低限。
    if (rpm < e.idleRpm) {
        const double deficit = (e.idleRpm - rpm) / e.idleRpm;
        t += clampd(deficit * 4.0, 0.0, 1.0) * 0.35 * e.maxTorque;
    }
    return t;
}

double Powertrain::lockTorque(const DifferentialDesc& d, double dOmega,
                              double inertia, double dt) {
    if (d.kind == DiffKind::Open) return 0.0;
    const double k = d.kind == DiffKind::Locked ? kLockedStiffness : d.stiffness;
    if (k <= 0.0) return 0.0;
    double t = k * dOmega / (1.0 + k * dt / std::max(inertia, 1e-6));
    if (d.kind == DiffKind::Lsd) t = clampd(t, -d.lockTorque, d.lockTorque);
    return t;
}

void Powertrain::updateShifting(double stepDt, double speedForward,
                                double throttleIn, double brakeIn,
                                double driveOmega) {
    const GearboxDesc& gb = desc_.gearbox;
    const int top = int(gb.gears.size());
    const bool stopped = std::fabs(speedForward) < kStandstill;
    stoppedTime_ = stopped ? stoppedTime_ + stepDt : 0.0;
    if (sinceShift_ < 1.0e6) sinceShift_ += stepDt;

    if (shifting_) {
        shiftTimer_ -= stepDt;
        if (shiftTimer_ <= 0.0) {
            shifting_ = false;
            gear_ = pendingGear_;
            sinceShift_ = 0.0;
        }
    } else {
        auto startShift = [&](int target) {
            if (target == gear_) return;
            pendingGear_ = target;
            if (gb.shiftTime > 1e-6) {
                shifting_ = true;
                shiftTimer_ = gb.shiftTime;
            } else {
                gear_ = target;
                sinceShift_ = 0.0;
            }
        };
        if (gear_ == 0) {
            startShift(1);  // 最初は 1 速に入れておく
        } else if (gb.automatic) {
            // 停車が少し続いてからペダルで前進 / 後退を選ぶ（止まった瞬間に
            // 踏んだままの足でひっくり返らないように）。
            const bool dwelt = stoppedTime_ > kDirectionDwell;
            if (dwelt && gear_ > 0 && brakeIn > 0.1 && throttleIn < 0.1) {
                startShift(-1);
            } else if (dwelt && gear_ < 0 && throttleIn > 0.1 && brakeIn < 0.1) {
                startShift(1);
            } else if (gear_ > 0 && clutch_ > kShiftClutchMin &&
                       sinceShift_ > kShiftCooldown) {
                // 判断は駆動系の回転数で（クラッチが滑っている最中のエンジン
                // 回転は当てにならない）。つながっていれば両者は同じ値。
                const double lineRpm =
                    radToRpm(std::fabs(driveOmega) * std::fabs(ratio()));
                if (lineRpm > gb.shiftUpRpm && gear_ < top) {
                    startShift(gear_ + 1);
                } else if (lineRpm < gb.shiftDownRpm && gear_ > 1) {
                    startShift(gear_ - 1);
                }
            }
        }
    }

    // リバース中はペダルの役割を入れ替える（S で下がり、W で止まる）。
    if (gear_ < 0) {
        throttle_ = brakeIn;
        brake_ = throttleIn;
    } else {
        throttle_ = throttleIn;
        brake_ = brakeIn;
    }
}

void Powertrain::tick(double dt, const std::vector<double>& wheelOmega,
                      std::vector<double>& wheelDriveTorque) {
    wheelDriveTorque.assign(wheels_.size(), 0.0);
    const EngineDesc& eng = desc_.engine;
    const double r = ratio();

    // ---- 1. 車輪 → エンジンへ角速度と慣性を戻す --------------------------
    double omegaMean = 0.0;
    double inertiaSum = 0.0;
    int drivenCount = 0;
    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        if (!wheels_[i].driven) continue;
        omegaMean += wheelOmega[i];
        inertiaSum += wheels_[i].inertia;
        ++drivenCount;
    }
    const bool engaged = r != 0.0 && drivenCount > 0;
    if (drivenCount > 0) omegaMean /= drivenCount;
    const double omegaClutch = engaged ? omegaMean * r : 0.0;
    const double inertiaDown = engaged ? inertiaSum / (r * r) : 0.0;

    // ---- 2. クラッチ（自動）------------------------------------------------
    double target = 0.0;
    if (engaged) {
        const double span = std::max(desc_.clutch.fullRpm - desc_.clutch.engageRpm, 1.0);
        const double byEngine =
            clampd((radToRpm(engineOmega_) - desc_.clutch.engageRpm) / span, 0.0, 1.0);
        // 駆動系が回っている（走っている）なら、エンジン回転が落ちても
        // つないだままにする = エンジンブレーキが効き、高速で切れない。
        const double byLine =
            clampd((radToRpm(std::fabs(omegaClutch)) - desc_.clutch.engageRpm) / span,
                   0.0, 1.0);
        target = std::max(byEngine, byLine);
    }
    const double rate = target > clutch_ ? kClutchRateUp : kClutchRateDown;
    clutch_ += clampd(target - clutch_, -rate * dt, rate * dt);

    double clutchTorque = 0.0;
    if (engaged && clutch_ > 0.0) {
        const double cap = clutch_ * desc_.clutch.maxTorque;
        const double k = kClutchStiffnessPerCapacity * desc_.clutch.maxTorque;
        const double slip = engineOmega_ - omegaClutch;
        // 2 質点（エンジン・駆動系）の相対速度に対する陰解法。
        const double denom =
            1.0 + k * dt * (1.0 / eng.inertia + 1.0 / std::max(inertiaDown, 1e-6));
        clutchTorque = clampd(k * slip / denom, -cap, cap);
    }

    // ---- 3. エンジン -------------------------------------------------------
    lastEngineTorque_ = engineTorqueAt(engineOmega_, throttle_);
    engineOmega_ += dt * (lastEngineTorque_ - clutchTorque) / eng.inertia;
    if (engineOmega_ < 0.0) engineOmega_ = 0.0;

    if (!engaged) return;

    // ---- 4. 変速機 → センターデフ → 軸デフ → 車輪 --------------------------
    const double outTorque = clutchTorque * r * desc_.gearbox.efficiency;

    // 軸ごとの平均速度と慣性（センターデフのロックに使う）。
    struct AxleAgg {
        int axle = 0;
        double omega = 0.0;
        double inertia = 0.0;
        int count = 0;
        double torque = 0.0;
    };
    std::vector<AxleAgg> axles;
    for (int a : drivenAxles_) {
        AxleAgg agg;
        agg.axle = a;
        for (std::size_t i = 0; i < wheels_.size(); ++i) {
            if (!wheels_[i].driven || wheels_[i].axle != a) continue;
            agg.omega += wheelOmega[i];
            agg.inertia += wheels_[i].inertia;
            ++agg.count;
        }
        if (agg.count > 0) agg.omega /= agg.count;
        axles.push_back(agg);
    }

    if (axles.size() == 1) {
        axles[0].torque = outTorque;
    } else {
        // 2 軸なら bias（前軸の割合。前 = z の大きい方）、3 軸以上は等分。
        double meanAxleOmega = 0.0;
        for (const AxleAgg& a : axles) meanAxleOmega += a.omega;
        meanAxleOmega /= double(axles.size());
        int frontIdx = 0;
        for (std::size_t i = 1; i < axles.size(); ++i) {
            if (desc_.axles[std::size_t(axles[i].axle)].z >
                desc_.axles[std::size_t(axles[frontIdx].axle)].z) {
                frontIdx = int(i);
            }
        }
        for (std::size_t i = 0; i < axles.size(); ++i) {
            double w = 1.0 / double(axles.size());
            if (axles.size() == 2) {
                w = int(i) == frontIdx ? desc_.center.bias : 1.0 - desc_.center.bias;
            }
            axles[i].torque =
                outTorque * w +
                lockTorque(desc_.center, meanAxleOmega - axles[i].omega,
                           axles[i].inertia, dt);
        }
    }

    for (const AxleAgg& a : axles) {
        const DifferentialDesc& diff = desc_.axles[std::size_t(a.axle)].diff;
        for (std::size_t i = 0; i < wheels_.size(); ++i) {
            if (!wheels_[i].driven || wheels_[i].axle != a.axle) continue;
            const double share = a.count > 0 ? a.torque / a.count : 0.0;
            wheelDriveTorque[i] =
                share + lockTorque(diff, a.omega - wheelOmega[i],
                                   wheels_[i].inertia, dt);
        }
    }
}

}  // namespace vehicle
}  // namespace wizengine
