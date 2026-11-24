#include "transmission/AtXml.h"

#include <cmath>
#include <cstdlib>
#include <sstream>

#include "transmission/AtKinematics.h"

namespace wizengine {
namespace transmission {

namespace {

constexpr const char* kPrefix = "<transmission>: ";

std::string joinNames(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}
std::vector<std::string> splitNames(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(tok);
    return out;
}
std::vector<double> numberList(const xml::Element& e, const char* key, const std::vector<double>& def,
                               xml::Warn& warn) {
    if (!e.has(key)) return def;
    std::vector<double> out;
    std::istringstream in(e.attr(key));
    std::string tok;
    while (in >> tok) {
        char* end = nullptr;
        const double v = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str() || *end != '\0' || !std::isfinite(v)) {
            warn(std::string("<") + e.name() + " " + key + ">: \"" + tok + "\" is not a number - "
                 "using the default list");
            return def;
        }
        out.push_back(v);
    }
    return out;
}

}  // namespace

xml::Element transmissionElement(const AtDesc& d) {
    xml::Element root("transmission");
    root.set("name", d.name);
    root.set("input", inputModeName(d.input));
    root.set("load", loadModeName(d.load));
    root.setNumber("rating", d.rating);
    root.set("inputShaft", d.inputShaft);
    root.set("outputShaft", d.outputShaft);
    {
        xml::Element e("engine");
        e.setNumber("inertia", d.engine.inertia);
        e.setNumber("idle", d.engine.idle);
        e.setNumber("idleGain", d.engine.idleGain);
        e.setNumber("redline", d.engine.redline);
        if (!d.engine.body.empty()) {
            e.set("body", d.engine.body);
            e.setNumbers("axis", d.engine.axis, 3, 6);
        }
        root.append(std::move(e));
    }
    {
        xml::Element e("converter");
        e.setNumbers("sr", d.converter.sr.data(), d.converter.sr.size(), 6);
        e.setNumbers("k", d.converter.k.data(), d.converter.k.size(), 6);
        e.setNumbers("tr", d.converter.tr.data(), d.converter.tr.size(), 6);
        e.setNumber("pumpInertia", d.converter.pumpInertia);
        e.setNumber("turbineInertia", d.converter.turbineInertia);
        root.append(std::move(e));
    }
    {
        xml::Element e("lockup");
        e.setNumber("capacity", d.lockup.capacity);
        e.setNumber("tau", d.lockup.tau);
        e.setInt("fromGear", d.lockup.fromGear);
        e.setNumber("speed", d.lockup.speed);
        e.setNumber("stiffness", d.lockup.stiffness);
        e.setNumber("damping", d.lockup.damping);
        e.setNumber("inertia", d.lockup.inertia);
        root.append(std::move(e));
    }
    {
        xml::Element e("pump");
        e.setNumber("displacement", d.pump.displacement);
        e.setNumber("pressureMin", d.pump.pressureMin);
        e.setNumber("pressureMax", d.pump.pressureMax);
        e.setNumber("efficiency", d.pump.efficiency);
        root.append(std::move(e));
    }
    {
        const auto& s = d.shift;
        xml::Element e("shift");
        e.setNumber("upLight", s.upLight);
        e.setNumber("upFull", s.upFull);
        e.setNumber("downLight", s.downLight);
        e.setNumber("downFull", s.downFull);
        e.setNumber("kickdown", s.kickdown);
        e.setNumber("minTime", s.minTime);
        e.setNumber("torquePhase", s.torquePhase);
        e.setNumber("inertiaPhase", s.inertiaPhase);
        e.setNumber("reduction", s.reduction);
        e.setNumber("kiss", s.kiss);
        e.setNumber("margin", s.margin);
        e.setNumber("kp", s.kp);
        e.setNumber("ki", s.ki);
        e.setNumber("engage", s.engage);
        root.append(std::move(e));
    }
    {
        const auto& o = d.output;
        xml::Element e("output");
        e.setNumber("inertia", o.inertia);
        e.setNumber("finalDrive", o.finalDrive);
        e.setNumber("finalEfficiency", o.finalEfficiency);
        e.setNumber("stiffness", o.stiffness);
        e.setNumber("damping", o.damping);
        e.setNumber("wheelRadius", o.wheelRadius);
        e.setNumber("mass", o.mass);
        e.setNumber("wheelInertia", o.wheelInertia);
        e.setNumber("crr", o.crr);
        e.setNumber("cda", o.cda);
        e.setNumber("rho", o.rho);
        e.setNumber("brake", o.brake);
        if (o.dynoRpm != 0.0) e.setNumber("dynoRpm", o.dynoRpm);
        root.append(std::move(e));
    }
    for (const auto& s : d.shafts) {
        xml::Element e("shaft");
        e.set("name", s.name);
        e.setNumber("inertia", s.inertia);
        e.setNumber("drag", s.drag);
        root.append(std::move(e));
    }
    for (const auto& g : d.gearsets) {
        xml::Element e("gearset");
        e.set("name", g.name);
        e.set("sun", g.sun);
        e.set("carrier", g.carrier);
        e.set("ring", g.ring);
        e.setInt("zs", g.zs);
        e.setInt("zr", g.zr);
        e.setInt("zp", g.zp);
        e.setInt("planets", g.planets);
        e.setNumber("efficiency", g.efficiency);
        root.append(std::move(e));
    }
    for (const auto& el : d.elements) {
        xml::Element e("element");
        e.set("name", el.name);
        e.set("type", elementKindName(el.kind));
        e.set("a", el.a);
        if (el.kind == ElementKind::Clutch) e.set("b", el.b);
        e.setNumber("capacity", el.capacity);
        e.setNumber("fill", el.fill);
        e.setNumber("tau", el.tau);
        e.setNumber("dynamic", el.dynamic);
        e.setNumber("drag", el.drag);
        root.append(std::move(e));
    }
    for (const auto& g : d.gears) {
        xml::Element e("gear");
        e.set("name", g.name);
        e.set("elements", joinNames(g.elements));
        root.append(std::move(e));
    }
    {
        xml::Element e("neutral");
        e.set("elements", joinNames(d.neutral));
        root.append(std::move(e));
    }
    return root;
}

AtDesc transmissionFromXml(const xml::Element& root, xml::Warn& parent) {
    xml::Warn warn(parent, std::string(kPrefix));
    xml::warnUnknownChildren(root,
                             {"engine", "converter", "lockup", "pump", "shift", "output", "shaft",
                              "gearset", "element", "gear", "neutral"},
                             "<transmission>", warn);
    const std::string preset = root.attr("preset");
    AtDesc d;
    if (preset == "jr913e") {
        d = jr913eDesc();
    } else if (!preset.empty()) {
        warn("preset \"" + preset + "\" is unknown (jr913e) - starting from the defaults");
    }
    d.name = root.attr("name", d.name.c_str());
    const std::string im = root.attr("input", inputModeName(d.input));
    if (xml::unknownName(inputModeFromName, im, InputMode::Torque, InputMode::Speed)) {
        warn("input=\"" + im + "\" is unknown (torque, speed, body) - using torque");
    }
    d.input = inputModeFromName(im, InputMode::Torque);
    const std::string lm = root.attr("load", loadModeName(d.load));
    if (xml::unknownName(loadModeFromName, lm, LoadMode::Vehicle, LoadMode::Dyno)) {
        warn("load=\"" + lm + "\" is unknown (vehicle, dyno) - using vehicle");
    }
    d.load = loadModeFromName(lm, LoadMode::Vehicle);
    d.rating = root.number("rating", d.rating);
    d.inputShaft = root.attr("inputShaft", d.inputShaft.c_str());
    d.outputShaft = root.attr("outputShaft", d.outputShaft.c_str());

    if (const xml::Element* e = root.first("engine")) {
        d.engine.inertia = e->number("inertia", d.engine.inertia);
        d.engine.idle = e->number("idle", d.engine.idle);
        d.engine.idleGain = e->number("idleGain", d.engine.idleGain);
        d.engine.redline = e->number("redline", d.engine.redline);
        d.engine.body = e->attr("body", d.engine.body.c_str());
        e->numbers("axis", d.engine.axis, 3);
    }
    if (const xml::Element* e = root.first("converter")) {
        auto& c = d.converter;
        c.sr = numberList(*e, "sr", c.sr, warn);
        c.k = numberList(*e, "k", c.k, warn);
        c.tr = numberList(*e, "tr", c.tr, warn);
        c.pumpInertia = e->number("pumpInertia", c.pumpInertia);
        c.turbineInertia = e->number("turbineInertia", c.turbineInertia);
    }
    if (const xml::Element* e = root.first("lockup")) {
        auto& l = d.lockup;
        l.capacity = e->number("capacity", l.capacity);
        l.tau = e->number("tau", l.tau);
        l.fromGear = e->integer("fromGear", l.fromGear);
        l.speed = e->number("speed", l.speed);
        l.stiffness = e->number("stiffness", l.stiffness);
        l.damping = e->number("damping", l.damping);
        l.inertia = e->number("inertia", l.inertia);
    }
    if (const xml::Element* e = root.first("pump")) {
        auto& p = d.pump;
        p.displacement = e->number("displacement", p.displacement);
        p.pressureMin = e->number("pressureMin", p.pressureMin);
        p.pressureMax = e->number("pressureMax", p.pressureMax);
        p.efficiency = e->number("efficiency", p.efficiency);
    }
    if (const xml::Element* e = root.first("shift")) {
        auto& s = d.shift;
        s.upLight = e->number("upLight", s.upLight);
        s.upFull = e->number("upFull", s.upFull);
        s.downLight = e->number("downLight", s.downLight);
        s.downFull = e->number("downFull", s.downFull);
        s.kickdown = e->number("kickdown", s.kickdown);
        s.minTime = e->number("minTime", s.minTime);
        s.torquePhase = e->number("torquePhase", s.torquePhase);
        s.inertiaPhase = e->number("inertiaPhase", s.inertiaPhase);
        s.reduction = e->number("reduction", s.reduction);
        s.kiss = e->number("kiss", s.kiss);
        s.margin = e->number("margin", s.margin);
        s.kp = e->number("kp", s.kp);
        s.ki = e->number("ki", s.ki);
        s.engage = e->number("engage", s.engage);
    }
    if (const xml::Element* e = root.first("output")) {
        auto& o = d.output;
        o.inertia = e->number("inertia", o.inertia);
        o.finalDrive = e->number("finalDrive", o.finalDrive);
        o.finalEfficiency = e->number("finalEfficiency", o.finalEfficiency);
        o.stiffness = e->number("stiffness", o.stiffness);
        o.damping = e->number("damping", o.damping);
        o.wheelRadius = e->number("wheelRadius", o.wheelRadius);
        o.mass = e->number("mass", o.mass);
        o.wheelInertia = e->number("wheelInertia", o.wheelInertia);
        o.crr = e->number("crr", o.crr);
        o.cda = e->number("cda", o.cda);
        o.rho = e->number("rho", o.rho);
        o.brake = e->number("brake", o.brake);
        o.dynoRpm = e->number("dynoRpm", o.dynoRpm);
    }
    // 歯車列: 1 つでも書いてあれば、プリセットの歯車列を丸ごと置き換える。
    const bool train = root.first("shaft") || root.first("gearset") || root.first("element") ||
                       root.first("gear");
    if (train) {
        d.shafts.clear();
        d.gearsets.clear();
        d.elements.clear();
        d.gears.clear();
        d.neutral.clear();
        for (const xml::Element* e : root.all("shaft")) {
            ShaftDesc s;
            s.name = e->attr("name");
            s.inertia = e->number("inertia", s.inertia);
            s.drag = e->number("drag", s.drag);
            d.shafts.push_back(s);
        }
        for (const xml::Element* e : root.all("gearset")) {
            GearsetDesc g;
            g.name = e->attr("name");
            g.sun = e->attr("sun");
            g.carrier = e->attr("carrier");
            g.ring = e->attr("ring");
            g.zs = e->integer("zs", g.zs);
            g.zr = e->integer("zr", g.zr);
            g.zp = e->integer("zp", 0);
            g.planets = e->integer("planets", g.planets);
            g.efficiency = e->number("efficiency", g.efficiency);
            d.gearsets.push_back(g);
        }
        for (const xml::Element* e : root.all("element")) {
            ElementDesc el;
            el.name = e->attr("name");
            const std::string t = e->attr("type", "clutch");
            if (xml::unknownName(elementKindFromName, t, ElementKind::Clutch, ElementKind::Brake)) {
                warn("<element name=\"" + el.name + "\" type=\"" + t +
                     "\">: unknown type (clutch, brake) - using clutch");
            }
            el.kind = elementKindFromName(t, ElementKind::Clutch);
            el.a = e->attr("a");
            el.b = e->attr("b");
            if (el.kind == ElementKind::Brake && !el.b.empty()) {
                warn("<element name=\"" + el.name + "\">: a brake holds a to the case - b ignored");
                el.b.clear();
            }
            el.capacity = e->number("capacity", el.capacity);
            el.fill = e->number("fill", el.fill);
            el.tau = e->number("tau", el.tau);
            el.dynamic = e->number("dynamic", el.dynamic);
            el.drag = e->number("drag", el.drag);
            d.elements.push_back(el);
        }
        for (const xml::Element* e : root.all("gear")) {
            GearDesc g;
            g.name = e->attr("name");
            g.elements = splitNames(e->attr("elements"));
            d.gears.push_back(g);
        }
    }
    if (const xml::Element* e = root.first("neutral")) d.neutral = splitNames(e->attr("elements"));

    std::vector<std::string> notes;
    d = clampAt(d, &notes);
    for (auto& n : notes) warn(n);
    // つなぎ先・段の拘束を、構造を解いて確かめる（純粋な計算なので軽い）。
    const AtKinematics k(d);
    for (const auto& e : k.errors()) warn(e);
    for (const auto& w : k.warnings()) warn(w);
    return d;
}

AtDesc transmissionFromXml(const xml::Element& e, std::vector<std::string>* warnings) {
    xml::Warn warn(warnings);
    return transmissionFromXml(e, warn);
}

}  // namespace transmission
}  // namespace wizengine
