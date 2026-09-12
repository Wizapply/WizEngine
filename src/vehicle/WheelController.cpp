#include "vehicle/WheelController.h"

#include <algorithm>
#include <cmath>

#include "vehicle/TireFormula.h"

namespace wizengine {
namespace vehicle {

namespace {

// 停車付近でスリップ比の分母に使う最低速度（m/s）。これより遅い領域は
// 力の上限（このティックで滑りを止められる力）が実質の制御になる。
constexpr double kLowSpeed = 0.5;

// Pacejka の Magic Formula（D を外に出した正規化形）。
double magic(double s, double b, double c, double e) {
    const double bs = b * s;
    return std::sin(c * std::atan(bs - e * (bs - std::atan(bs))));
}

}  // namespace

GroundHit planeGroundQuery(const Vec3& origin, const Vec3& dir,
                           double maxDistance) {
    GroundHit h;
    if (dir.y >= -1e-9) return h;  // 下を向いていない
    const double t = -origin.y / dir.y;
    if (t < 0.0 || t > maxDistance) return h;
    h.hit = true;
    h.distance = t;
    h.point = origin + dir * t;
    h.normal = Vec3{0.0, 1.0, 0.0};
    return h;
}

WheelController::WheelController(const TireDesc& tire,
                                 const SuspensionDesc& suspension,
                                 const Vec3& localAttach)
    : tire_(tire), sus_(suspension), localAttach_(localAttach) {}

void WheelController::setTireFormula(std::unique_ptr<FormulaInstance> formula) {
    formula_ = std::move(formula);
    formulaFailures_ = 0;
}

void WheelController::reset() {
    if (formula_) formula_->reset();
    grounded_ = false;
    compression_ = 0.0;
    deflection_ = 0.0;
    load_ = 0.0;
    forceX_ = forceY_ = 0.0;
    relaxX_ = relaxY_ = 0.0;
    sumX_ = sumY_ = 0.0;
    avgX_ = avgY_ = 0.0;
    slipRatio_ = slipAngle_ = 0.0;
    omega_ = 0.0;
    spinAngle_ = 0.0;
}

void WheelController::updateContact(const ChassisState& chassis, double steerRad,
                                    const GroundQuery& ground, double massShare,
                                    double stepDt) {
    steerRad_ = steerRad;
    stepDt_ = stepDt;
    massShare_ = std::max(massShare, 1.0);
    sumX_ = sumY_ = 0.0;

    const Quat& q = chassis.rotation;
    strutUp_ = q.rotate(Vec3{0.0, 1.0, 0.0}).normalized();
    const Vec3 down = -strutUp_;
    attachWorld_ = chassis.position + q.rotate(localAttach_);

    // 舵を切った向きの「前」（車輪の面内、down と直交）。
    const double sSteer = std::sin(steerRad), cSteer = std::cos(steerRad);
    const Vec3 fwdWheel = q.rotate(Vec3{-sSteer, 0.0, -cSteer});

    // 接地の検出。伸びきった車輪の底まで届くレイを飛ばし、当たらなければ
    // 宙に浮いている。真下 1 本だと段差で縮みが一気に飛ぶ（車輪の中心が縁を
    // 越えた瞬間に段の高さぶん持ち上がる = 車が跳ねる）ので、車輪の面内で
    // ±60° に広げた rays 本の扇にし、それぞれの当たり点に「半径 r の円が
    // 触れる」ときの車輪中心の高さに直す:
    //   c = L cosθ − sqrt(r² − (L sinθ)²)   （c = 取り付け点から中心までの距離）
    // 真下（θ = 0）なら c = L − r で従来と同じ。一番高い円（c 最小）を採る =
    // 縁に近づくにつれて円が縁に乗り上げ、縮みは連続に変わる。法線は
    // 「当たり点 → 円の中心」（縁に乗っているときは斜め = 乗り越える向きに
    // タイヤ力が働く）。rays = 1 は旧来どおり真下 1 本・面の法線。
    const double reach = sus_.restLength + tire_.radius;
    const double r = tire_.radius;
    const int rays = std::max(1, std::min(tire_.rays, 15));
    const double thetaMax = degToRad(60.0);
    bool found = false;
    double bestC = 0.0;
    Vec3 bestPoint, bestNormal;
    for (int k = 0; k < rays; ++k) {
        const double theta =
            rays == 1 ? 0.0 : -thetaMax + 2.0 * thetaMax * double(k) / double(rays - 1);
        const double ct = std::cos(theta), st = std::sin(theta);
        const Vec3 dir = (down * ct + fwdWheel * st).normalized();
        const GroundHit hit = ground(attachWorld_, dir, reach / ct);
        if (!hit.hit) continue;
        const double L = hit.distance;
        const double s = std::fabs(L * st);
        if (s > r) continue;  // 車輪の真下（幅 2r の帯）に無い点
        const double c = L * ct - std::sqrt(std::max(r * r - s * s, 0.0));
        if (c > sus_.restLength) continue;  // 円がそこまで下がらない = 触れない
        if (found && c >= bestC) continue;
        found = true;
        bestC = c;
        bestPoint = hit.point;
        if (rays == 1) {
            bestNormal = hit.normal;
        } else {
            const Vec3 toCenter = attachWorld_ + down * c - hit.point;
            bestNormal = toCenter.length() > 1e-9 ? toCenter.normalized() : hit.normal;
        }
    }
    if (!found) {
        grounded_ = false;
        compression_ = 0.0;
        deflection_ = 0.0;
        load_ = 0.0;
        // 浮いている間はタイヤ力を残さない（緩和長の状態も含めて）。
        forceX_ = forceY_ = 0.0;
        relaxX_ = relaxY_ = 0.0;
        if (formula_) formula_->reset();
        contactPoint_ = attachWorld_ + down * reach;
        vx_ = vy_ = 0.0;
        return;
    }
    grounded_ = true;
    normal_ = bestNormal.normalized();
    contactPoint_ = bestPoint;
    // ソフトタイヤ: サスとタイヤの径方向ばねが直列に縮む。同じ力を分け合う
    // ので、縮みは剛性の逆比（サス側 = 全体 × k_t / (k_s + k_t)）。タイヤの
    // 潰れは半径の 25% で頭打ち（残りはサスが受ける）- それ以上潰すと接地面が
    // リム（半径 70%）より内側に来て、ゴムではなく金属が地面に着くことになる。
    // 剛タイヤなら全部サス。
    const double total = std::max(sus_.restLength - bestC, 0.0);
    if (tire_.soft && tire_.stiffness > 0.0) {
        const double maxDefl = 0.25 * tire_.radius;
        const double share = tire_.stiffness / (sus_.stiffness + tire_.stiffness);
        double xs = total * share;
        deflection_ = total - xs;
        if (deflection_ > maxDefl) {
            deflection_ = maxDefl;
            xs = total - deflection_;
        }
        compression_ = clampd(xs, 0.0, sus_.restLength);
    } else {
        deflection_ = 0.0;
        compression_ = clampd(total, 0.0, sus_.restLength);
    }

    // 縮み速度 = 取り付け点が地面へ向かう速度（地面は静止とみなす）。
    const Vec3 rAttach = attachWorld_ - chassis.position;
    const Vec3 vAttach = chassis.velocity + chassis.angularVelocity.cross(rAttach);
    const double rate = vAttach.dot(down);

    double f = sus_.stiffness * compression_ +
               (rate > 0.0 ? sus_.bump : sus_.rebound) * rate;
    if (compression_ > sus_.travel) {
        // バンプストップ: 8 倍のばねで受ける。
        f += sus_.stiffness * 8.0 * (compression_ - sus_.travel);
    }
    // 上限: 静止荷重の 8 倍。段差で縮みが飛んだステップの力が 1 発の衝撃に
    // ならないための安全弁（バンプストップの範囲は十分に残る）。
    load_ = std::min(std::max(f, 0.0), massShare_ * 9.81 * 8.0);

    // タイヤ座標系（地面に沿った前・右）。舵角は車体の上向きまわり。
    const Vec3& f0 = fwdWheel;
    Vec3 fwd = f0 - normal_ * f0.dot(normal_);
    if (fwd.length() < 1e-6) fwd = f0;
    forward_ = fwd.normalized();
    right_ = forward_.cross(normal_).normalized();

    const Vec3 rContact = contactPoint_ - chassis.position;
    const Vec3 vContact =
        chassis.velocity + chassis.angularVelocity.cross(rContact);
    vx_ = vContact.dot(forward_);
    vy_ = vContact.dot(right_);
}

void WheelController::addLoad(double newtons) {
    if (!grounded_) return;
    load_ = std::max(load_ + newtons, 0.0);
}

double WheelController::tick(double omega, double tickDt) {
    omega_ = omega;
    if (!grounded_ || load_ <= 0.0) {
        forceX_ = forceY_ = 0.0;
        relaxX_ = relaxY_ = 0.0;
        return 0.0;
    }
    const double r = tire_.radius;
    const double slipVel = omega * r - vx_;
    const double v = std::max(std::fabs(vx_), kLowSpeed);
    slipRatio_ = slipVel / v;
    slipAngle_ = std::atan2(vy_, v);

    // 力の許される範囲。向きは滑りに従う（縦力は滑り速度と同符号、横力は
    // 横滑りと逆符号）ので片側は常に 0。大きさは「このティックで滑り速度を
    // 反転させてしまう力」まで（車輪側はティックの dt、車体側はステップの
    // dt で効く）。停車付近の振動と、制動で溜まった逆向きの力が停車後に
    // 車を押し戻す（符号の遅れ）を消す。
    const double capX =
        std::fabs(slipVel) /
        (tickDt * r * r / tire_.inertia + stepDt_ / massShare_);
    const double capY = massShare_ * std::fabs(vy_) / stepDt_;
    const double fxLo = slipVel >= 0.0 ? 0.0 : -capX;
    const double fxHi = slipVel >= 0.0 ? capX : 0.0;
    const double fyLo = vy_ >= 0.0 ? -capY : 0.0;
    const double fyHi = vy_ >= 0.0 ? 0.0 : capY;

    // 組み込みの「スリップ → 力」: Magic Formula + 摩擦楕円 + 上限 + 緩和長。
    // ノード式（formula_）はこれと同じ入力を受けて fx / fy を返す。
    auto builtin = [&]() {
        const double peak = tire_.mu * load_;
        double fx = peak * magic(slipRatio_, tire_.bx, tire_.cx, tire_.ex);
        double fy = -peak * magic(slipAngle_, tire_.by, tire_.cy, tire_.ey);
        if (peak > 1e-9) {
            const double rho = std::sqrt((fx * fx + fy * fy) / (peak * peak));
            if (rho > 1.0) {
                fx /= rho;
                fy /= rho;
            }
        }
        // 範囲は緩和長の前に掛ける（状態が範囲を超えて溜まらない）。
        fx = clampd(fx, fxLo, fxHi);
        fy = clampd(fy, fyLo, fyHi);
        // 緩和長: 力が定常値へ一次遅れで追従する（時定数 = σ / 速度）。
        // 既定のノード式（TireFormula.cpp）と同じ順序・同じ状態の持ち方。
        if (tire_.relaxation > 1e-6) {
            const double tau = tire_.relaxation / v;
            const double a = std::min(1.0, tickDt / tau);
            relaxX_ += a * (fx - relaxX_);
            relaxY_ += a * (fy - relaxY_);
        } else {
            relaxX_ = fx;
            relaxY_ = fy;
        }
        forceX_ = relaxX_;
        forceY_ = relaxY_;
    };
    bool done = false;
    if (formula_) {
        // 順番は tireFormulaInputs() と同じ。
        const double in[20] = {slipRatio_, slipAngle_, load_,    tire_.mu,
                               v,          vx_,        vy_,      omega,
                               r,          tire_.bx,   tire_.cx, tire_.ex,
                               tire_.by,   tire_.cy,   tire_.ey, tire_.relaxation,
                               fxLo,       fxHi,       fyLo,     fyHi};
        double out[2] = {0.0, 0.0};
        if (formula_->eval(in, out, tickDt)) {
            forceX_ = out[0];
            forceY_ = out[1];
            done = true;
        } else {
            ++formulaFailures_;
        }
    }
    if (!done) builtin();

    // 安全のため出力にももう一度範囲を掛ける（式が守らなくても暴れない）。
    forceX_ = clampd(forceX_, fxLo, fxHi);
    forceY_ = clampd(forceY_, fyLo, fyHi);

    sumX_ += forceX_;
    sumY_ += forceY_;
    return forceX_ * r;
}

void WheelController::finishStep(int ticks) {
    const double n = ticks > 0 ? double(ticks) : 1.0;
    avgX_ = sumX_ / n;
    avgY_ = sumY_ / n;
}

Vec3 WheelController::suspensionForce() const {
    return strutUp_ * load_;
}

Vec3 WheelController::tireForce() const {
    if (!grounded_) return Vec3{};
    return forward_ * avgX_ + right_ * avgY_;
}

double WheelController::rollingTorque() const {
    return grounded_ ? tire_.rollingResistance * load_ * tire_.radius : 0.0;
}

Vec3 WheelController::wheelCenter() const {
    return attachWorld_ - strutUp_ * (sus_.restLength - compression_);
}

WheelTelemetry WheelController::telemetry() const {
    WheelTelemetry t;
    t.compression = compression_;
    t.load = load_;
    t.angularVelocity = omega_;
    t.slipRatio = slipRatio_;
    t.slipAngleDeg = radToDeg(slipAngle_);
    t.forceX = avgX_;
    t.forceY = avgY_;
    t.steerDeg = radToDeg(steerRad_);
    t.spinAngle = spinAngle_;
    t.deflection = deflection_;
    t.grounded = grounded_;
    return t;
}

}  // namespace vehicle
}  // namespace wizengine
