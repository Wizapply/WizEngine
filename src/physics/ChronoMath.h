#pragma once

// Chrono の型の小さな幾何の計算（複数の担当が使うもの）。

#include <chrono/core/ChQuaternion.h>
#include <chrono/core/ChVector3.h>

#include <cmath>

namespace wizengine {
namespace physics {

// +Z をこの向きに合わせる回転。ChLinkLockRevolute はリンク座標系の Z 軸まわり
// に回り、ChLinkLockPrismatic は Z 軸方向にスライドするので、ユーザーが指定した
// ワールド軸をリンクの Z に持ってくる必要がある。
// Chrono のベクトル演算子は版ごとに名前が違うので、成分計算で書いてある。
inline chrono::ChQuaternion<> quatFromZAxis(const chrono::ChVector3d& axis) {
    double ax = axis.x(), ay = axis.y(), az = axis.z();
    const double n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n < 1e-9) return chrono::ChQuaternion<>(1, 0, 0, 0);
    ax /= n;
    ay /= n;
    az /= n;

    const double c = az;  // dot((0,0,1), axis)
    if (c < -0.999999) {  // 真後ろ: X 軸まわりに 180 度
        return chrono::ChQuaternion<>(0, 1, 0, 0);
    }
    // 最短回転。cross((0,0,1), axis) = (-ay, ax, 0)
    chrono::ChQuaternion<> q(1.0 + c, -ay, ax, 0.0);
    q.Normalize();
    return q;
}

}  // namespace physics
}  // namespace wizengine
