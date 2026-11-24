#include "control/ControlSignals.h"

#include "transmission/AtSignals.h"

#include <algorithm>
#include <utility>

namespace wizengine {
namespace control {

namespace {

struct Entry {
    const char* name;
    Quantity q;
};

const std::vector<Entry>& bodySensors() {
    static const std::vector<Entry> t = {
        {"x", Quantity::X},     {"y", Quantity::Y},         {"z", Quantity::Z},
        {"vx", Quantity::Vx},   {"vy", Quantity::Vy},       {"vz", Quantity::Vz},
        {"wx", Quantity::Wx},   {"wy", Quantity::Wy},       {"wz", Quantity::Wz},
        {"rx", Quantity::Rx},   {"ry", Quantity::Ry},       {"rz", Quantity::Rz},
        {"speed", Quantity::Speed}, {"vfwd", Quantity::VFwd}, {"vlat", Quantity::VLat},
    };
    return t;
}
const std::vector<Entry>& jointSensors() {
    static const std::vector<Entry> t = {
        {"angle", Quantity::Angle}, {"rate", Quantity::Rate},   {"pos", Quantity::Pos},
        {"vel", Quantity::Vel},     {"force", Quantity::Force}, {"torque", Quantity::Torque},
    };
    return t;
}
const std::vector<Entry>& vehicleSensors() {
    static const std::vector<Entry> t = {
        {"speed", Quantity::VehSpeed}, {"rpm", Quantity::Rpm}, {"gear", Quantity::Gear},
    };
    return t;
}
const std::vector<Entry>& bodyActuators() {
    static const std::vector<Entry> t = {
        {"fx", Quantity::Fx},   {"fy", Quantity::Fy},   {"fz", Quantity::Fz},
        {"tx", Quantity::Tx},   {"ty", Quantity::Ty},   {"tz", Quantity::Tz},
        {"lfx", Quantity::LFx}, {"lfy", Quantity::LFy}, {"lfz", Quantity::LFz},
        {"ltx", Quantity::LTx}, {"lty", Quantity::LTy}, {"ltz", Quantity::LTz},
    };
    return t;
}
const std::vector<Entry>& jointActuators() {
    static const std::vector<Entry> t = {{"target", Quantity::Target}};
    return t;
}
const std::vector<Entry>& emitterActuators() {
    static const std::vector<Entry> t = {{"rate", Quantity::EmitRate}};
    return t;
}
const std::vector<Entry>& vehicleActuators() {
    static const std::vector<Entry> t = {
        {"throttle", Quantity::Throttle}, {"brake", Quantity::Brake},
        {"steer", Quantity::Steer},       {"handbrake", Quantity::Handbrake},
    };
    return t;
}

// air: の量（読める量。書ける量は local_pressure を除いたもの）。
const std::vector<std::string>& airSensorNames() {
    static const std::vector<std::string> t = {
        "density", "temperature", "humidity", "pressure", "local_pressure", "altitude",
        "wind_x",  "wind_y",      "wind_z",   "drag",     "lift",           "magnus",
    };
    return t;
}
const std::vector<std::string>& airActuatorNames() {
    static const std::vector<std::string> t = [] {
        std::vector<std::string> v;
        for (const auto& n : airSensorNames()) {
            if (n != "local_pressure") v.push_back(n);
        }
        return v;
    }();
    return t;
}

// "kind:target.qty" を 3 つに分ける。kind だけ（"time"）は qty も target も空。
bool split(const std::string& name, std::string& kind, std::string& target,
           std::string& qty) {
    const std::size_t colon = name.find(':');
    if (colon == std::string::npos) {
        kind = name;
        target.clear();
        qty.clear();
        return true;
    }
    kind = name.substr(0, colon);
    const std::string rest = name.substr(colon + 1);
    const std::size_t dot = rest.rfind('.');
    if (dot == std::string::npos) {
        target = rest;
        qty.clear();
        return true;
    }
    target = rest.substr(0, dot);
    qty = rest.substr(dot + 1);
    return true;
}

bool lookup(const std::vector<Entry>& table, const std::string& qty, Quantity& out) {
    for (const Entry& e : table) {
        if (qty == e.name) {
            out = e.q;
            return true;
        }
    }
    return false;
}

std::string listOf(const std::vector<Entry>& table) {
    std::string s;
    for (const Entry& e : table) s += (s.empty() ? "" : " ") + std::string(e.name);
    return s;
}

bool parseWith(const std::string& name, bool sensor, SignalRef& out, std::string& error) {
    std::string kind, target, qty;
    split(name, kind, target, qty);
    if (sensor && kind == "time" && target.empty()) {
        out = SignalRef{Source::Time, "", Quantity::None, ""};
        return true;
    }
    if (kind == "param" || kind == "var") {
        // パラメータ / 変数の名前は . を含まない（英数字と _ -）ので、そのまま使う。
        // どちらもセンサ（読む）とアクチュエータ（書く）の両方になれる。
        const std::string pname = name.substr(name.find(':') + 1);
        if (pname.empty()) {
            error = "\"" + name + "\": " + kind + " needs a name (" + kind + ":<name>)";
            return false;
        }
        out = SignalRef{kind == "param" ? Source::Param : Source::Var, pname, Quantity::None, ""};
        return true;
    }
    if (!sensor && kind == "scope") {
        // ラベルは . を含んでもよいので、split せずにそのまま使う。
        const std::string label = name.substr(name.find(':') + 1);
        if (label.empty()) {
            error = "\"" + name + "\": scope needs a label (scope:<label>)";
            return false;
        }
        out = SignalRef{Source::Scope, label, Quantity::None, ""};
        return true;
    }
    if (kind == "air") {
        // シーンに 1 つなので対象の名前は無い: air:<量>。
        const std::string qname = name.substr(name.find(':') + 1);
        const auto& list = sensor ? airSensorNames() : airActuatorNames();
        if (std::find(list.begin(), list.end(), qname) == list.end()) {
            std::string all;
            for (const auto& n : list) all += (all.empty() ? "" : " ") + n;
            error = "\"" + name + "\": unknown air " + (sensor ? "sensor" : "actuator") +
                    " quantity (air:<quantity> - " + all + ")";
            return false;
        }
        out = SignalRef{Source::Air, "", Quantity::Air, qname};
        return true;
    }
    if (kind == "trans") {
        // 量の名前は AT の中身（要素・軸の名前）で決まるので、ここでは形だけ見る。
        // 本当にあるかは制御器を組むとき（ControlComponent が transmission/AtSignals で引く）。
        if (target.empty() || qty.empty()) {
            error = "\"" + name + "\": needs trans:<name>.<quantity>";
            return false;
        }
        out = SignalRef{Source::Transmission, target, Quantity::Trans, qty};
        return true;
    }
    const std::vector<Entry>* table = nullptr;
    Source src = Source::Body;
    if (kind == "body") {
        src = Source::Body;
        table = sensor ? &bodySensors() : &bodyActuators();
    } else if (kind == "joint") {
        src = Source::Joint;
        table = sensor ? &jointSensors() : &jointActuators();
    } else if (kind == "vehicle") {
        src = Source::Vehicle;
        table = sensor ? &vehicleSensors() : &vehicleActuators();
    } else if (kind == "emitter" && !sensor) {
        src = Source::Emitter;
        table = &emitterActuators();
    } else {
        error = "\"" + name + "\": unknown signal kind \"" + kind + "\" (" +
                (sensor ? "time, body:, joint:, vehicle:, trans:, air:, param:, var:"
                        : "body:, joint:, vehicle:, trans:, air:, emitter:, param:, var:, scope:") +
                ")";
        return false;
    }
    if (target.empty()) {
        error = "\"" + name + "\": needs a name (" + kind + ":<name>.<quantity>)";
        return false;
    }
    Quantity q = Quantity::None;
    if (!lookup(*table, qty, q)) {
        error = "\"" + name + "\": unknown " + (sensor ? "sensor" : "actuator") +
                " quantity \"" + qty + "\" for " + kind + " (" + listOf(*table) + ")";
        return false;
    }
    out = SignalRef{src, target, q, ""};
    return true;
}

std::vector<std::string> names(const std::vector<Entry>& table) {
    std::vector<std::string> v;
    for (const Entry& e : table) v.emplace_back(e.name);
    return v;
}

}  // namespace

bool parseSensor(const std::string& name, SignalRef& out, std::string& error) {
    return parseWith(name, true, out, error);
}

bool parseActuator(const std::string& name, SignalRef& out, std::string& error) {
    return parseWith(name, false, out, error);
}

const std::vector<std::string>& sensorQuantities(Source source) {
    // AT は要素・軸ごとの量を除いた共通の名前（唯一の定義は transmission/AtSignals）。
    if (source == Source::Transmission) return wizengine::transmission::atCommonSensorNames();
    if (source == Source::Air) return airSensorNames();
    static const std::vector<std::string> body = names(bodySensors());
    static const std::vector<std::string> joint = names(jointSensors());
    static const std::vector<std::string> vehicle = names(vehicleSensors());
    static const std::vector<std::string> none;
    switch (source) {
        case Source::Body: return body;
        case Source::Joint: return joint;
        case Source::Vehicle: return vehicle;
        default: return none;
    }
}

const std::vector<std::string>& actuatorQuantities(Source source) {
    if (source == Source::Transmission) return wizengine::transmission::atInputNames();
    if (source == Source::Air) return airActuatorNames();
    static const std::vector<std::string> body = names(bodyActuators());
    static const std::vector<std::string> joint = names(jointActuators());
    static const std::vector<std::string> vehicle = names(vehicleActuators());
    static const std::vector<std::string> emitter = names(emitterActuators());
    static const std::vector<std::string> none;
    switch (source) {
        case Source::Body: return body;
        case Source::Joint: return joint;
        case Source::Vehicle: return vehicle;
        case Source::Emitter: return emitter;
        default: return none;
    }
}

}  // namespace control
}  // namespace wizengine
