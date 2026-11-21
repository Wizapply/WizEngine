#include "transmission/AtTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace wizengine {
namespace transmission {

AtDesc jr913eDesc() {
    AtDesc d;
    d.name = "jr913e";
    d.rating = 700.0;
    // 軸（8 本）。input = タービン軸 = 太陽 1 + キャリヤ 4、output = キャリヤ 3 + 内歯車 4。
    // 慣性は 9 速 AT の部品の大きさからの見積もり（ドラム・ハブ・歯車を含む）。
    d.shafts = {
        {"input", 0.012, 0.0008},  // S1 + C4 + クラッチ E のドラム
        {"c1", 0.010, 0.0004},     // キャリヤ 1（クラッチ D / E のハブ、ブレーキ C）
        {"n1", 0.014, 0.0004},     // 内歯車 1 + キャリヤ 2
        {"s2", 0.004, 0.0002},     // 太陽 2（ブレーキ A）
        {"n2", 0.012, 0.0004},     // 内歯車 2 + 太陽 3（クラッチ D / F）
        {"output", 0.020, 0.0008}, // キャリヤ 3 + 内歯車 4 + 出力軸
        {"r3", 0.012, 0.0002},     // 内歯車 3（ブレーキ B）
        {"s4", 0.004, 0.0002},     // 太陽 4（クラッチ F）
    };
    // 歯数: 公表のギア比（5.425 / 3.263 / 2.250 / 1.649 / 1.221 / 1.000 / 0.862 /
    // 0.713 / 0.597、R 4.799）を小数 3 桁で再現し、遊星を等配できる組。
    d.gearsets = {
        {"P1", "input", "c1", "n1", 60, 128, 34, 4, 0.985},
        {"P2", "s2", "n1", "n2", 41, 91, 25, 4, 0.985},
        {"P3", "n2", "output", "r3", 38, 86, 24, 4, 0.985},
        {"P4", "s4", "input", "output", 39, 97, 29, 4, 0.985},
    };
    // 締結要素（容量 0 = 段ごとの分担 × 定格 × 1.4 で自動）。
    auto el = [](const char* n, ElementKind k, const char* a, const char* b) {
        ElementDesc e;
        e.name = n;
        e.kind = k;
        e.a = a;
        e.b = b;
        return e;
    };
    d.elements = {
        el("A", ElementKind::Brake, "s2", ""),
        el("B", ElementKind::Brake, "r3", ""),
        el("C", ElementKind::Brake, "c1", ""),
        el("D", ElementKind::Clutch, "c1", "n2"),
        el("E", ElementKind::Clutch, "input", "c1"),
        el("F", ElementKind::Clutch, "s4", "n2"),
    };
    d.gears = {
        {"1", {"A", "B", "D"}}, {"2", {"B", "D", "E"}}, {"3", {"A", "B", "E"}},
        {"4", {"B", "E", "F"}}, {"5", {"A", "E", "F"}}, {"6", {"D", "E", "F"}},
        {"7", {"A", "D", "F"}}, {"8", {"C", "D", "F"}}, {"9", {"A", "C", "F"}},
        {"R", {"A", "B", "C"}},
    };
    d.neutral = {"A", "B"};
    return d;
}

const char* inputModeName(InputMode m) {
    return m == InputMode::Speed ? "speed" : m == InputMode::Body ? "body" : "torque";
}
const char* loadModeName(LoadMode m) { return m == LoadMode::Dyno ? "dyno" : "vehicle"; }
const char* elementKindName(ElementKind k) { return k == ElementKind::Brake ? "brake" : "clutch"; }
InputMode inputModeFromName(const std::string& s, InputMode fallback) {
    if (s == "torque") return InputMode::Torque;
    if (s == "speed") return InputMode::Speed;
    if (s == "body") return InputMode::Body;
    return fallback;
}
LoadMode loadModeFromName(const std::string& s, LoadMode fallback) {
    if (s == "vehicle") return LoadMode::Vehicle;
    if (s == "dyno") return LoadMode::Dyno;
    return fallback;
}
ElementKind elementKindFromName(const std::string& s, ElementKind fallback) {
    if (s == "clutch") return ElementKind::Clutch;
    if (s == "brake") return ElementKind::Brake;
    return fallback;
}

int gearNumber(const std::string& name) {
    if (name == "R") return -1;
    if (name.empty()) return 0;
    for (char c : name) {
        if (c < '0' || c > '9') return 0;
    }
    return std::atoi(name.c_str());
}

namespace {

struct Clamper {
    std::vector<std::string>* w;
    void num(double& v, double lo, double hi, const char* what) {
        if (!std::isfinite(v)) {
            if (w) w->push_back(std::string(what) + ": not a number - using " + std::to_string(lo));
            v = lo;
            return;
        }
        if (v < lo || v > hi) {
            const double c = std::min(hi, std::max(lo, v));
            if (w) {
                w->push_back(std::string(what) + ": " + std::to_string(v) + " is outside " +
                             std::to_string(lo) + ".." + std::to_string(hi) + " - clamped");
            }
            v = c;
        }
    }
};

}  // namespace

AtDesc clampAt(const AtDesc& in, std::vector<std::string>* warnings) {
    AtDesc d = in;
    Clamper c{warnings};
    c.num(d.rating, 1.0, 1e5, "rating");
    c.num(d.engine.inertia, 0.01, 50.0, "<engine inertia>");
    c.num(d.engine.idle, 0.0, 3000.0, "<engine idle>");
    c.num(d.engine.idleGain, 0.0, 100.0, "<engine idleGain>");
    c.num(d.engine.redline, 500.0, 30000.0, "<engine redline>");
    {
        double* a = d.engine.axis;
        const double len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (!(len > 1e-9) || !std::isfinite(len)) {
            if (warnings) warnings->push_back("<engine axis>: zero vector - using 0 0 1");
            a[0] = 0.0;
            a[1] = 0.0;
            a[2] = 1.0;
        } else {
            for (int i = 0; i < 3; ++i) a[i] /= len;
        }
    }
    if (d.input == InputMode::Body && d.engine.body.empty() && warnings) {
        warnings->push_back("input=\"body\" needs <engine body=\"<object name>\"> - the engine "
                            "shaft will not be connected");
    }
    c.num(d.converter.pumpInertia, 0.001, 10.0, "<converter pumpInertia>");
    c.num(d.converter.turbineInertia, 0.001, 10.0, "<converter turbineInertia>");
    // 表: 3 本とも同じ長さ、SR は 0 から増える並び。壊れていれば既定の表。
    {
        const ConverterDesc def;
        const auto& cv = d.converter;
        bool ok = cv.sr.size() >= 2 && cv.sr.size() == cv.k.size() && cv.sr.size() == cv.tr.size();
        for (std::size_t i = 1; ok && i < cv.sr.size(); ++i) ok = cv.sr[i] > cv.sr[i - 1];
        for (std::size_t i = 0; ok && i < cv.sr.size(); ++i) {
            ok = std::isfinite(cv.sr[i]) && std::isfinite(cv.k[i]) && std::isfinite(cv.tr[i]) &&
                 cv.k[i] >= 0.0 && cv.tr[i] >= 0.0;
        }
        if (!ok) {
            if (warnings) {
                warnings->push_back("<converter>: sr / k / tr must be the same length, sr "
                                    "increasing, k and tr >= 0 - using the default table");
            }
            d.converter.sr = def.sr;
            d.converter.k = def.k;
            d.converter.tr = def.tr;
        }
    }
    c.num(d.lockup.capacity, 0.0, 1e5, "<lockup capacity>");
    c.num(d.lockup.tau, 0.005, 10.0, "<lockup tau>");
    c.num(d.lockup.speed, 0.0, 400.0, "<lockup speed>");
    c.num(d.lockup.stiffness, 10.0, 1e7, "<lockup stiffness>");
    c.num(d.lockup.damping, 0.0, 1e5, "<lockup damping>");
    c.num(d.lockup.inertia, 0.001, 10.0, "<lockup inertia>");
    if (d.lockup.fromGear < 1) d.lockup.fromGear = 1;
    c.num(d.pump.displacement, 0.0, 200.0, "<pump displacement>");
    c.num(d.pump.pressureMin, 0.0, 100.0, "<pump pressureMin>");
    c.num(d.pump.pressureMax, d.pump.pressureMin, 100.0, "<pump pressureMax>");
    c.num(d.pump.efficiency, 0.1, 1.0, "<pump efficiency>");
    auto& s = d.shift;
    c.num(s.upLight, 300.0, 20000.0, "<shift upLight>");
    c.num(s.upFull, s.upLight, 20000.0, "<shift upFull>");
    c.num(s.downLight, 100.0, s.upLight, "<shift downLight>");
    c.num(s.downFull, s.downLight, s.upFull, "<shift downFull>");
    c.num(s.kickdown, 0.0, 1.0, "<shift kickdown>");
    c.num(s.minTime, 0.0, 10.0, "<shift minTime>");
    c.num(s.torquePhase, 0.01, 2.0, "<shift torquePhase>");
    c.num(s.inertiaPhase, 0.02, 3.0, "<shift inertiaPhase>");
    c.num(s.reduction, 0.0, 1.0, "<shift reduction>");
    c.num(s.kiss, 0.0, 0.5, "<shift kiss>");
    c.num(s.margin, 1.0, 5.0, "<shift margin>");
    c.num(s.kp, 0.0, 1e4, "<shift kp>");
    c.num(s.ki, 0.0, 1e5, "<shift ki>");
    c.num(s.engage, 0.05, 5.0, "<shift engage>");
    auto& o = d.output;
    c.num(o.inertia, 0.001, 100.0, "<output inertia>");
    c.num(o.finalDrive, 0.1, 50.0, "<output finalDrive>");
    c.num(o.finalEfficiency, 0.5, 1.0, "<output finalEfficiency>");
    c.num(o.stiffness, 10.0, 1e7, "<output stiffness>");
    c.num(o.damping, 0.0, 1e6, "<output damping>");
    c.num(o.wheelRadius, 0.05, 3.0, "<output wheelRadius>");
    c.num(o.mass, 1.0, 1e6, "<output mass>");
    c.num(o.wheelInertia, 0.0, 1e4, "<output wheelInertia>");
    c.num(o.crr, 0.0, 1.0, "<output crr>");
    c.num(o.cda, 0.0, 100.0, "<output cda>");
    c.num(o.rho, 0.0, 10.0, "<output rho>");
    c.num(o.brake, 0.0, 1e6, "<output brake>");
    c.num(o.dynoRpm, -30000.0, 30000.0, "<output dynoRpm>");
    for (auto& sh : d.shafts) {
        c.num(sh.inertia, 1e-5, 100.0, ("<shaft " + sh.name + "> inertia").c_str());
        c.num(sh.drag, 0.0, 100.0, ("<shaft " + sh.name + "> drag").c_str());
    }
    for (auto& g : d.gearsets) {
        if (g.zs < 1 || g.zr <= g.zs) {
            if (warnings) {
                warnings->push_back("<gearset " + g.name + ">: needs 0 < zs < zr - using 40 / 90");
            }
            g.zs = 40;
            g.zr = 90;
        }
        if (g.zp <= 0) g.zp = std::max(1, (g.zr - g.zs) / 2);
        if (g.planets < 1) g.planets = 1;
        c.num(g.efficiency, 0.5, 1.0, ("<gearset " + g.name + "> efficiency").c_str());
    }
    for (auto& e : d.elements) {
        const std::string w = "<element " + e.name + ">";
        c.num(e.capacity, 0.0, 1e6, (w + " capacity").c_str());
        c.num(e.fill, 0.0, 2.0, (w + " fill").c_str());
        c.num(e.tau, 0.002, 2.0, (w + " tau").c_str());
        c.num(e.dynamic, 0.2, 1.0, (w + " dynamic").c_str());
        c.num(e.drag, 0.0, 10.0, (w + " drag").c_str());
    }
    return d;
}

}  // namespace transmission
}  // namespace wizengine
