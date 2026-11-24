#pragma once

#include <cmath>

// 流体（水面の浮力・抗力と、流体粒子の初期配置）専用の最小ベクトル / 四元数。
//
// なぜ自前か: src/fluid/ は Chrono にも Filament にも依存しない純粋な数値の
// ライブラリにしてある（vehicle/ と同じ方針 = 単体で回してテストでき、どの
// スレッドでもコピーできる）。境界（FluidComponent・PhysicsWorld）で詰め替える。
namespace wizengine {
namespace fluid {

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
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double length() const { return std::sqrt(dot(*this)); }
};

inline Vec3 operator*(double s, const Vec3& v) { return v * s; }

// 単位四元数（w, x, y, z）。Chrono の ChQuaternion（e0..e3）と同じ並び。
struct Quat {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;

    Quat() = default;
    Quat(double w_, double x_, double y_, double z_) : w(w_), x(x_), y(y_), z(z_) {}

    // ローカル → ワールド。
    Vec3 rotate(const Vec3& v) const {
        const Vec3 u{x, y, z};
        const Vec3 t = u.cross(v) * 2.0;
        return v + t * w + u.cross(t);
    }
    // ワールド → ローカル。
    Vec3 rotateBack(const Vec3& v) const { return Quat{w, -x, -y, -z}.rotate(v); }
};

constexpr double kPi = 3.14159265358979323846;

}  // namespace fluid
}  // namespace wizengine
