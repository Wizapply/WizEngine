#include "components/AtDriveline.h"

#include <algorithm>
#include <cmath>

#include "physics/PhysicsWorld.h"

namespace at = wizengine::transmission;
namespace ph = wizengine::physics;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRpm = 60.0 / (2.0 * kPi);  // rad/s → rpm
constexpr double kG = 9.81;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// トルコンの表（K は rpm/√(N·m)、0 = 容量ゼロ）を Chrono の容量係数
// （(rad/s)/√(N·m)）の細かい折れ線に直す。点の間は容量 c = 1/K² を直線でつなぐ
// （AtTypes.h の ConverterDesc の注記）。Chrono は K を直線でつなぐので、元の点の
// 間を細かく刻んで、c が直線 = トルクがなめらかに変わるようにする。
void converterTable(const at::ConverterDesc& cv, std::vector<double>& sr, std::vector<double>& k,
                    std::vector<double>& tr) {
    auto cOf = [&](std::size_t i) { return cv.k[i] > 0.0 ? 1.0 / (cv.k[i] * cv.k[i]) : 0.0; };
    auto interp = [&](const std::vector<double>& ys, double x) {
        if (x <= cv.sr.front()) return ys.front();
        if (x >= cv.sr.back()) return ys.back();
        std::size_t i = 1;
        while (i < cv.sr.size() && cv.sr[i] < x) ++i;
        const double t = (x - cv.sr[i - 1]) / (cv.sr[i] - cv.sr[i - 1]);
        return ys[i - 1] + (ys[i] - ys[i - 1]) * t;
    };
    std::vector<double> cs(cv.sr.size());
    for (std::size_t i = 0; i < cs.size(); ++i) cs[i] = cOf(i);
    const double toRad = 2.0 * kPi / 60.0;
    const double x0 = cv.sr.front(), x1 = cv.sr.back();
    const int n = 200;
    sr.clear();
    k.clear();
    tr.clear();
    for (int i = 0; i <= n; ++i) {
        const double x = x0 + (x1 - x0) * double(i) / n;
        const double c = interp(cs, x);
        // c = 0（容量ゼロ）は大きな K で代える（Chrono は K で割るので 0 は置けない）。
        const double kk = c > 1e-12 ? 1.0 / std::sqrt(c) : 1.0e5;
        sr.push_back(x);
        k.push_back(std::min(kk, 1.0e5) * toRad);
        tr.push_back(interp(cv.tr, x));
    }
}

}  // namespace

AtDriveline::AtDriveline(const at::AtDesc& desc)
    : desc_(desc), kin_(desc_), control_(desc_, kin_) {}

bool AtDriveline::build(PhysicsWorld& physics, std::size_t engineBody, const double bodyAxis[3],
                        double engineW0, std::string& error) {
    if (!kin_.ok()) {
        error = kin_.errors().empty() ? "invalid transmission" : kin_.errors().front();
        return false;
    }
    const auto& d = desc_;
    const auto& o = d.output;
    ph::ShaftNetworkSpec spec;
    // 軸
    auto addShaft = [&](double inertia, double w0) {
        spec.shafts.push_back(ph::ShaftNetworkSpec::Shaft{inertia, w0});
        return int(spec.shafts.size()) - 1;
    };
    const bool ownEngine = d.input == at::InputMode::Torque;
    engine_ = addShaft(d.converter.pumpInertia + (ownEngine ? d.engine.inertia : 0.0), engineW0);
    primary_ = addShaft(d.lockup.inertia, 0.0);
    train0_ = int(spec.shafts.size());
    for (const auto& s : d.shafts) addShaft(s.inertia, 0.0);
    input_ = train0_ + kin_.inputShaft();
    output_ = train0_ + kin_.outputShaft();
    spec.shafts[std::size_t(input_)].inertia += d.converter.turbineInertia;
    spec.shafts[std::size_t(output_)].inertia += o.inertia;
    const bool vehicle = d.load == at::LoadMode::Vehicle;
    fd_ = wheel_ = -1;
    if (vehicle) {
        fd_ = addShaft(0.01, 0.0);
        wheel_ = addShaft(o.mass * o.wheelRadius * o.wheelRadius + o.wheelInertia, 0.0);
    }
    shaftCount_ = spec.shafts.size();
    // 遊星
    for (const auto& g : kin_.gearsets()) {
        spec.planetaries.push_back(
            ph::ShaftNetworkSpec::Planetary{train0_ + g.c, train0_ + g.s, train0_ + g.r, g.k});
    }
    // クラッチ: 締結要素 → ロックアップ → ブレーキ → パーキング
    for (const auto& e : kin_.elements()) {
        spec.clutches.push_back(
            ph::ShaftNetworkSpec::Clutch{train0_ + e.a, e.b >= 0 ? train0_ + e.b : -1});
    }
    lockupClutch_ = int(spec.clutches.size());
    spec.clutches.push_back(ph::ShaftNetworkSpec::Clutch{engine_, primary_});
    brakeClutch_ = parkClutch_ = -1;
    if (vehicle) {
        brakeClutch_ = int(spec.clutches.size());
        spec.clutches.push_back(ph::ShaftNetworkSpec::Clutch{wheel_, -1});
    }
    parkClutch_ = int(spec.clutches.size());
    spec.clutches.push_back(ph::ShaftNetworkSpec::Clutch{output_, -1});
    // input = body: 剛体へ直結する短い軸（慣性は剛体が持つので小さく）と、エンジン軸との
    // 切り離し（inputs().couple）。
    link_ = coupleClutch_ = -1;
    if (d.input == at::InputMode::Body) {
        link_ = addShaft(0.001, engineW0);
        shaftCount_ = spec.shafts.size();
        coupleClutch_ = int(spec.clutches.size());
        spec.clutches.push_back(ph::ShaftNetworkSpec::Clutch{link_, engine_});
    }
    // ばね: ダンパ（1 次側 → 入力軸）、ドライブシャフト（終減速の後 → 車輪）
    damperSpring_ = 0;
    spec.springs.push_back(ph::ShaftNetworkSpec::Spring{primary_, input_, d.lockup.stiffness,
                                                        d.lockup.damping});
    shaftSpring_ = finalGear_ = -1;
    if (vehicle) {
        shaftSpring_ = 1;
        spec.springs.push_back(
            ph::ShaftNetworkSpec::Spring{fd_, wheel_, o.stiffness, o.damping});
        finalGear_ = 0;
        spec.gears.push_back(ph::ShaftNetworkSpec::Gear{output_, fd_, 1.0 / o.finalDrive});
    }
    // トルコン
    {
        ph::ShaftNetworkSpec::Converter c;
        c.pump = engine_;
        c.turbine = input_;
        converterTable(d.converter, c.sr, c.k, c.tr);
        spec.converters.push_back(std::move(c));
    }
    // エンジン軸の与え方と、出力の台上試験
    engineMotor_ = dynoMotor_ = bodyLink_ = -1;
    if (d.input == at::InputMode::Speed) {
        engineMotor_ = int(spec.motors.size());
        spec.motors.push_back(ph::ShaftNetworkSpec::SpeedMotor{engine_});
    }
    if (d.load == at::LoadMode::Dyno) {
        dynoMotor_ = int(spec.motors.size());
        spec.motors.push_back(ph::ShaftNetworkSpec::SpeedMotor{output_});
    }
    if (d.input == at::InputMode::Body) {
        if (engineBody == kNone) {
            error = "input=\"body\": the engine body \"" + d.engine.body + "\" was not found";
            return false;
        }
        ph::ShaftNetworkSpec::BodyLink b;
        b.shaft = link_;
        b.body = engineBody;
        for (int i = 0; i < 3; ++i) b.dir[i] = bodyAxis[i];
        bodyLink_ = 0;
        spec.bodyLinks.push_back(b);
    }

    net_ = physics.addShaftNetwork(spec);
    if (net_ == kNone) {
        error = "the shaft network could not be created (needs the core backend)";
        return false;
    }
    control_.reset();
    in_ = at::AtInputs{};
    out_ = at::AtReadout{};
    st_ = ph::ShaftNetworkState{};
    engApplied_ = pumpLoss_ = speedPrev_ = effFilt_ = 0.0;
    read(physics, 0.0);
    return true;
}

double AtDriveline::converterTorqueRatio(double sr) const {
    const auto& cv = desc_.converter;
    if (sr <= cv.sr.front()) return cv.tr.front();
    if (sr >= cv.sr.back()) return cv.tr.back();
    std::size_t i = 1;
    while (i < cv.sr.size() && cv.sr[i] < sr) ++i;
    const double t = (sr - cv.sr[i - 1]) / (cv.sr[i] - cv.sr[i - 1]);
    return cv.tr[i - 1] + (cv.tr[i] - cv.tr[i - 1]) * t;
}

// ---- step の前 -----------------------------------------------------------------
void AtDriveline::apply(PhysicsWorld& physics, double dt) {
    if (!built() || !(dt > 0.0)) return;
    const auto& d = desc_;
    const auto& o = d.output;
    const auto& w = st_.w;
    auto W = [&](int i) { return i >= 0 && std::size_t(i) < w.size() ? w[std::size_t(i)] : 0.0; };

    // TCU（前のステップの回転とトルクから、このステップの容量を決める）
    at::AtPlantState ps;
    ps.wEngine = W(engine_);
    ps.wInput = W(input_);
    ps.wOutput = W(output_);
    ps.kmh = out_.speed * 3.6;
    ps.engineTorque = out_.engineTorque;
    ps.inputTorque = out_.inputTorque;
    control_.step(dt, in_, ps);

    std::vector<double> loads(shaftCount_, 0.0);
    // エンジン（input = torque のときだけ、ここがトルクを与える）
    const double engRpm = W(engine_) * kRpm;
    engApplied_ = 0.0;
    if (d.input == at::InputMode::Torque) {
        double t = in_.engineTorque;
        if (t > 0.0) t *= control_.torqueRequest();
        if (d.engine.idle > 0.0) {
            t += clampd(d.engine.idleGain * (d.engine.idle - engRpm), 0.0, 200.0);
        }
        if (engRpm > d.engine.redline && t > 0.0) t = 0.0;
        engApplied_ = t;
        loads[std::size_t(engine_)] += t;
    }
    // 油圧ポンプ（エンジン駆動。吐出量 × ライン圧）
    pumpLoss_ = d.pump.displacement * 1e-6 * control_.linePressure() * 1e5 /
                (2.0 * kPi * d.pump.efficiency) * std::tanh(W(engine_) / 10.0);
    loads[std::size_t(engine_)] -= pumpLoss_;
    // 軸受・かき混ぜの損失
    for (std::size_t s = 0; s < d.shafts.size(); ++s) {
        const int i = train0_ + int(s);
        loads[std::size_t(i)] -= d.shafts[s].drag * W(i);
    }
    // 開いている要素の引きずり（相対回転に比例。締まるほど消える）
    const auto& els = kin_.elements();
    for (std::size_t e = 0; e < els.size(); ++e) {
        const double c = d.elements[e].drag * (1.0 - clamp01(control_.elementPressure(e) * 4.0));
        if (c <= 0.0) continue;
        const int a = train0_ + els[e].a;
        const int b = els[e].b >= 0 ? train0_ + els[e].b : -1;
        const double rel = W(a) - (b >= 0 ? W(b) : 0.0);
        loads[std::size_t(a)] -= c * rel;
        if (b >= 0) loads[std::size_t(b)] += c * rel;
    }
    // かみ合いの損失（前のステップの太陽のトルクから。転がりの仕事 × (1 − 効率)）
    const auto& sets = kin_.gearsets();
    for (std::size_t g = 0; g < sets.size() && g < st_.sunTorque.size(); ++g) {
        const auto& s = sets[g];
        const int is = train0_ + s.s, ic = train0_ + s.c, ir = train0_ + s.r;
        const double ts = st_.sunTorque[g];
        const double l1 = (1.0 - s.eff) * std::fabs(ts) * std::tanh((W(is) - W(ic)) / 2.0);
        const double l2 = (1.0 - s.eff) * std::fabs(s.k * ts) * std::tanh((W(ir) - W(ic)) / 2.0);
        loads[std::size_t(is)] -= l1;
        loads[std::size_t(ir)] -= l2;
        loads[std::size_t(ic)] += l1 + l2;
    }
    // 終減速の損失と車両の抵抗（転がり・空気・勾配）
    if (wheel_ >= 0) {
        loads[std::size_t(output_)] -= (1.0 - o.finalEfficiency) * std::fabs(out_.outputTorque) *
                                       std::tanh(W(output_) / 2.0);
        const double v = W(wheel_) * o.wheelRadius;
        const double f = o.crr * o.mass * kG * std::cos(in_.grade) * std::tanh(v / 0.3) +
                         0.5 * o.rho * o.cda * v * std::fabs(v) + o.mass * kG * std::sin(in_.grade);
        loads[std::size_t(wheel_)] -= f * o.wheelRadius;
    }
    physics.setShaftLoads(net_, loads);

    // クラッチの容量: 締結要素（滑っている間は動摩擦 = すべり速度で 1 へ戻る比）・
    // ロックアップ・ブレーキ・パーキング
    std::vector<double> limits(els.size() + 4, 0.0);
    for (std::size_t e = 0; e < els.size(); ++e) {
        const int a = train0_ + els[e].a;
        const int b = els[e].b >= 0 ? train0_ + els[e].b : -1;
        const double slip = std::fabs(W(a) - (b >= 0 ? W(b) : 0.0));
        const double dyn = d.elements[e].dynamic;
        const double mu = slip > 0.5 ? dyn + (1.0 - dyn) * clamp01(slip / 30.0) : 1.0;
        limits[e] = control_.elementCapacity(e) * mu;
    }
    limits[std::size_t(lockupClutch_)] = control_.lockupCapacity();
    if (brakeClutch_ >= 0) limits[std::size_t(brakeClutch_)] = o.brake * clamp01(in_.brake);
    if (parkClutch_ >= 0) {
        // パーキングポール: 止まりかけなら出力軸を止める（速いと歯先で弾かれる）。
        limits[std::size_t(parkClutch_)] =
            in_.select == 0 ? (std::fabs(out_.speed) * 3.6 < 5.0 ? 1e5 : 100.0) : 0.0;
    }
    if (coupleClutch_ >= 0) {
        limits[std::size_t(coupleClutch_)] = in_.couple > 0.5 ? 1e5 : 0.0;
    }
    limits.resize(std::size_t(std::max(parkClutch_, coupleClutch_)) + 1);
    physics.setShaftClutchLimits(net_, limits);

    // 速度モータ（エンジン回転を与える / 台上の出力回転）
    if (engineMotor_ >= 0 || dynoMotor_ >= 0) {
        std::vector<double> speeds(std::size_t(std::max(engineMotor_, dynoMotor_) + 1), 0.0);
        if (engineMotor_ >= 0) speeds[std::size_t(engineMotor_)] = in_.engineRpm / kRpm;
        if (dynoMotor_ >= 0) {
            const double rpm = in_.outputRpmSet ? in_.outputRpm : o.dynoRpm;
            speeds[std::size_t(dynoMotor_)] = rpm / kRpm;
        }
        physics.setShaftMotorSpeeds(net_, speeds);
    }
}

// ---- step の後 -----------------------------------------------------------------
void AtDriveline::read(PhysicsWorld& physics, double dt) {
    if (!built()) return;
    physics.readShaftNetwork(net_, st_);
    const auto& d = desc_;
    const auto& o = d.output;
    auto W = [&](int i) {
        return i >= 0 && std::size_t(i) < st_.w.size() ? st_.w[std::size_t(i)] : 0.0;
    };
    auto at0 = [](const std::vector<double>& v, int i) {
        return i >= 0 && std::size_t(i) < v.size() ? v[std::size_t(i)] : 0.0;
    };
    at::AtReadout& r = out_;
    r.engineRpm = W(engine_) * kRpm;
    r.turbineRpm = W(input_) * kRpm;
    r.outputRpm = W(output_) * kRpm;
    const double wWheel = wheel_ >= 0 ? W(wheel_) : W(output_) / o.finalDrive;
    r.wheelRpm = wWheel * kRpm;

    const auto tc = st_.converters.empty() ? ph::ShaftNetworkState::ConverterState{}
                                           : st_.converters.front();
    const double lockOnEngine = at0(st_.clutchTorque, lockupClutch_);
    const double damperOnInput = at0(st_.springTorque, damperSpring_);
    r.turbineTorque = tc.turbineTorque;
    r.inputTorque = tc.turbineTorque + damperOnInput;
    // AT がエンジン軸から取ったトルク = 流体 + ロックアップ + 油圧ポンプ（向きは取る側が +）。
    r.loadTorque = -(tc.pumpTorque + lockOnEngine) + pumpLoss_;
    if (d.input == at::InputMode::Torque) {
        r.engineTorque = engApplied_;
    } else if (d.input == at::InputMode::Speed) {
        // 速度モータがエンジン軸を回したトルク = 回転を保つのに要ったトルク。
        r.engineTorque = at0(st_.motorTorque, engineMotor_);
    } else {
        // 剛体（クランク）が AT へ渡したトルク = 切り離しのクラッチがエンジン軸に掛けた
        // トルク（反トルクは直結の軸 = 軸 a に掛かるので反転する）。
        r.engineTorque = -at0(st_.clutchTorque, coupleClutch_);
    }
    if (wheel_ >= 0) {
        r.axleTorque = at0(st_.springTorque, shaftSpring_);
        // 歯車が終減速の後の軸に掛けたトルク ÷ 終減速比 = 出力軸が出したトルク。
        r.outputTorque = at0(st_.gearTorque, finalGear_) / o.finalDrive;
    } else {
        // 台上: モータが出力軸に掛けたトルクの反対 = AT が出したトルク。
        r.outputTorque = -at0(st_.motorTorque, dynoMotor_);
        r.axleTorque = r.outputTorque * o.finalDrive;
    }
    r.gear = control_.gear();
    r.targetGear = control_.targetGear();
    r.shifting = control_.shifting();
    r.phase = control_.phase();
    r.ratio = std::fabs(W(output_)) > 1.0 ? W(input_) / W(output_) : 0.0;
    r.speedRatio = tc.speedRatio;
    r.torqueRatio = W(input_) <= W(engine_) ? converterTorqueRatio(clamp01(tc.speedRatio)) : 1.0;
    r.tcSlipRpm = (W(engine_) - W(input_)) * kRpm;
    r.lockup = control_.lockupPressure();
    r.lockupSlipRpm = (W(engine_) - W(primary_)) * kRpm;
    r.speed = wWheel * o.wheelRadius;
    r.accel = dt > 0.0 ? (r.speed - speedPrev_) / dt : 0.0;
    speedPrev_ = r.speed;
    r.torqueRequest = control_.torqueRequest();
    r.linePressure = control_.linePressure();
    r.powerIn = r.loadTorque * W(engine_) / 1000.0;
    r.powerOut = r.outputTorque * W(output_) / 1000.0;
    if (r.powerIn > 1.0 && dt > 0.0) {
        const double e = clampd(r.powerOut / r.powerIn, -1.0, 1.0);
        effFilt_ += (e - effFilt_) * (1.0 - std::exp(-dt / 0.2));
    }
    r.efficiency = effFilt_;

    const auto& els = kin_.elements();
    r.elementTorque.assign(els.size(), 0.0);
    r.elementPressure.assign(els.size(), 0.0);
    r.elementSlipRpm.assign(els.size(), 0.0);
    for (std::size_t e = 0; e < els.size(); ++e) {
        r.elementTorque[e] = at0(st_.clutchTorque, int(e));
        r.elementPressure[e] = control_.elementPressure(e);
        const int a = train0_ + els[e].a;
        const int b = els[e].b >= 0 ? train0_ + els[e].b : -1;
        r.elementSlipRpm[e] = (W(a) - (b >= 0 ? W(b) : 0.0)) * kRpm;
    }
    r.shaftW.assign(d.shafts.size(), 0.0);
    for (std::size_t s = 0; s < d.shafts.size(); ++s) r.shaftW[s] = W(train0_ + int(s));
}
