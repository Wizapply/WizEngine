#include "vehicle/VehicleTypes.h"

#include "vehicle/VehicleMath.h"

namespace wizengine {
namespace vehicle {

double sampleCurve(const std::vector<CurvePoint>& curve, double x,
                   double fallback) {
    if (curve.empty()) return fallback;
    if (x <= curve.front().x) return curve.front().y;
    if (x >= curve.back().x) return curve.back().y;
    for (std::size_t i = 1; i < curve.size(); ++i) {
        const CurvePoint& a = curve[i - 1];
        const CurvePoint& b = curve[i];
        if (x <= b.x) {
            const double span = b.x - a.x;
            const double t = span > 1e-12 ? (x - a.x) / span : 0.0;
            return a.y + (b.y - a.y) * t;
        }
    }
    return curve.back().y;
}

std::vector<CurvePoint> defaultTorqueCurve() {
    // 自然吸気の乗用車らしい形: 低回転で細く、中回転でピーク、高回転で落ちる。
    return {{0.0, 0.35},    {1000.0, 0.55}, {2000.0, 0.75}, {3000.0, 0.90},
            {4000.0, 1.00}, {5000.0, 0.97}, {6000.0, 0.85}, {7000.0, 0.60}};
}

const char* diffKindName(DiffKind k) {
    switch (k) {
        case DiffKind::Locked: return "locked";
        case DiffKind::Lsd: return "lsd";
        case DiffKind::Open: break;
    }
    return "open";
}

DiffKind diffKindFromName(const std::string& s, DiffKind fallback) {
    if (s == "open") return DiffKind::Open;
    if (s == "locked" || s == "lock") return DiffKind::Locked;
    if (s == "lsd" || s == "limited") return DiffKind::Lsd;
    return fallback;
}

VehicleDesc defaultVehicleDesc() {
    VehicleDesc d;
    AxleDesc front;
    front.z = 1.3;
    front.steerDeg = 32.0;
    front.driven = false;
    front.brakeBias = 1.0;
    AxleDesc rear;
    rear.z = -1.3;
    rear.driven = true;
    rear.brakeBias = 0.65;
    rear.handbrake = 1.0;
    d.axles = {front, rear};
    return d;
}

VehicleDesc clampVehicle(VehicleDesc d) {
    auto cl = [](double& v, double lo, double hi) { v = clampd(v, lo, hi); };
    cl(d.engine.maxTorque, 1.0, 100000.0);
    cl(d.engine.idleRpm, 100.0, 5000.0);
    cl(d.engine.maxRpm, d.engine.idleRpm + 100.0, 30000.0);
    cl(d.engine.inertia, 0.01, 100.0);
    cl(d.engine.friction, 0.0, 10.0);
    cl(d.engine.frictionConst, 0.0, 1000.0);
    cl(d.clutch.maxTorque, 1.0, 100000.0);
    cl(d.clutch.engageRpm, 0.0, 20000.0);
    cl(d.clutch.fullRpm, d.clutch.engageRpm + 1.0, 30000.0);
    if (d.gearbox.gears.empty()) d.gearbox.gears = {1.0};
    if (d.gearbox.gears.size() > 12) d.gearbox.gears.resize(12);
    for (double& g : d.gearbox.gears) g = clampd(g, 0.05, 50.0);
    cl(d.gearbox.reverse, 0.05, 50.0);
    cl(d.gearbox.finalDrive, 0.05, 50.0);
    cl(d.gearbox.efficiency, 0.1, 1.0);
    cl(d.gearbox.shiftUpRpm, 100.0, 30000.0);
    cl(d.gearbox.shiftDownRpm, 0.0, d.gearbox.shiftUpRpm);
    cl(d.gearbox.shiftTime, 0.0, 5.0);
    auto clampDiff = [&](DifferentialDesc& x) {
        cl(x.stiffness, 0.0, 1e6);
        cl(x.lockTorque, 0.0, 1e6);
        cl(x.bias, 0.0, 1.0);
    };
    clampDiff(d.center);
    auto clampTire = [&](TireDesc& t) {
        cl(t.radius, 0.05, 5.0);
        cl(t.width, 0.01, 5.0);
        cl(t.inertia, 0.001, 1000.0);
        cl(t.mu, 0.0, 5.0);
        cl(t.bx, 0.1, 100.0);
        cl(t.cx, 0.5, 3.0);
        cl(t.ex, -10.0, 1.0);
        cl(t.by, 0.1, 100.0);
        cl(t.cy, 0.5, 3.0);
        cl(t.ey, -10.0, 1.0);
        cl(t.relaxation, 0.0, 5.0);
        cl(t.rollingResistance, 0.0, 1.0);
    };
    auto clampSus = [&](SuspensionDesc& s) {
        cl(s.restLength, 0.01, 5.0);
        cl(s.travel, 0.0, s.restLength);
        cl(s.stiffness, 1.0, 1e7);
        cl(s.bump, 0.0, 1e6);
        cl(s.rebound, 0.0, 1e6);
        cl(s.antiRoll, 0.0, 1e7);
    };
    clampTire(d.tireDefaults);
    clampSus(d.suspensionDefaults);
    if (d.axles.size() > 8) d.axles.resize(8);
    for (AxleDesc& a : d.axles) {
        cl(a.z, -50.0, 50.0);
        cl(a.y, -10.0, 10.0);
        cl(a.halfTrack, 0.05, 25.0);
        cl(a.steerDeg, 0.0, 80.0);
        cl(a.ackermann, 0.0, 1.0);
        cl(a.brakeBias, 0.0, 10.0);
        cl(a.handbrake, 0.0, 10.0);
        clampDiff(a.diff);
        clampTire(a.tire);
        clampSus(a.suspension);
    }
    cl(d.brakeTorque, 0.0, 1e6);
    cl(d.dragArea, 0.0, 100.0);
    cl(d.steerRate, 0.1, 100.0);
    cl(d.steerSpeedRef, 0.0, 1000.0);
    cl(d.tractionControl, 0.0, 10.0);
    cl(d.pedalRate, 0.1, 100.0);
    d.ticks = d.ticks < 1 ? 1 : (d.ticks > 64 ? 64 : d.ticks);
    return d;
}

}  // namespace vehicle
}  // namespace wizengine
