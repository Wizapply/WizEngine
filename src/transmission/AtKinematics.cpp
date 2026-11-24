#include "transmission/AtKinematics.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace wizengine {
namespace transmission {

namespace {

// 密行列の連立 1 次方程式（部分ピボット付きガウス消去）。M は n×n の行優先、
// 解は b に入る。ピボットが小さすぎれば false（拘束が足りない / 余る）。
bool solveDense(std::vector<double>& M, std::vector<double>& b, int n) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        double best = std::fabs(M[std::size_t(c * n + c)]);
        for (int r = c + 1; r < n; ++r) {
            const double v = std::fabs(M[std::size_t(r * n + c)]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-14) return false;
        if (piv != c) {
            for (int j = 0; j < n; ++j) std::swap(M[std::size_t(c * n + j)], M[std::size_t(piv * n + j)]);
            std::swap(b[std::size_t(c)], b[std::size_t(piv)]);
        }
        const double d = M[std::size_t(c * n + c)];
        for (int r = c + 1; r < n; ++r) {
            const double f = M[std::size_t(r * n + c)] / d;
            if (f == 0.0) continue;
            for (int j = c; j < n; ++j) M[std::size_t(r * n + j)] -= f * M[std::size_t(c * n + j)];
            b[std::size_t(r)] -= f * b[std::size_t(c)];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[std::size_t(r)];
        for (int j = r + 1; j < n; ++j) s -= M[std::size_t(r * n + j)] * b[std::size_t(j)];
        b[std::size_t(r)] = s / M[std::size_t(r * n + r)];
    }
    return true;
}

}  // namespace

AtKinematics::AtKinematics(const AtDesc& desc) {
    std::map<std::string, int> shaftOf;
    for (const auto& s : desc.shafts) {
        if (s.name.empty() || shaftOf.count(s.name)) {
            errors_.push_back("<shaft name=\"" + s.name + "\">: empty or duplicate name");
            continue;
        }
        shaftOf[s.name] = int(shaftCount_++);
    }
    auto shaft = [&](const std::string& name, const std::string& where) {
        const auto it = shaftOf.find(name);
        if (it == shaftOf.end()) {
            errors_.push_back(where + ": no <shaft name=\"" + name + "\">");
            return -1;
        }
        return it->second;
    };
    in_ = shaft(desc.inputShaft, "input shaft");
    out_ = shaft(desc.outputShaft, "output shaft");
    if (in_ >= 0 && in_ == out_) errors_.push_back("input and output are the same shaft");

    for (const auto& g : desc.gearsets) {
        const std::string w = "<gearset name=\"" + g.name + "\">";
        Gearset s;
        s.s = shaft(g.sun, w + " sun");
        s.c = shaft(g.carrier, w + " carrier");
        s.r = shaft(g.ring, w + " ring");
        s.k = double(g.zr) / double(g.zs);
        s.eff = g.efficiency;
        if (s.s >= 0 && (s.s == s.c || s.s == s.r || s.c == s.r)) {
            errors_.push_back(w + ": two members on the same shaft (the set would be locked)");
        }
        if ((g.zr - g.zs) % 2 != 0) {
            warnings_.push_back(w + ": zr - zs is odd - the planets cannot have whole teeth");
        }
        if ((g.zs + g.zr) % std::max(1, g.planets) != 0) {
            warnings_.push_back(w + ": (zs + zr) / planets is not a whole number - the planets "
                                    "cannot be spaced evenly");
        }
        sets_.push_back(s);
    }

    std::map<std::string, int> elemOf;
    for (const auto& e : desc.elements) {
        const std::string w = "<element name=\"" + e.name + "\">";
        Element el;
        el.a = shaft(e.a, w);
        if (e.kind == ElementKind::Clutch) {
            el.b = shaft(e.b, w);
            if (el.a >= 0 && el.a == el.b) errors_.push_back(w + ": a clutch needs two different shafts");
        }
        if (e.name.empty() || elemOf.count(e.name)) {
            errors_.push_back(w + ": empty or duplicate name");
        } else {
            elemOf[e.name] = int(elements_.size());
        }
        elements_.push_back(el);
    }

    for (const auto& gd : desc.gears) {
        Gear g;
        g.number = gearNumber(gd.name);
        for (const auto& n : gd.elements) {
            const auto it = elemOf.find(n);
            if (it == elemOf.end()) {
                errors_.push_back("<gear name=\"" + gd.name + "\">: no <element name=\"" + n + "\">");
                continue;
            }
            g.elements.push_back(it->second);
        }
        if (gd.name == "R") gearR_ = int(gears_.size());
        if (g.number == 0 && gd.name != "R") {
            warnings_.push_back("<gear name=\"" + gd.name + "\">: name must be 1..99 or R - "
                                "the automatic shift will not use it");
        }
        gears_.push_back(g);
    }
    for (const auto& n : desc.neutral) {
        const auto it = elemOf.find(n);
        if (it == elemOf.end()) {
            errors_.push_back("neutral: no <element name=\"" + n + "\">");
            continue;
        }
        neutral_.push_back(it->second);
    }
    if (!ok()) return;

    for (std::size_t g = 0; g < gears_.size(); ++g) {
        if (gears_[g].number > 0) forward_.push_back(int(g));
    }
    std::sort(forward_.begin(), forward_.end(), [&](int a, int b) {
        return gears_[std::size_t(a)].number < gears_[std::size_t(b)].number;
    });
    if (forward_.empty()) errors_.push_back("no forward gear (<gear name=\"1\"> ...)");

    // 段ごとの変速比: 遊星の式 + 締結した要素 + 入力 = 1 を解いて 1 / ω_out。
    const int n = int(shaftCount_);
    for (std::size_t gi = 0; gi < gears_.size(); ++gi) {
        Gear& gear = gears_[gi];
        const int m = int(sets_.size() + gear.elements.size()) + 1;
        if (m != n) continue;
        std::vector<double> M(std::size_t(n * n), 0.0), b(std::size_t(n), 0.0);
        int r = 0;
        for (const auto& s : sets_) {
            M[std::size_t(r * n + s.s)] += 1.0;
            M[std::size_t(r * n + s.r)] += s.k;
            M[std::size_t(r * n + s.c)] -= 1.0 + s.k;
            ++r;
        }
        for (int e : gear.elements) {
            const Element& el = elements_[std::size_t(e)];
            M[std::size_t(r * n + el.a)] += 1.0;
            if (el.b >= 0) M[std::size_t(r * n + el.b)] -= 1.0;
            ++r;
        }
        M[std::size_t(r * n + in_)] = 1.0;
        b[std::size_t(r)] = 1.0;
        if (!solveDense(M, b, n)) continue;
        const double wo = b[std::size_t(out_)];
        if (std::fabs(wo) > 1e-9) gear.ratio = 1.0 / wo;
    }

    // 要素の分担と、容量の自動決定（段ごとの分担の最大 × 定格 × 1.4）。
    factors_.assign(gears_.size(), std::vector<double>(elements_.size(), 0.0));
    for (std::size_t g = 0; g < gears_.size(); ++g) {
        if (!staticFactors(g, factors_[g])) {
            warnings_.push_back("<gear name=\"" + desc.gears[g].name +
                                "\">: the engaged elements do not give one ratio");
        }
    }
    for (std::size_t e = 0; e < elements_.size(); ++e) {
        double cap = desc.elements[e].capacity;
        if (cap <= 0.0) {
            double f = 0.0;
            for (std::size_t g = 0; g < gears_.size(); ++g) f = std::max(f, std::fabs(factors_[g][e]));
            cap = std::max(50.0, f * desc.rating * 1.4);
        }
        elements_[e].cap = cap;
    }
}

// 段 g の静的な釣り合い: 入力に 1 N·m、出力を止めた（ブレーキ）ときの各行の反トルク。
bool AtKinematics::staticFactors(std::size_t g, std::vector<double>& out) const {
    out.assign(elements_.size(), 0.0);
    const int n = int(shaftCount_);
    const Gear& gear = gears_[g];
    const int m = int(sets_.size() + gear.elements.size()) + 1;
    if (m != n || gear.ratio == 0.0) return false;
    // M[軸][行] = 行の係数、解く: Σ G_r λ_r = −e_in
    std::vector<double> M(std::size_t(n * n), 0.0), b(std::size_t(n), 0.0);
    int r = 0;
    for (const auto& s : sets_) {
        M[std::size_t(s.s * n + r)] += 1.0;
        M[std::size_t(s.r * n + r)] += s.k;
        M[std::size_t(s.c * n + r)] -= 1.0 + s.k;
        ++r;
    }
    for (int e : gear.elements) {
        const Element& el = elements_[std::size_t(e)];
        M[std::size_t(el.a * n + r)] += 1.0;
        if (el.b >= 0) M[std::size_t(el.b * n + r)] -= 1.0;
        ++r;
    }
    M[std::size_t(out_ * n + r)] = 1.0;
    b[std::size_t(in_)] = -1.0;
    if (!solveDense(M, b, n)) return false;
    for (std::size_t i = 0; i < gear.elements.size(); ++i) {
        out[std::size_t(gear.elements[i])] = b[sets_.size() + i];
    }
    return true;
}

double AtKinematics::factor(std::size_t g, std::size_t e) const {
    if (g >= factors_.size() || e >= factors_[g].size()) return 0.0;
    return factors_[g][e];
}

bool AtKinematics::singleSwap(int from, int to) const {
    if (from < 0 || to < 0) return false;
    const auto& a = gears_[std::size_t(from)].elements;
    const auto& b = gears_[std::size_t(to)].elements;
    int on = 0, off = 0;
    for (int e : b) on += std::find(a.begin(), a.end(), e) == a.end();
    for (int e : a) off += std::find(b.begin(), b.end(), e) == b.end();
    return on == 1 && off == 1;
}

}  // namespace transmission
}  // namespace wizengine
