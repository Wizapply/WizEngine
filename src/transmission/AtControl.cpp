#include "transmission/AtControl.h"

#include <algorithm>
#include <cmath>

namespace wizengine {
namespace transmission {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRpm = 60.0 / (2.0 * kPi);  // rad/s → rpm

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace

AtControl::AtControl(const AtDesc& desc, const AtKinematics& kin) : desc_(desc), kin_(kin) {
    reset();
}

void AtControl::reset() {
    elements_.assign(kin_.elements().size(), Element{});
    // N / P で締結している要素は最初から満圧（エンジンを掛けたまま置いてある状態）。
    for (int e : kin_.neutral()) {
        Element& el = elements_[std::size_t(e)];
        el.cmd = el.fill = el.p = 1.0;
    }
    ps_ = AtPlantState{};
    cur_ = -1;
    want_ = -1;
    sinceShift_ = 10.0;
    kickArmed_ = true;
    shift_ = Shift{};
    request_ = 1.0;
    lockupCmd_ = lockupP_ = 0.0;
    linePressure_ = desc_.pump.pressureMin;
    throttle_ = 0.0;
}

void AtControl::step(double dt, const AtInputs& in, const AtPlantState& ps) {
    if (!kin_.ok() || !(dt > 0.0)) return;
    ps_ = ps;
    throttle_ = in.throttle;
    runTcu(dt, in);
    hydraulics(dt);
    // ライン圧は負荷で上げる（油圧ポンプの損失トルクの見積もりに使う）。
    linePressure_ = desc_.pump.pressureMin +
                    (desc_.pump.pressureMax - desc_.pump.pressureMin) *
                        clamp01(std::max(in.throttle, std::fabs(ps.inputTorque) / desc_.rating));
}

double AtControl::elementCapacity(std::size_t e) const {
    const double kiss = desc_.shift.kiss;
    const double frac = clamp01((elements_[e].p - kiss) / (1.0 - kiss));
    return kin_.elements()[e].cap * frac;
}

int AtControl::gear() const {
    return cur_ >= 0 ? kin_.gears()[std::size_t(cur_)].number : 0;
}

int AtControl::targetGear() const {
    const int tg = shift_.active ? shift_.to : want_;
    return tg >= 0 ? kin_.gears()[std::size_t(tg)].number : 0;
}

double AtControl::eqRpm(int g) const {
    if (g < 0) return 0.0;
    return std::fabs(ps_.wOutput * kin_.gears()[std::size_t(g)].ratio) * kRpm;
}

double AtControl::turbineSideInertia() const {
    double j = desc_.converter.turbineInertia + desc_.lockup.inertia;
    if (kin_.inputShaft() >= 0) j += desc_.shafts[std::size_t(kin_.inputShaft())].inertia;
    if (lockupP_ > 0.5) j += desc_.engine.inertia + desc_.converter.pumpInertia;
    return j;
}

void AtControl::startShift(int to) {
    const int from = cur_;
    const std::vector<int>& sf = from >= 0 ? kin_.gears()[std::size_t(from)].elements : kin_.neutral();
    const std::vector<int>& st = to >= 0 ? kin_.gears()[std::size_t(to)].elements : kin_.neutral();
    Shift s;
    s.active = true;
    s.from = from;
    s.to = to;
    for (int e : st) {
        if (std::find(sf.begin(), sf.end(), e) == sf.end()) s.on.push_back(e);
    }
    for (int e : sf) {
        if (std::find(st.begin(), st.end(), e) == st.end()) s.off.push_back(e);
    }
    const bool fwd = from >= 0 && to >= 0 && from != kin_.reverse() && to != kin_.reverse();
    if (fwd && s.on.size() == 1 && s.off.size() == 1) {
        s.oc = s.on[0];
        s.og = s.off[0];
        s.up = std::fabs(kin_.gears()[std::size_t(to)].ratio) < std::fabs(kin_.gears()[std::size_t(from)].ratio);
        // パワーオン / オフはエンジンが出しているトルクで決める（入力軸のトルクは急ブレーキで
        // エンジンの慣性が押し込むぶんを含み、減速中でも正になるため）。
        s.powerOn = ps_.engineTorque > 10.0 && throttle_ > 0.02;
        s.ocFirst = s.up == s.powerOn;
        s.phase = s.ocFirst ? 1 : 3;
        s.wStart = ps_.wInput;
        // 準備の間の解放側: 分担 × 余裕まで油圧を下げる（トルク相を短くするため）。
        const double kiss = desc_.shift.kiss;
        const double ogCap = kin_.elements()[std::size_t(s.og)].cap;
        const double need =
            desc_.shift.margin * std::fabs(kin_.factor(std::size_t(from), std::size_t(s.og)) * ps_.inputTorque);
        s.ogHold = kiss + (1.0 - kiss) * clampd(need / ogCap, 0.15, 1.0);
    } else {
        s.garage = true;
        s.phase = 1;
    }
    shift_ = s;
    sinceShift_ = 0.0;
}

void AtControl::runTcu(double dt, const AtInputs& in) {
    const auto& sd = desc_.shift;
    sinceShift_ += dt;
    request_ = 1.0;
    const int sel = std::max(0, std::min(4, in.select));
    const double th = clamp01(in.throttle);

    // 欲しい段。
    int want = cur_;
    if (sel == 0 || sel == 2) {
        want = -1;
    } else if (sel == 1) {
        want = kin_.reverse();
    } else if (cur_ < 0 || cur_ == kin_.reverse()) {
        want = kin_.forward().front();
    } else if (!shift_.active) {
        const auto it = std::find(kin_.forward().begin(), kin_.forward().end(), cur_);
        const int pos = int(it - kin_.forward().begin());
        const int last = int(kin_.forward().size()) - 1;
        const double n = eqRpm(cur_);
        if (sel == 3) {
            // 変速線は開度の 1.5 乗で曲げる（軽く踏んでいる間は早めに上げる）。
            const double shape = std::pow(th, 1.5);
            const double up = lerp(sd.upLight, sd.upFull, shape);
            const double dn = lerp(sd.downLight, sd.downFull, shape);
            if (sinceShift_ >= sd.minTime) {
                // ブレーキ中は上げない（戻した直後に軽負荷の線で上げてしまうと、減速の
                // ための下げをやり直すことになる）。
                if (pos < last && n > up && in.brake < 0.05 &&
                    eqRpm(kin_.forward()[std::size_t(pos + 1)]) > dn * 1.02) {
                    want = kin_.forward()[std::size_t(pos + 1)];
                } else if (pos > 0 && n < dn && eqRpm(kin_.forward()[std::size_t(pos - 1)]) < up * 0.97) {
                    // 下げ: 下げ線より上に収まるいちばん高い段へ（減速が速いと 1 段ずつでは
                    // 追いつかない）。1 要素の掛け替えで行ける段だけ、無ければ 1 段下。
                    want = kin_.forward()[std::size_t(pos - 1)];
                    for (int p = pos - 2; p >= 0; --p) {
                        const int g = kin_.forward()[std::size_t(p)];
                        if (eqRpm(kin_.forward()[std::size_t(p + 1)]) >= dn) break;
                        if (eqRpm(g) < up * 0.97 && kin_.singleSwap(cur_, g)) want = g;
                    }
                }
                // キックダウン: 踏み込んだ瞬間に、上げ線の手前に収まるいちばん低い段へ
                // （1 要素の掛け替えで行ける段だけ）。
                if (th >= sd.kickdown && kickArmed_ && pos > 0) {
                    for (int p = 0; p < pos; ++p) {
                        const int g = kin_.forward()[std::size_t(p)];
                        if (eqRpm(g) < sd.upFull * 0.93 && kin_.singleSwap(cur_, g)) {
                            want = g;
                            break;
                        }
                    }
                    kickArmed_ = false;
                }
            }
            if (th < sd.kickdown - 0.1) kickArmed_ = true;
        } else {
            // 手動: 1 段ずつ近づける。回し過ぎになる下げはしない、レッドラインで上げる。
            const int target = std::max(1, std::min(kin_.gears()[std::size_t(kin_.forward().back())].number,
                                                    in.manualGear));
            const int tpos = int(std::find_if(kin_.forward().begin(), kin_.forward().end(),
                                              [&](int g) {
                                                  return kin_.gears()[std::size_t(g)].number >= target;
                                              }) -
                                 kin_.forward().begin());
            if (sinceShift_ >= 0.2) {
                if (tpos > pos && pos < last) {
                    want = kin_.forward()[std::size_t(pos + 1)];
                } else if (tpos < pos && pos > 0 &&
                           eqRpm(kin_.forward()[std::size_t(pos - 1)]) < desc_.engine.redline * 0.95) {
                    want = kin_.forward()[std::size_t(pos - 1)];
                }
                if (n > desc_.engine.redline * 0.98 && pos < last) want = kin_.forward()[std::size_t(pos + 1)];
                if (n < sd.downLight * 0.7 && pos > 0) want = kin_.forward()[std::size_t(pos - 1)];
            }
        }
    }
    want_ = want;
    if (!shift_.active && want != cur_) startShift(want);

    // 要素の指令。
    const double kiss = sd.kiss;
    const std::vector<int>& base = cur_ >= 0 ? kin_.gears()[std::size_t(cur_)].elements : kin_.neutral();
    for (std::size_t e = 0; e < elements_.size(); ++e) {
        elements_[e].cmd = std::find(base.begin(), base.end(), int(e)) != base.end() ? 1.0 : 0.0;
    }
    if (shift_.active) {
        Shift& s = shift_;
        s.t += dt;
        if (s.garage) {
            double fillMax = 0.0;
            for (int e : s.on) fillMax = std::max(fillMax, desc_.elements[std::size_t(e)].fill);
            for (int e : s.on) {
                elements_[std::size_t(e)].cmd =
                    s.t < fillMax ? kiss : kiss + (1.0 - kiss) * clamp01((s.t - fillMax) / sd.engage);
            }
            for (int e : s.off) elements_[std::size_t(e)].cmd = std::max(0.0, 1.0 - s.t / (0.5 * sd.engage));
            if (s.t >= fillMax + sd.engage) {
                cur_ = s.to;
                s.active = false;
            }
        } else {
            Element& oc = elements_[std::size_t(s.oc)];
            Element& og = elements_[std::size_t(s.og)];
            const double wo = ps_.wOutput;
            const double wSync = wo * kin_.gears()[std::size_t(s.to)].ratio;
            const double wIn = ps_.wInput;
            const double tin = std::fabs(ps_.inputTorque);
            const double fOC = std::fabs(kin_.factor(std::size_t(s.to), std::size_t(s.oc)));
            const double fOG = std::fabs(kin_.factor(std::size_t(s.from), std::size_t(s.og)));
            const double jin = turbineSideInertia();
            const double ocFill = desc_.elements[std::size_t(s.oc)].fill;
            auto traj = [&]() {
                return s.wStart + (wSync - s.wStart) * clamp01(s.t / sd.inertiaPhase);
            };
            const double sigma = wSync < s.wStart ? 1.0 : -1.0;
            const double alphaTerm = jin * std::fabs(wSync - s.wStart) / sd.inertiaPhase;
            const double ocCap = kin_.elements()[std::size_t(s.oc)].cap;
            const double ogCap = kin_.elements()[std::size_t(s.og)].cap;
            auto toCmd = [&](double torque, double cap) {
                return kiss + (1.0 - kiss) * clamp01(torque / cap);
            };
            if (s.ocFirst) {
                if (s.phase == 1) {
                    oc.cmd = kiss;
                    og.cmd = s.ogHold;
                    if (s.t >= ocFill) {
                        s.phase = 2;
                        s.t = 0.0;
                    }
                } else if (s.phase == 2) {
                    const double x = clamp01(s.t / sd.torquePhase);
                    const double need = toCmd(sd.margin * fOC * tin, ocCap);
                    oc.cmd = kiss + x * (need - kiss);
                    og.cmd = s.ogHold * (1.0 - x);
                    if (x >= 1.0) {
                        s.phase = 3;
                        s.t = 0.0;
                        s.wStart = wIn;
                        s.integ = 0.0;
                    }
                } else if (s.phase == 3) {
                    og.cmd = 0.0;
                    const double e = wIn - traj();
                    s.integ += sigma * e * dt;
                    const double cap = fOC * tin + fOC * alphaTerm + sd.kp * sigma * e + sd.ki * s.integ;
                    oc.cmd = toCmd(std::max(0.0, cap), ocCap);
                    if (s.up && s.powerOn) request_ = 1.0 - sd.reduction;
                    if ((std::fabs(wIn - wSync) * kRpm < 25.0 && s.t > 0.02) ||
                        s.t > 2.5 * sd.inertiaPhase) {
                        s.phase = 4;
                        s.t = 0.0;
                        s.ogCmd0 = oc.cmd;
                    }
                } else {
                    og.cmd = 0.0;
                    const double x = clamp01(s.t / 0.08);
                    oc.cmd = s.ogCmd0 + x * (1.0 - s.ogCmd0);
                    if (x >= 1.0) {
                        cur_ = s.to;
                        s.active = false;
                    }
                }
            } else {
                if (s.phase == 3) {
                    // 解放側を滑らせてタービンを新段の回転へ（締結側は準備だけ）。
                    oc.cmd = kiss;
                    const double e = wIn - traj();
                    s.integ += sigma * e * dt;
                    const double cap = fOG * tin - fOG * alphaTerm - sd.kp * sigma * e - sd.ki * s.integ;
                    og.cmd = toCmd(std::max(0.0, cap), ogCap);
                    if ((std::fabs(wIn - wSync) * kRpm < 40.0 && s.t >= ocFill) ||
                        s.t > 2.5 * sd.inertiaPhase) {
                        s.phase = 2;
                        s.t = 0.0;
                        s.ogCmd0 = og.cmd;
                    }
                } else {
                    const double x = clamp01(s.t / sd.torquePhase);
                    oc.cmd = kiss + x * (1.0 - kiss);
                    og.cmd = s.ogCmd0 * (1.0 - x);
                    if (x >= 1.0) {
                        cur_ = s.to;
                        s.active = false;
                    }
                }
            }
        }
    }

    // ロックアップ: D / M で fromGear 以上・一定の車速以上（5 km/h のヒステリシス）。
    {
        const double kmh = std::fabs(ps_.kmh);
        const int num = cur_ >= 0 ? kin_.gears()[std::size_t(cur_)].number : 0;
        const bool garage = shift_.active && shift_.garage;
        const double lim = desc_.lockup.speed - (lockupCmd_ > 0.5 ? 5.0 : 0.0);
        // エンジンがアイドル近くまで引きずられたら開ける（急ブレーキでのエンスト防止）。
        const bool stall = desc_.engine.idle > 0.0 && ps_.wEngine * kRpm < desc_.engine.idle + 250.0;
        lockupCmd_ = (sel >= 3 && num >= desc_.lockup.fromGear && kmh > lim && !garage && !stall)
                         ? 1.0
                         : 0.0;
    }
}

void AtControl::hydraulics(double dt) {
    for (std::size_t i = 0; i < elements_.size(); ++i) {
        Element& e = elements_[i];
        const ElementDesc& d = desc_.elements[i];
        if (e.cmd > 1e-4) {
            e.fill = d.fill > 0.0 ? std::min(1.0, e.fill + dt / d.fill) : 1.0;
        } else {
            e.fill = d.fill > 0.0 ? std::max(0.0, e.fill - dt / (0.5 * d.fill)) : 0.0;
        }
        const double target = e.fill >= 1.0 ? e.cmd : 0.0;
        e.p += (target - e.p) * (1.0 - std::exp(-dt / d.tau));
    }
    // 開けるのは締めるより速い（排圧するだけ）。
    const double ltau = lockupCmd_ < lockupP_ ? 0.2 * desc_.lockup.tau : desc_.lockup.tau;
    lockupP_ += (lockupCmd_ - lockupP_) * (1.0 - std::exp(-dt / ltau));
}


}  // namespace transmission
}  // namespace wizengine
