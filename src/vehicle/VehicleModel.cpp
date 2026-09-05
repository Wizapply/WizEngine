#include "vehicle/VehicleModel.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "vehicle/TireFormula.h"

namespace wizengine {
namespace vehicle {

namespace {
constexpr double kAirDensity = 1.225;  // kg/m³
}

VehicleModel::VehicleModel(const VehicleDesc& descIn, LuaRuntime* lua,
                           const std::vector<FormulaGraphDesc>* library)
    : desc_(clampVehicle(descIn)) {
    // タイヤのノード式: 名前ごとに 1 回コンパイルし、車輪ごとに実体を作る。
    std::map<std::string, std::shared_ptr<const FormulaProgram>> programs;
    auto programFor = [&](const std::string& name) -> std::shared_ptr<const FormulaProgram> {
        const auto it = programs.find(name);
        if (it != programs.end()) return it->second;
        const FormulaGraphDesc* graph = nullptr;
        if (library) {
            for (const FormulaGraphDesc& g : *library) {
                if (g.name == name) graph = &g;
            }
        }
        for (const FormulaGraphDesc& g : desc_.formulas) {
            if (!graph && g.name == name) graph = &g;
        }
        std::shared_ptr<const FormulaProgram> prog;
        if (!graph) {
            diagnostics_.push_back("tire formula \"" + name +
                                   "\" is not defined - using the built-in tire model");
        } else {
            std::vector<std::string> errors, warnings;
            prog = FormulaProgram::compile(*graph, tireFormulaInputs(), tireFormulaOutputs(),
                                           errors, warnings);
            for (const std::string& w : warnings) diagnostics_.push_back(w);
            for (const std::string& e : errors) diagnostics_.push_back(e);
            if (!prog) {
                diagnostics_.push_back("tire formula \"" + name +
                                       "\" cannot run - using the built-in tire model");
            }
        }
        programs[name] = prog;
        return prog;
    };
    std::map<std::string, std::string> backendUsed;

    std::vector<WheelSpec> specs;
    for (std::size_t a = 0; a < desc_.axles.size(); ++a) {
        const AxleDesc& axle = desc_.axles[a];
        for (int side = -1; side <= 1; side += 2) {
            Wheel w{WheelController(axle.tire, axle.suspension,
                                    Vec3{axle.halfTrack * side, axle.y, -axle.z}),
                    WheelSpec{}, 0.0, 0.0, 0.0};
            w.spec.axle = int(a);
            w.spec.side = side;
            w.spec.inertia = axle.tire.inertia;
            w.spec.driven = axle.driven;
            if (!axle.tire.formula.empty()) {
                if (auto prog = programFor(axle.tire.formula)) {
                    std::unique_ptr<FormulaInstance> inst;
                    std::string err;
                    if (lua && lua->ok()) inst = lua->instantiate(prog, err);
                    if (!inst) {
                        if (lua && lua->ok()) {
                            diagnostics_.push_back("tire formula \"" + axle.tire.formula +
                                                   "\": luajit failed (" + err +
                                                   ") - using the interpreter");
                        }
                        inst = prog->interpret();
                    }
                    backendUsed[axle.tire.formula] = inst->backend();
                    w.controller.setTireFormula(std::move(inst));
                }
            }
            specs.push_back(w.spec);
            wheels_.push_back(std::move(w));
        }
    }
    for (const auto& kv : backendUsed) {
        diagnostics_.push_back("tire formula \"" + kv.first + "\" -> " + kv.second);
    }
    // ホイールベース = 操舵軸から一番後ろの非操舵軸まで（アッカーマン用）。
    double rearMost = 0.0;
    bool hasRear = false;
    for (const AxleDesc& a : desc_.axles) {
        if (a.steerDeg > 0.0) continue;
        if (!hasRear || a.z < rearMost) {
            rearMost = a.z;
            hasRear = true;
        }
    }
    double frontMost = 0.0;
    bool hasFront = false;
    for (const AxleDesc& a : desc_.axles) {
        if (a.steerDeg <= 0.0) continue;
        if (!hasFront || a.z > frontMost) {
            frontMost = a.z;
            hasFront = true;
        }
    }
    wheelbase_ = (hasRear && hasFront) ? frontMost - rearMost : 0.0;
    powertrain_.configure(desc_, specs);
    telemetry_.wheels.resize(wheels_.size());
    reset();
}

void VehicleModel::reset() {
    powertrain_.reset();
    for (Wheel& w : wheels_) {
        w.controller.reset();
        w.omega = 0.0;
        w.brakeTorque = 0.0;
        w.driveTorque = 0.0;
    }
    target_ = VehicleInput{};
    smoothed_ = VehicleInput{};
    speed_ = 0.0;
    tcsCut_ = 0.0;
    forces_.clear();
    telemetry_ = VehicleTelemetry{};
    telemetry_.wheels.resize(wheels_.size());
}

int VehicleModel::formulaFailures() const {
    int n = 0;
    for (const Wheel& w : wheels_) n += w.controller.formulaFailures();
    return n;
}

double VehicleModel::wheelSteer(const AxleDesc& axle, int side) const {
    if (axle.steerDeg <= 0.0) return 0.0;
    // 速度感応: 速いほど最大舵角を絞る（キーボードのフル舵で前輪が飽和
    // しないように）。
    double limit = 1.0;
    if (desc_.steerSpeedRef > 0.0) {
        const double r = std::fabs(speed_) / desc_.steerSpeedRef;
        limit = 1.0 / (1.0 + r * r);
    }
    const double center = degToRad(axle.steerDeg) * smoothed_.steer * limit;
    if (std::fabs(center) < 1e-6 || axle.ackermann <= 0.0 || wheelbase_ < 0.1) {
        return center;
    }
    // 旋回中心までの距離 R から内輪・外輪の幾何学的な角度を出し、ackermann で
    // 平行舵とのあいだを混ぜる。+ = 左旋回なので、内輪は左（side = -1）。
    const double mag = std::fabs(center);
    const double radius = wheelbase_ / std::tan(mag);
    const bool inner = (center > 0.0) ? (side < 0) : (side > 0);
    const double geom =
        std::atan(wheelbase_ / std::max(radius + (inner ? -1.0 : 1.0) * axle.halfTrack,
                                        0.05));
    const double blended = mag + axle.ackermann * (geom - mag);
    return signd(center) * blended;
}

const std::vector<ForceAtPoint>& VehicleModel::step(const ChassisState& chassis,
                                                    double dt,
                                                    const GroundQuery& ground) {
    forces_.clear();
    if (dt <= 0.0 || wheels_.empty()) return forces_;

    // ---- 1. 入力の追従 -----------------------------------------------------
    auto follow = [dt](double& v, double t, double rate) {
        t = clampd(t, -1.0, 1.0);
        v += clampd(t - v, -rate * dt, rate * dt);
    };
    follow(smoothed_.throttle, clampd(target_.throttle, 0.0, 1.0), desc_.pedalRate);
    follow(smoothed_.brake, clampd(target_.brake, 0.0, 1.0), desc_.pedalRate);
    follow(smoothed_.handbrake, clampd(target_.handbrake, 0.0, 1.0), desc_.pedalRate);
    follow(smoothed_.steer, target_.steer, desc_.steerRate);

    // ---- 2. 変速と方向 -----------------------------------------------------
    const Vec3 forwardW = chassis.rotation.rotate(Vec3{0.0, 0.0, -1.0});
    const double speed = chassis.velocity.dot(forwardW);
    speed_ = speed;
    double driveOmega = 0.0;
    double driveSlip = 0.0;  // 駆動輪の最大スリップ比（前ステップの値）
    int driven = 0;
    for (const Wheel& w : wheels_) {
        if (!w.spec.driven) continue;
        driveOmega += w.omega;
        driveSlip = std::max(driveSlip, std::fabs(w.controller.telemetry().slipRatio));
        ++driven;
    }
    if (driven > 0) driveOmega /= driven;

    // トラクションコントロール: 駆動輪が空転し始めたらスロットルを絞る。
    // 絞る側は速く、戻す側はゆっくり（ばたつかせない）。
    double throttleIn = smoothed_.throttle;
    if (desc_.tractionControl > 0.0) {
        const double over = (driveSlip - desc_.tractionControl) / desc_.tractionControl;
        const double target = clampd(over, 0.0, 0.9);
        const double rate = target > tcsCut_ ? 20.0 : 4.0;
        tcsCut_ += clampd(target - tcsCut_, -rate * dt, rate * dt);
        throttleIn *= 1.0 - tcsCut_;
    }
    powertrain_.updateShifting(dt, speed, throttleIn, smoothed_.brake,
                               driveOmega);

    // ---- 3. 接地とサス -----------------------------------------------------
    const double massShare = chassis.mass / double(wheels_.size());
    for (Wheel& w : wheels_) {
        const AxleDesc& axle = desc_.axles[std::size_t(w.spec.axle)];
        w.controller.updateContact(chassis, wheelSteer(axle, w.spec.side), ground,
                                   massShare, dt);
        w.brakeTorque =
            desc_.brakeTorque * (powertrain_.brake() * axle.brakeBias +
                                 smoothed_.handbrake * axle.handbrake);
    }
    // アンチロールバー: 左右の縮み差に比例した荷重を、縮んだ側に足し、伸びた
    // 側から引く。左右がそろって接地しているときだけ。
    for (std::size_t i = 0; i + 1 < wheels_.size(); i += 2) {
        Wheel& left = wheels_[i];
        Wheel& right = wheels_[i + 1];
        const AxleDesc& axle = desc_.axles[std::size_t(left.spec.axle)];
        if (!left.controller.grounded() || !right.controller.grounded()) continue;
        const double d = left.controller.compression() - right.controller.compression();
        const double f = axle.suspension.antiRoll * d;
        left.controller.addLoad(f);
        right.controller.addLoad(-f);
    }

    // ---- 4. パワートレインの反復 ------------------------------------------
    const int ticks = std::max(desc_.ticks, 1);
    const double tickDt = dt / ticks;
    scratchOmega_.resize(wheels_.size());
    for (int t = 0; t < ticks; ++t) {
        for (std::size_t i = 0; i < wheels_.size(); ++i) {
            scratchOmega_[i] = wheels_[i].omega;
        }
        powertrain_.tick(tickDt, scratchOmega_, scratchTorque_);
        for (std::size_t i = 0; i < wheels_.size(); ++i) {
            Wheel& w = wheels_[i];
            w.driveTorque = scratchTorque_[i];
            const double net = w.driveTorque - w.controller.reactionTorque();
            w.omega += tickDt * net / w.spec.inertia;
            // ブレーキと転がり抵抗はクーロン的: このティックで止められる
            // なら止める（符号が反転してばたつくのを防ぐ）。
            const double resist = w.brakeTorque + w.controller.rollingTorque();
            const double dw = resist * tickDt / w.spec.inertia;
            if (std::fabs(w.omega) <= dw) {
                w.omega = 0.0;
            } else {
                w.omega -= signd(w.omega) * dw;
            }
            w.controller.tick(w.omega, tickDt);
            w.controller.addSpin(w.omega * tickDt);
        }
    }

    // ---- 5. 車体へ掛ける力 -------------------------------------------------
    for (Wheel& w : wheels_) {
        w.controller.finishStep(ticks);
        if (!w.controller.grounded()) continue;
        forces_.push_back({w.controller.suspensionForce(), w.controller.suspensionPoint()});
        forces_.push_back({w.controller.tireForce(), w.controller.contactPoint()});
    }

    // ---- 6. 空気抵抗 -------------------------------------------------------
    const double v = chassis.velocity.length();
    if (desc_.dragArea > 0.0 && v > 1e-3) {
        const Vec3 drag = chassis.velocity * (-0.5 * kAirDensity * desc_.dragArea * v);
        forces_.push_back({drag, chassis.position});
    }

    // ---- 計測値 -------------------------------------------------------------
    telemetry_.rpm = powertrain_.engineRpm();
    telemetry_.gear = powertrain_.gear();
    telemetry_.shifting = powertrain_.shifting();
    telemetry_.speed = speed;
    telemetry_.clutch = powertrain_.clutchEngagement();
    telemetry_.engineTorque = powertrain_.engineTorque();
    for (std::size_t i = 0; i < wheels_.size(); ++i) {
        telemetry_.wheels[i] = wheels_[i].controller.telemetry();
    }
    return forces_;
}

std::vector<WheelLocalPose> VehicleModel::wheelLocalPoses() const {
    std::vector<WheelLocalPose> out;
    out.reserve(wheels_.size());
    for (const Wheel& w : wheels_) {
        const AxleDesc& axle = desc_.axles[std::size_t(w.spec.axle)];
        WheelLocalPose p;
        p.attach = w.controller.localAttach();
        p.drop = axle.suspension.restLength - w.controller.compression();
        p.steerRad = w.controller.steerRad();
        p.spinAngle = w.controller.spinAngle();
        p.radius = axle.tire.radius;
        p.width = axle.tire.width;
        out.push_back(p);
    }
    return out;
}

std::vector<WheelLocalPose> VehicleModel::designWheelPoses(const VehicleDesc& descIn,
                                                           double chassisMass) {
    const VehicleDesc desc = clampVehicle(descIn);
    std::vector<WheelLocalPose> out;
    const double wheels = double(desc.axles.size() * 2);
    for (const AxleDesc& axle : desc.axles) {
        const double sag = wheels > 0.0
            ? clampd(chassisMass * 9.81 / (wheels * axle.suspension.stiffness), 0.0,
                     axle.suspension.travel)
            : 0.0;
        for (int side = -1; side <= 1; side += 2) {
            WheelLocalPose p;
            p.attach = Vec3{axle.halfTrack * side, axle.y, -axle.z};
            p.drop = axle.suspension.restLength - sag;
            p.radius = axle.tire.radius;
            p.width = axle.tire.width;
            out.push_back(p);
        }
    }
    return out;
}

std::vector<WheelPose> VehicleModel::wheelPoses(const ChassisState& chassis) const {
    std::vector<WheelPose> out;
    out.reserve(wheels_.size());
    for (const Wheel& w : wheels_) {
        WheelPose p;
        p.radius = w.controller.radius();
        p.width = desc_.axles[std::size_t(w.spec.axle)].tire.width;
        p.center = w.controller.wheelCenter();
        // 舵（車体の上向きまわり）× 回転（車軸まわり。前へ転がる = -X まわりの正）。
        const Quat steer = Quat::fromAxisAngle(Vec3{0.0, 1.0, 0.0}, w.controller.steerRad());
        const Quat spin = Quat::fromAxisAngle(Vec3{-1.0, 0.0, 0.0}, w.controller.spinAngle());
        p.rotation = (chassis.rotation * steer * spin).normalized();
        out.push_back(p);
    }
    return out;
}

}  // namespace vehicle
}  // namespace wizengine
