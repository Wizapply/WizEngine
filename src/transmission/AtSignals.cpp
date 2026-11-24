#include "transmission/AtSignals.h"

#include <cmath>
#include <cstddef>

namespace wizengine {
namespace transmission {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRpm = 60.0 / (2.0 * kPi);

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

enum SensorId {
    sEngineRpm, sTurbineRpm, sOutputRpm, sWheelRpm, sEngineTorque, sLoadTorque,
    sTurbineTorque, sInputTorque, sOutputTorque, sAxleTorque, sGear, sTargetGear,
    sShifting, sPhase, sRatio, sSpeedRatio, sTorqueRatio, sTcSlip, sLockup, sLockupSlip,
    sSpeed, sSpeedKmh, sAccel, sEfficiency, sTorqueRequest, sLinePressure, sPowerIn,
    sPowerOut, sCount
};
// 要素・軸の量は id を 1000 番台から（種類 * 1000 + 番号）。
constexpr int kElemTorque = 1000, kElemPressure = 2000, kElemSlip = 3000, kShaftRpm = 4000,
              kShaftW = 5000;

enum InputIdx { iEngineTorque, iEngineRpm, iThrottle, iBrake, iSelect, iManual, iOutputRpm,
                iGrade, iCouple };

double at(const std::vector<double>& v, int i) {
    return i >= 0 && std::size_t(i) < v.size() ? v[std::size_t(i)] : 0.0;
}

}  // namespace

const std::vector<std::string>& atCommonSensorNames() {
    static const std::vector<std::string> v = {
        "engine_rpm", "turbine_rpm", "output_rpm", "wheel_rpm", "engine_torque", "load_torque",
        "turbine_torque", "input_torque", "output_torque", "axle_torque", "gear", "target_gear",
        "shifting", "phase", "ratio", "tc_speed_ratio", "tc_torque_ratio", "tc_slip", "lockup",
        "lockup_slip", "speed", "speed_kmh", "accel", "efficiency", "torque_request",
        "line_pressure", "power_in", "power_out"};
    return v;
}

int atSensorId(const AtDesc& desc, const std::string& q) {
    const auto& names = atCommonSensorNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (q == names[i]) return int(i);
    }
    auto suffix = [&](const char* prefix, std::string& rest) {
        const std::size_t n = std::char_traits<char>::length(prefix);
        if (q.size() > n && q.compare(0, n, prefix) == 0) {
            rest = q.substr(n);
            return true;
        }
        return false;
    };
    auto elem = [&](const std::string& name) {
        for (std::size_t e = 0; e < desc.elements.size(); ++e) {
            if (desc.elements[e].name == name) return int(e);
        }
        return -1;
    };
    auto shaftIdx = [&](const std::string& name) {
        for (std::size_t s = 0; s < desc.shafts.size(); ++s) {
            if (desc.shafts[s].name == name) return int(s);
        }
        return -1;
    };
    std::string rest;
    if (suffix("torque_", rest) && elem(rest) >= 0) return kElemTorque + elem(rest);
    if (suffix("pressure_", rest) && elem(rest) >= 0) return kElemPressure + elem(rest);
    if (suffix("slip_", rest) && elem(rest) >= 0) return kElemSlip + elem(rest);
    if (suffix("rpm_", rest) && shaftIdx(rest) >= 0) return kShaftRpm + shaftIdx(rest);
    if (suffix("w_", rest) && shaftIdx(rest) >= 0) return kShaftW + shaftIdx(rest);
    return -1;
}

double atSensor(const AtReadout& t, int id) {
    if (id >= kElemTorque) {
        const int kind = id / 1000 * 1000, i = id % 1000;
        switch (kind) {
            case kElemTorque: return at(t.elementTorque, i);
            case kElemPressure: return at(t.elementPressure, i);
            case kElemSlip: return at(t.elementSlipRpm, i);
            case kShaftRpm: return at(t.shaftW, i) * kRpm;
            case kShaftW: return at(t.shaftW, i);
            default: return 0.0;
        }
    }
    switch (id) {
        case sEngineRpm: return t.engineRpm;
        case sTurbineRpm: return t.turbineRpm;
        case sOutputRpm: return t.outputRpm;
        case sWheelRpm: return t.wheelRpm;
        case sEngineTorque: return t.engineTorque;
        case sLoadTorque: return t.loadTorque;
        case sTurbineTorque: return t.turbineTorque;
        case sInputTorque: return t.inputTorque;
        case sOutputTorque: return t.outputTorque;
        case sAxleTorque: return t.axleTorque;
        case sGear: return double(t.gear);
        case sTargetGear: return double(t.targetGear);
        case sShifting: return t.shifting ? 1.0 : 0.0;
        case sPhase: return double(t.phase);
        case sRatio: return t.ratio;
        case sSpeedRatio: return t.speedRatio;
        case sTorqueRatio: return t.torqueRatio;
        case sTcSlip: return t.tcSlipRpm;
        case sLockup: return t.lockup;
        case sLockupSlip: return t.lockupSlipRpm;
        case sSpeed: return t.speed;
        case sSpeedKmh: return t.speed * 3.6;
        case sAccel: return t.accel;
        case sEfficiency: return t.efficiency;
        case sTorqueRequest: return t.torqueRequest;
        case sLinePressure: return t.linePressure;
        case sPowerIn: return t.powerIn;
        case sPowerOut: return t.powerOut;
        default: return 0.0;
    }
}

std::vector<std::string> atSensorNames(const AtDesc& desc) {
    std::vector<std::string> v = atCommonSensorNames();
    for (const auto& e : desc.elements) {
        v.push_back("torque_" + e.name);
        v.push_back("pressure_" + e.name);
        v.push_back("slip_" + e.name);
    }
    for (const auto& s : desc.shafts) {
        v.push_back("rpm_" + s.name);
        v.push_back("w_" + s.name);
    }
    return v;
}

const std::vector<std::string>& atInputNames() {
    static const std::vector<std::string> v = {"engine_torque", "engine_rpm", "throttle", "brake",
                                               "select", "manual", "output_rpm", "grade", "couple"};
    return v;
}

int atInputId(const std::string& q) {
    const auto& v = atInputNames();
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == q) return int(i);
    }
    return -1;
}

void atSetInput(AtInputs& in, int id, double value) {
    if (!std::isfinite(value)) return;
    switch (id) {
        case iEngineTorque: in.engineTorque = value; break;
        case iEngineRpm: in.engineRpm = value; break;
        case iThrottle: in.throttle = clampd(value, 0.0, 1.0); break;
        case iBrake: in.brake = clampd(value, 0.0, 1.0); break;
        case iSelect: in.select = int(std::lround(clampd(value, 0.0, 4.0))); break;
        case iManual: in.manualGear = int(std::lround(clampd(value, 1.0, 99.0))); break;
        case iOutputRpm:
            in.outputRpm = value;
            in.outputRpmSet = true;
            break;
        case iGrade: in.grade = clampd(value, -0.6, 0.6); break;
        case iCouple: in.couple = clampd(value, 0.0, 1.0); break;
        default: break;
    }
}

}  // namespace transmission
}  // namespace wizengine
