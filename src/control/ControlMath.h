#pragma once

#include <algorithm>
#include <cmath>

#include "control/ControlSignals.h"

// 制御器のセンサ値の計算（純粋な数値。Chrono も Filament も知らない）。
// ControlComponent が Chrono の値をここの型へ詰め替えて呼ぶ。
namespace wizengine {
namespace control {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }

// 単位四元数（w, x, y, z）。
struct Quat {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
};
inline Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline Quat conj(const Quat& q) { return {q.w, -q.x, -q.y, -q.z}; }
inline Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0;
    return v + t * q.w + cross(u, t);
}

// ボディの状態（ワールド。位置は Chrono のボディ原点 = 重心）。
struct BodyState {
    Vec3 p;
    Quat q;
    Vec3 v;
    Vec3 w;
};

// 姿勢のオイラー角 (rad)。R = Rz*Ry*Rx（scenemath::eulerDegreesFromQuat と
// 同じ分解。ジンバルロックでは z = 0 に寄せる）。
inline Vec3 eulerFromQuat(const Quat& q) {
    const double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    const double w = q.w / n, x = q.x / n, y = q.y / n, z = q.z / n;
    const double r00 = 1.0 - 2.0 * (y * y + z * z);
    const double r10 = 2.0 * (x * y + w * z);
    const double r20 = 2.0 * (x * z - w * y);
    const double r21 = 2.0 * (y * z + w * x);
    const double r22 = 1.0 - 2.0 * (x * x + y * y);
    const double r11 = 1.0 - 2.0 * (x * x + z * z);
    const double r12 = 2.0 * (y * z - w * x);
    Vec3 e;
    e.y = std::asin(std::max(-1.0, std::min(1.0, -r20)));
    if (std::fabs(r20) < 0.999999) {
        e.x = std::atan2(r21, r22);
        e.z = std::atan2(r10, r00);
    } else {
        e.x = std::atan2(-r12, r11);
        e.z = 0.0;
    }
    return e;
}

// body:<名前>.<量> の値。知らない量は 0。
inline double bodyQuantity(Quantity q, const BodyState& b) {
    switch (q) {
        case Quantity::X: return b.p.x;
        case Quantity::Y: return b.p.y;
        case Quantity::Z: return b.p.z;
        case Quantity::Vx: return b.v.x;
        case Quantity::Vy: return b.v.y;
        case Quantity::Vz: return b.v.z;
        case Quantity::Wx: return b.w.x;
        case Quantity::Wy: return b.w.y;
        case Quantity::Wz: return b.w.z;
        case Quantity::Rx: return eulerFromQuat(b.q).x;
        case Quantity::Ry: return eulerFromQuat(b.q).y;
        case Quantity::Rz: return eulerFromQuat(b.q).z;
        case Quantity::Speed: return length(b.v);
        case Quantity::VFwd: return dot(b.v, rotate(b.q, Vec3{0.0, 0.0, -1.0}));
        case Quantity::VLat: return dot(b.v, rotate(b.q, Vec3{1.0, 0.0, 0.0}));
        default: return 0.0;
    }
}

// ジョイントの相対運動の計測（body1 を body2 から見る。body2 が地面なら
// 単位姿勢・静止を渡す）。begin でシミュレート開始時の姿勢を基準に取り、
// 以後の measure は「基準からの」角度・変位を返す。軸はワールドの axis を
// body2 のローカルへ固定して持つ（body2 が回っても軸は一緒に回る）。
//
// 符号: 角度はモータが body1 に正のトルクを掛ける向き（Chrono の
// ChLinkMotorRotation と手計算のトルクモータの約束）で増える。
class JointMeasure {
public:
    void begin(const BodyState& b1, const BodyState& b2, const Vec3& axisWorld) {
        Vec3 a = axisWorld;
        const double n = length(a);
        a = n > 1e-12 ? a * (1.0 / n) : Vec3{0.0, 1.0, 0.0};
        axis2_ = rotate(conj(b2.q), a);
        rel0_ = conj(b2.q) * b1.q;
        d0_ = rotate(conj(b2.q), b1.p - b2.p);
        angle_ = 0.0;
        last_ = 0.0;
        started_ = true;
    }
    bool started() const { return started_; }

    struct Result {
        double angle = 0.0, rate = 0.0, pos = 0.0, vel = 0.0;
    };
    Result measure(const BodyState& b1, const BodyState& b2) {
        Result r;
        if (!started_) return r;
        // 相対回転の変化 Δ = rel * rel0^-1（body2 の座標）を軸まわりに分解
        // （swing-twist の twist）。±π で折り返すので差分を積んで連続にする。
        const Quat rel = conj(b2.q) * b1.q;
        const Quat d = rel * conj(rel0_);
        const double proj = d.x * axis2_.x + d.y * axis2_.y + d.z * axis2_.z;
        double wrapped = 2.0 * std::atan2(proj, d.w);
        const double kPi = 3.14159265358979323846;
        while (wrapped > kPi) wrapped -= 2.0 * kPi;
        while (wrapped < -kPi) wrapped += 2.0 * kPi;
        double step = wrapped - last_;
        while (step > kPi) step -= 2.0 * kPi;
        while (step < -kPi) step += 2.0 * kPi;
        angle_ += step;
        last_ = wrapped;
        const Vec3 axisW = rotate(b2.q, axis2_);
        r.angle = angle_;
        r.rate = dot(b1.w - b2.w, axisW);
        const Vec3 dl = rotate(conj(b2.q), b1.p - b2.p) - d0_;
        r.pos = dot(dl, axis2_);
        r.vel = dot(b1.v - b2.v, axisW);
        return r;
    }

private:
    Vec3 axis2_{0.0, 1.0, 0.0};
    Quat rel0_;
    Vec3 d0_;
    double angle_ = 0.0;
    double last_ = 0.0;
    bool started_ = false;
};

// ワールドのトルク τ を「重心の両側に掛ける逆向きの力の対」に直す。
// 合力 0・モーメント τ。PhysicsWorld にトルクの口が無いので、点に掛かる力
// （applyForceAtPoint）2 回で与えるための計算。arm は腕の長さ (m)。
inline void torqueAsCouple(const Vec3& tau, double arm, Vec3& offset, Vec3& force) {
    const double t = length(tau);
    if (t < 1e-12 || arm <= 0.0) {
        offset = Vec3{};
        force = Vec3{};
        return;
    }
    const Vec3 n = tau * (1.0 / t);
    // n と平行でない基準から直交する単位ベクトル a を作る。
    const Vec3 ref = std::fabs(n.x) < 0.9 ? Vec3{1.0, 0.0, 0.0} : Vec3{0.0, 1.0, 0.0};
    Vec3 a = cross(n, ref);
    a = a * (1.0 / length(a));
    const Vec3 b = cross(n, a);  // a × b = n
    offset = a * arm;
    // 点 +offset に F、-offset に -F: 2 arm (a × F) = τ → F = t / (2 arm) b
    force = b * (t / (2.0 * arm));
}

}  // namespace control
}  // namespace wizengine
