#pragma once

#include <cmath>

// 車両モデル専用の最小ベクトル / 四元数。
//
// なぜ自前か: src/vehicle/ は Chrono にも Filament にも依存しない純粋な
// 数値ライブラリにしてある（単体で回してテストできる、どのスレッドでも
// コピーできる）。scene_math.h は Filament の math を、PhysicsWorld は Chrono の
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
