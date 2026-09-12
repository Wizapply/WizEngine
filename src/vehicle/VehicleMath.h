#pragma once

#include <cmath>

// 車両モデル専用の最小ベクトル / 四元数。
//
// なぜ自前か: src/vehicle/ は Chrono にも Filament にも依存しない純粋な
// 数値ライブラリにしてある（単体で回してテストできる、どのスレッドでも
// コピーできる）。SceneMath.h は Filament の math を、PhysicsWorld は Chrono の
// ChVector3d を使うので、ここだけ独立した型を持ち、境界（VehicleComponent）で
// 詰め替える。
namespace wizengine {
namespace vehicle {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;

    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) {
        x += o.x; y += o.y; z += o.z;
        return *this;
    }
    Vec3& operator-=(const Vec3& o) {
        x -= o.x; y -= o.y; z -= o.z;
        return *this;
    }
    Vec3& operator*=(double s) {
        x *= s; y *= s; z *= s;
        return *this;
    }

    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double length() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const {
        const double l = length();
        return l > 1e-12 ? *this / l : Vec3{};
    }
};

inline Vec3 operator*(double s, const Vec3& v) { return v * s; }

// 単位四元数（w, x, y, z）。Chrono の ChQuaternion（e0..e3）と同じ並び。
struct Quat {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;

    Quat() = default;
    Quat(double w_, double x_, double y_, double z_)
        : w(w_), x(x_), y(y_), z(z_) {}

    // 軸まわりの回転（軸は単位ベクトル、角度はラジアン）。
    static Quat fromAxisAngle(const Vec3& axis, double radians) {
        const double h = radians * 0.5;
        const double s = std::sin(h);
        return {std::cos(h), axis.x * s, axis.y * s, axis.z * s};
    }

    Quat operator*(const Quat& o) const {
        return {w * o.w - x * o.x - y * o.y - z * o.z,
                w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w};
    }
    Quat conjugate() const { return {w, -x, -y, -z}; }
    Quat normalized() const {
        const double n = std::sqrt(w * w + x * x + y * y + z * z);
        if (n < 1e-12) return {};
        return {w / n, x / n, y / n, z / n};
    }

    // ローカル → ワールド。
    Vec3 rotate(const Vec3& v) const {
        const Vec3 u{x, y, z};
        const Vec3 t = u.cross(v) * 2.0;
        return v + t * w + u.cross(t);
    }
    // ワールド → ローカル。
    Vec3 rotateBack(const Vec3& v) const { return conjugate().rotate(v); }
};

// ---- 車輪のレイ用の当たり判定 -----------------------------------------------
// 光線（origin + t*dir、dir は単位）と向き付きの箱（中心・回転・半寸法）。
// 当たれば t（0 < t <= maxDist）と外向きの法線を返す。origin が箱の中に
// あるときは false - 車体と重なった箱を「地面」と取り違えないため。
inline bool rayHitsOrientedBox(const Vec3& origin, const Vec3& dir, const Vec3& center,
                               const Quat& rot, const Vec3& half, double maxDist,
                               double& tOut, Vec3& normalOut) {
    const Vec3 o = rot.rotateBack(origin - center);
    const Vec3 d = rot.rotateBack(dir);
    const double oa[3] = {o.x, o.y, o.z};
    const double da[3] = {d.x, d.y, d.z};
    const double ha[3] = {half.x, half.y, half.z};
    if (std::fabs(oa[0]) <= ha[0] && std::fabs(oa[1]) <= ha[1] && std::fabs(oa[2]) <= ha[2]) {
        return false;  // 中から撃っている
    }
    double tMin = 0.0, tMax = maxDist;
    int axis = -1;
    double sign = 0.0;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(da[a]) < 1e-12) {
            if (oa[a] < -ha[a] || oa[a] > ha[a]) return false;
            continue;
        }
        double t1 = (-ha[a] - oa[a]) / da[a];
        double t2 = (ha[a] - oa[a]) / da[a];
        double s = -1.0;  // t1 が -half 側の面
        if (t1 > t2) {
            const double tmp = t1;
            t1 = t2;
            t2 = tmp;
            s = 1.0;
        }
        if (t1 > tMin) {
            tMin = t1;
            axis = a;
            sign = s;
        }
        if (t2 < tMax) tMax = t2;
        if (tMin > tMax) return false;
    }
    if (axis < 0 || tMin <= 0.0) return false;
    Vec3 nLocal;
    if (axis == 0) nLocal = Vec3{sign, 0.0, 0.0};
    else if (axis == 1) nLocal = Vec3{0.0, sign, 0.0};
    else nLocal = Vec3{0.0, 0.0, sign};
    tOut = tMin;
    normalOut = rot.rotate(nLocal);
    return true;
}

// 光線と球の表面。origin が球の中なら false。
inline bool rayHitsSphereSurface(const Vec3& origin, const Vec3& dir, const Vec3& center,
                                 double radius, double maxDist, double& tOut, Vec3& normalOut) {
    const Vec3 rel = center - origin;
    const double along = rel.dot(dir);
    const double perpSq = rel.dot(rel) - along * along;
    const double rSq = radius * radius;
    if (perpSq > rSq) return false;
    if (rel.dot(rel) < rSq) return false;  // 中から撃っている
    const double t = along - std::sqrt(rSq - perpSq);
    if (t <= 0.0 || t > maxDist) return false;
    tOut = t;
    normalOut = ((origin + dir * t) - center).normalized();
    return true;
}

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline double signd(double v) { return v < 0.0 ? -1.0 : (v > 0.0 ? 1.0 : 0.0); }

constexpr double kPi = 3.14159265358979323846;
inline double degToRad(double d) { return d * kPi / 180.0; }
inline double radToDeg(double r) { return r * 180.0 / kPi; }
inline double rpmToRad(double rpm) { return rpm * 2.0 * kPi / 60.0; }
inline double radToRpm(double w) { return w * 60.0 / (2.0 * kPi); }

}  // namespace vehicle
}  // namespace wizengine
