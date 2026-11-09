#pragma once

// Chrono の型の小さな幾何の計算（複数の担当が使うもの）。

#include <chrono/core/ChQuaternion.h>
#include <chrono/core/ChVector3.h>

#include <cmath>

namespace wizengine {
namespace physics {

// 回転の基準にする軸。
enum class Axis { X, Z };

// 基準軸（+X か +Z）をこの向きに合わせる回転。ChLinkLockRevolute はリンク座標系の
// Z 軸まわりに回り、ChLinkLockPrismatic は Z 軸方向にスライドするので、ユーザーが
// 指定したワールド軸をリンクの Z に持ってくる（Axis::Z）。ChLinkMotorLinear* は
// X 軸方向に動くので、直動モータだけは Axis::X。
// Chrono のベクトル演算子は版ごとに名前が違うので、成分計算で書いてある。
// 軸ごとの式は switch で明示する（汎用の外積で書くと -0 や丸めが変わる）。
inline chrono::ChQuaternion<> quatFromAxis(Axis base, const chrono::ChVector3d& axis) {
    double ax = axis.x(), ay = axis.y(), az = axis.z();
    const double n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n < 1e-9) return chrono::ChQuaternion<>(1, 0, 0, 0);
    ax /= n;
    ay /= n;
    az /= n;

    switch (base) {
        case Axis::X: {
            const double c = ax;  // dot((1,0,0), axis)
            if (c < -0.999999) {  // 真後ろ: Y 軸まわりに 180 度
                return chrono::ChQuaternion<>(0, 0, 1, 0);
            }
            // 最短回転。cross((1,0,0), axis) = (0, -az, ay)
            chrono::ChQuaternion<> q(1.0 + c, 0.0, -az, ay);
            q.Normalize();
            return q;
        }
        case Axis::Z: {
            const double c = az;  // dot((0,0,1), axis)
            if (c < -0.999999) {  // 真後ろ: X 軸まわりに 180 度
                return chrono::ChQuaternion<>(0, 1, 0, 0);
            }
            // 最短回転。cross((0,0,1), axis) = (-ay, ax, 0)
            chrono::ChQuaternion<> q(1.0 + c, -ay, ax, 0.0);
            q.Normalize();
            return q;
        }
    }
    return chrono::ChQuaternion<>(1, 0, 0, 0);  // 到達しない（軸を足したら case も足す）
}

}  // namespace physics
}  // namespace wizengine
