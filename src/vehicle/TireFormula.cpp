#include "vehicle/TireFormula.h"

namespace wizengine {
namespace vehicle {

const std::vector<std::string>& tireFormulaInputs() {
    static const std::vector<std::string> v = {
        "kappa", "alpha", "fz", "mu", "v",  "vx", "vy",   "omega",
        "radius", "bx",   "cx", "ex", "by", "cy", "ey",   "relax",
        "fxLo",  "fxHi", "fyLo", "fyHi"};
    return v;
}

const std::vector<std::string>& tireFormulaOutputs() {
    static const std::vector<std::string> v = {"fx", "fy"};
    return v;
}

FormulaGraphDesc defaultTireFormulaGraph(const std::string& name) {
    FormulaGraphDesc g;
    g.name = name;
    int next = 1;
    auto node = [&](const std::string& kind, const std::string& nm, double x, double y) {
        FormulaNodeDesc n;
        n.id = next++;
        n.kind = kind;
        n.name = nm;
        n.x = x;
        n.y = y;
        g.nodes.push_back(n);
        return n.id;
    };
    auto wire = [&](int from, int to, int port, int fromPort = 0) {
        g.wires.push_back({from, fromPort, to, port});
    };
    // 入力（左の列）。
    const int kappa = node("in", "kappa", 40, 40);
    const int alpha = node("in", "alpha", 40, 100);
    const int fz = node("in", "fz", 40, 160);
    const int mu = node("in", "mu", 40, 220);
    const int v = node("in", "v", 40, 280);
    const int bx = node("in", "bx", 40, 340);
    const int cx = node("in", "cx", 40, 400);
    const int ex = node("in", "ex", 40, 460);
    const int by = node("in", "by", 40, 520);
    const int cy = node("in", "cy", 40, 580);
    const int ey = node("in", "ey", 40, 640);
    const int relax = node("in", "relax", 40, 700);
    const int fxLo = node("in", "fxLo", 40, 760);
    const int fxHi = node("in", "fxHi", 40, 820);
    const int fyLo = node("in", "fyLo", 40, 880);
    const int fyHi = node("in", "fyHi", 40, 940);
    // peak = mu * fz
    const int peak = node("mul", "", 260, 190);
    wire(mu, peak, 0);
    wire(fz, peak, 1);
    // 縦: magic(kappa, bx, cx, ex) * peak
    const int mx = node("magic", "", 260, 40);
    wire(kappa, mx, 0);
    wire(bx, mx, 1);
    wire(cx, mx, 2);
    wire(ex, mx, 3);
    const int fx0 = node("mul", "", 460, 40);
    wire(peak, fx0, 0);
    wire(mx, fx0, 1);
    // 横: -magic(alpha, by, cy, ey) * peak
    const int my = node("magic", "", 260, 520);
    wire(alpha, my, 0);
    wire(by, my, 1);
    wire(cy, my, 2);
    wire(ey, my, 3);
    const int fy0 = node("mul", "", 460, 520);
    wire(peak, fy0, 0);
    wire(my, fy0, 1);
    const int fy1 = node("neg", "", 620, 520);
    wire(fy0, fy1, 0);
    // 摩擦楕円
    const int ell = node("ellipse", "", 780, 280);
    wire(fx0, ell, 0);
    wire(fy1, ell, 1);
    wire(peak, ell, 2);
    // 許される範囲（WheelController が渡す: 滑りの向きに逆らわず、この
    // ティックで滑り速度を反転させない）。緩和長の前に掛けるので、状態が
    // 範囲を超えて溜まらない。
    const int clampX = node("clamp", "", 960, 200);
    wire(ell, clampX, 0, 0);
    wire(fxLo, clampX, 1);
    wire(fxHi, clampX, 2);
    const int clampY = node("clamp", "", 960, 360);
    wire(ell, clampY, 0, 1);
    wire(fyLo, clampY, 1);
    wire(fyHi, clampY, 2);
    // 緩和長: tau = relax / v、一次遅れ
    const int tau = node("div", "", 620, 700);
    wire(relax, tau, 0);
    wire(v, tau, 1);
    const int lagX = node("lag", "", 1140, 200);
    wire(clampX, lagX, 0);
    wire(tau, lagX, 1);
    const int lagY = node("lag", "", 1140, 360);
    wire(clampY, lagY, 0);
    wire(tau, lagY, 1);
    const int outX = node("out", "fx", 1320, 200);
    wire(lagX, outX, 0);
    const int outY = node("out", "fy", 1320, 360);
    wire(lagY, outY, 0);
    return g;
}

}  // namespace vehicle
}  // namespace wizengine
