#pragma once

#include <math/mat4.h>
#include <math/vec4.h>

#include "PhysicsWorld.h"

// Build a Filament transform (column-major mat4f) from a Chrono pose.
// The rotation matrix is derived from the quaternion by hand to avoid
// depending on quaternion->matrix constructor details that vary between
// Filament versions.
inline filament::math::mat4f toFilament(const BodyTransform& b) {
    using filament::math::mat4f;
    using filament::math::float4;

    const float w = float(b.qw), x = float(b.qx),
                y = float(b.qy), z = float(b.qz);

    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    mat4f m;  // default constructs to identity
    // Columns are float4; mat4f is column-major.
    m[0] = float4{1 - 2 * (yy + zz), 2 * (xy + wz),     2 * (xz - wy),     0};
    m[1] = float4{2 * (xy - wz),     1 - 2 * (xx + zz), 2 * (yz + wx),     0};
    m[2] = float4{2 * (xz + wy),     2 * (yz - wx),     1 - 2 * (xx + yy), 0};
    m[3] = float4{float(b.px),       float(b.py),       float(b.pz),       1};
    return m;
}
