#include "vehicle/VehicleXml.h"

#include <cstddef>

#include "vehicle/FormulaXml.h"

namespace wizengine {
namespace vehicle {

namespace {

void warnTo(std::vector<std::string>* w, std::string msg) {
    if (w) w->push_back(std::move(msg));
}

void warnUnknown(const xml::Element& parent,
                 std::initializer_list<const char*> known, const char* where,
                 std::vector<std::string>* warnings) {
    for (const auto& c : parent.children()) {
        bool ok = false;
        for (const char* k : known) {
            if (c.name() == k) { ok = true; break; }
        }
        if (!ok) {
            warnTo(warnings, std::string("<vehicle>: unsupported <") + c.name() +
                                 "> in " + where + " - ignored");
        }
    }
}

// ---- タイヤ / サス（書き出しと読み込みは隣り合わせ）-------------------------
xml::Element tireElement(const TireDesc& t) {
    xml::Element e("tire");
    e.setNumber("radius", t.radius);
    e.setNumber("width", t.width);
    e.setNumber("inertia", t.inertia);
    e.setNumber("mu", t.mu);
    e.setNumber("bx", t.bx);
    e.setNumber("cx", t.cx);
    e.setNumber("ex", t.ex);
    e.setNumber("by", t.by);
    e.setNumber("cy", t.cy);
    e.setNumber("ey", t.ey);
    e.setNumber("relax", t.relaxation);
    e.setNumber("rolling", t.rollingResistance);
    if (!t.formula.empty()) e.set("formula", t.formula);
    return e;
}
TireDesc tireFromXml(const xml::Element& e, const TireDesc& base) {
    TireDesc t = base;
    t.radius = e.number("radius", t.radius);
    t.width = e.number("width", t.width);
    t.inertia = e.number("inertia", t.inertia);
    t.mu = e.number("mu", t.mu);
    t.bx = e.number("bx", t.bx);
    t.cx = e.number("cx", t.cx);
    t.ex = e.number("ex", t.ex);
    t.by = e.number("by", t.by);
    t.cy = e.number("cy", t.cy);
    t.ey = e.number("ey", t.ey);
    t.relaxation = e.number("relax", t.relaxation);
    t.rollingResistance = e.number("rolling", t.rollingResistance);
    if (e.has("formula")) t.formula = e.attr("formula");
    return t;
}

xml::Element suspensionElement(const SuspensionDesc& s) {
    xml::Element e("suspension");
    e.setNumber("rest", s.restLength);
    e.setNumber("travel", s.travel);
    e.setNumber("stiffness", s.stiffness);
    e.setNumber("bump", s.bump);
    e.setNumber("rebound", s.rebound);
    e.setNumber("antiroll", s.antiRoll);
    return e;
}
SuspensionDesc suspensionFromXml(const xml::Element& e, const SuspensionDesc& base) {
    SuspensionDesc s = base;
    s.restLength = e.number("rest", s.restLength);
    s.travel = e.number("travel", s.travel);
    s.stiffness = e.number("stiffness", s.stiffness);
    s.bump = e.number("bump", s.bump);
    s.rebound = e.number("rebound", s.rebound);
    s.antiRoll = e.number("antiroll", s.antiRoll);
    return s;
}

// ---- デフ ------------------------------------------------------------------
void setDiff(xml::Element& e, const char* kindKey, const DifferentialDesc& d,
             bool withBias) {
    e.set(kindKey, diffKindName(d.kind));
    if (withBias) e.setNumber("bias", d.bias);
    e.setNumber("stiffness", d.stiffness);
    e.setNumber("lock", d.lockTorque);
}
DifferentialDesc diffFromXml(const xml::Element& e, const char* kindKey,
                             const DifferentialDesc& base, bool withBias,
                             std::vector<std::string>* warnings) {
    DifferentialDesc d = base;
    if (e.has(kindKey)) {
        const std::string s = e.attr(kindKey);
        if (diffKindFromName(s, DiffKind::Open) != diffKindFromName(s, DiffKind::Locked)) {
            warnTo(warnings, std::string("<vehicle>: differential type \"") + s +
                                 "\" is unknown - reading as open");
        }
        d.kind = diffKindFromName(s, d.kind);
    }
    if (withBias) d.bias = e.number("bias", d.bias);
    d.stiffness = e.number("stiffness", d.stiffness);
    d.lockTorque = e.number("lock", d.lockTorque);
    return d;
}

}  // namespace

xml::Element vehicleElement(const VehicleDesc& d) {
    xml::Element v("vehicle");
    v.setInt("ticks", d.ticks);
    v.setNumber("brake", d.brakeTorque);
    v.setNumber("drag", d.dragArea);
    v.setNumber("steerRate", d.steerRate);
    v.setNumber("steerSpeed", d.steerSpeedRef);
    v.setNumber("tcs", d.tractionControl);
    v.setNumber("pedalRate", d.pedalRate);

    xml::Element eng("engine");
    eng.setNumber("torque", d.engine.maxTorque);
    eng.setNumber("idle", d.engine.idleRpm);
    eng.setNumber("max", d.engine.maxRpm);
    eng.setNumber("inertia", d.engine.inertia);
    eng.setNumber("friction", d.engine.friction);
    eng.setNumber("frictionConst", d.engine.frictionConst);
    if (!d.engine.torqueCurve.empty()) {
        std::vector<double> flat;
        for (const CurvePoint& p : d.engine.torqueCurve) {
            flat.push_back(p.x);
            flat.push_back(p.y);
        }
        eng.setNumbers("curve", flat.data(), flat.size());
    }
    v.append(std::move(eng));

    xml::Element cl("clutch");
    cl.setNumber("torque", d.clutch.maxTorque);
    cl.setNumber("engage", d.clutch.engageRpm);
    cl.setNumber("full", d.clutch.fullRpm);
    v.append(std::move(cl));

    xml::Element gb("gearbox");
    gb.setNumbers("ratios", d.gearbox.gears.data(), d.gearbox.gears.size());
    gb.setNumber("reverse", d.gearbox.reverse);
    gb.setNumber("final", d.gearbox.finalDrive);
    gb.setNumber("efficiency", d.gearbox.efficiency);
    gb.setNumber("up", d.gearbox.shiftUpRpm);
    gb.setNumber("down", d.gearbox.shiftDownRpm);
    gb.setNumber("time", d.gearbox.shiftTime);
    gb.setBool("automatic", d.gearbox.automatic);
    v.append(std::move(gb));

    xml::Element center("center");
    setDiff(center, "type", d.center, /*withBias*/ true);
    v.append(std::move(center));

    for (const FormulaGraphDesc& f : d.formulas) v.append(formulaElement(f));

    for (const AxleDesc& a : d.axles) {
        xml::Element ax("axle");
        ax.setNumber("z", a.z);
        ax.setNumber("y", a.y);
        ax.setNumber("track", a.halfTrack * 2.0);
        ax.setNumber("steer", a.steerDeg);
        ax.setNumber("ackermann", a.ackermann);
        ax.setBool("driven", a.driven);
        ax.setNumber("brake", a.brakeBias);
        ax.setNumber("handbrake", a.handbrake);
        setDiff(ax, "diff", a.diff, /*withBias*/ false);
        ax.append(tireElement(a.tire));
        ax.append(suspensionElement(a.suspension));
        v.append(std::move(ax));
    }
    return v;
}

VehicleDesc vehicleFromXml(const xml::Element& e,
                           std::vector<std::string>* warnings) {
    VehicleDesc d = defaultVehicleDesc();
    warnUnknown(e, {"engine", "clutch", "gearbox", "center", "tire", "suspension", "axle",
                    "formula"},
                "<vehicle>", warnings);

    for (const xml::Element* f : e.all("formula")) {
        FormulaGraphDesc g = formulaFromXml(*f, warnings);
        bool dup = false;
        for (const FormulaGraphDesc& prev : d.formulas) {
            if (prev.name == g.name) dup = true;
        }
        if (g.name.empty() || dup) {
            warnTo(warnings, "<vehicle>: <formula name=\"" + g.name +
                                 "\"> is empty or duplicated - ignored");
            continue;
        }
        d.formulas.push_back(std::move(g));
    }

    d.ticks = e.integer("ticks", d.ticks);
    d.brakeTorque = e.number("brake", d.brakeTorque);
    d.dragArea = e.number("drag", d.dragArea);
    d.steerRate = e.number("steerRate", d.steerRate);
    d.steerSpeedRef = e.number("steerSpeed", d.steerSpeedRef);
    d.tractionControl = e.number("tcs", d.tractionControl);
    d.pedalRate = e.number("pedalRate", d.pedalRate);

    if (const xml::Element* eng = e.first("engine")) {
        d.engine.maxTorque = eng->number("torque", d.engine.maxTorque);
        d.engine.idleRpm = eng->number("idle", d.engine.idleRpm);
        d.engine.maxRpm = eng->number("max", d.engine.maxRpm);
        d.engine.inertia = eng->number("inertia", d.engine.inertia);
        d.engine.friction = eng->number("friction", d.engine.friction);
        d.engine.frictionConst = eng->number("frictionConst", d.engine.frictionConst);
        if (eng->has("curve")) {
            double flat[64];
            const std::size_t n = eng->numbers("curve", flat, 64);
            std::vector<CurvePoint> curve;
            for (std::size_t i = 0; i + 1 < n; i += 2) {
                curve.push_back({flat[i], flat[i + 1]});
            }
            bool sorted = true;
            for (std::size_t i = 1; i < curve.size(); ++i) {
                if (curve[i].x < curve[i - 1].x) sorted = false;
            }
            if (curve.size() < 2 || !sorted) {
                warnTo(warnings, "<vehicle><engine curve>: needs \"rpm value\" pairs "
                                 "with rpm ascending - using the default curve");
            } else {
                d.engine.torqueCurve = std::move(curve);
            }
        }
    }
    if (const xml::Element* cl = e.first("clutch")) {
        d.clutch.maxTorque = cl->number("torque", d.clutch.maxTorque);
        d.clutch.engageRpm = cl->number("engage", d.clutch.engageRpm);
        d.clutch.fullRpm = cl->number("full", d.clutch.fullRpm);
    }
    if (const xml::Element* gb = e.first("gearbox")) {
        if (gb->has("ratios")) {
            double r[12];
            const std::size_t n = gb->numbers("ratios", r, 12);
            if (n == 0) {
                warnTo(warnings, "<vehicle><gearbox ratios>: no ratios - keeping the default");
            } else {
                d.gearbox.gears.assign(r, r + n);
            }
        }
        d.gearbox.reverse = gb->number("reverse", d.gearbox.reverse);
        d.gearbox.finalDrive = gb->number("final", d.gearbox.finalDrive);
        d.gearbox.efficiency = gb->number("efficiency", d.gearbox.efficiency);
        d.gearbox.shiftUpRpm = gb->number("up", d.gearbox.shiftUpRpm);
        d.gearbox.shiftDownRpm = gb->number("down", d.gearbox.shiftDownRpm);
        d.gearbox.shiftTime = gb->number("time", d.gearbox.shiftTime);
        d.gearbox.automatic = gb->boolean("automatic", d.gearbox.automatic);
    }
    if (const xml::Element* c = e.first("center")) {
        d.center = diffFromXml(*c, "type", d.center, /*withBias*/ true, warnings);
    }
    if (const xml::Element* t = e.first("tire")) {
        d.tireDefaults = tireFromXml(*t, d.tireDefaults);
    }
    if (const xml::Element* s = e.first("suspension")) {
        d.suspensionDefaults = suspensionFromXml(*s, d.suspensionDefaults);
    }

    const auto axles = e.all("axle");
    if (!axles.empty()) {
        d.axles.clear();
        for (const xml::Element* ax : axles) {
            warnUnknown(*ax, {"tire", "suspension"}, "<axle>", warnings);
            AxleDesc a;
            a.tire = d.tireDefaults;
            a.suspension = d.suspensionDefaults;
            a.z = ax->number("z", a.z);
            a.y = ax->number("y", a.y);
            a.halfTrack = ax->number("track", a.halfTrack * 2.0) * 0.5;
            a.steerDeg = ax->number("steer", 0.0);
            a.ackermann = ax->number("ackermann", a.ackermann);
            a.driven = ax->boolean("driven", false);
            a.brakeBias = ax->number("brake", a.brakeBias);
            a.handbrake = ax->number("handbrake", a.handbrake);
            a.diff = diffFromXml(*ax, "diff", a.diff, /*withBias*/ false, warnings);
            if (const xml::Element* t = ax->first("tire")) a.tire = tireFromXml(*t, a.tire);
            if (const xml::Element* s = ax->first("suspension")) {
                a.suspension = suspensionFromXml(*s, a.suspension);
            }
            d.axles.push_back(a);
        }
        bool anyDriven = false;
        for (const AxleDesc& a : d.axles) anyDriven = anyDriven || a.driven;
        if (!anyDriven) {
            warnTo(warnings, "<vehicle>: no axle has driven=\"true\" - the vehicle "
                             "cannot move under power");
        }
    }
    return clampVehicle(d);
}

}  // namespace vehicle
}  // namespace wizengine
