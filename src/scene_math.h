#pragma once

#include <Eigen/Geometry>

#include "CameraObject.h"
#include "PhysicsWorld.h"

// Linear algebra for the scene, on Eigen.
//
// Chrono already builds against Eigen (it is its own matrix backend), so this
// adds no dependency - it just replaces hand-written dot/cross/normalise code
// with the library's, which is both shorter and better tested.
namespace scenemath {

using Vec3 = Eigen::Vector3d;
using Quat = Eigen::Quaterniond;

// Pi from Eigen, so the value is not spelled out in several places.
#ifdef EIGEN_PI
inline constexpr double kPi = EIGEN_PI;
#else
inline constexpr double kPi = 3.14159265358979323846;
#endif

inline constexpr double radians(double degrees) {
    return degrees * kPi / 180.0;
}

// Camera basis: forward, right and up as an orthonormal frame. World up is +Y.
struct Basis {
    Vec3 eye;
    Vec3 forward;
    Vec3 right;
    Vec3 up;
    bool valid = false;
};

inline Basis cameraBasis(const CameraObject& camera) {
    Basis b;
    const auto eye = camera.eye();
    const auto target = camera.target();
    b.eye = Vec3(eye.x, eye.y, eye.z);
    const Vec3 toTarget = Vec3(target.x, target.y, target.z) - b.eye;
    if (toTarget.squaredNorm() <= 0.0) return b;  // degenerate: eye == target

    b.forward = toTarget.normalized();
    const Vec3 worldUp = Vec3::UnitY();
    const Vec3 right = b.forward.cross(worldUp);
    if (right.squaredNorm() <= 0.0) return b;  // looking straight up or down
    b.right = right.normalized();
    b.up = b.right.cross(b.forward);
    b.valid = true;
    return b;
}

// Ray through a point given in normalised device coords (x, y in [-1,1], y up).
inline Vec3 rayThrough(const Basis& basis, double ndcX, double ndcY,
                       double fovYDegrees, double aspect) {
    const double tanY = std::tan(radians(fovYDegrees) * 0.5);
    return (basis.forward + basis.right * (ndcX * tanY * aspect) +
            basis.up * (ndcY * tanY))
        .normalized();
}

// Inverse of rayThrough: where a world point lands on screen. Returns false
// when the point is behind the camera.
inline bool projectToNdc(const Basis& basis, const Vec3& point,
                         double fovYDegrees, double aspect, double& ndcX,
                         double& ndcY) {
    const Vec3 rel = point - basis.eye;
    const double z = rel.dot(basis.forward);
    if (z <= 1e-6) return false;
    const double tanY = std::tan(radians(fovYDegrees) * 0.5);
    ndcX = (rel.dot(basis.right) / z) / (tanY * aspect);
    ndcY = (rel.dot(basis.up) / z) / tanY;
    return true;
}

// Ray vs oriented box. Slab test in the box's own frame; returns the distance
// along the ray to the entry point, or a negative value on a miss.
inline double rayHitsBox(const Vec3& origin, const Vec3& dir,
                         const BodyTransform& box, double halfExtent) {
    const Quat rot(box.qw, box.qx, box.qy, box.qz);
    const Quat inv = rot.conjugate();
    const Vec3 o = inv * (origin - Vec3(box.px, box.py, box.pz));
    const Vec3 d = inv * dir;

    double tMin = 0.0, tMax = std::numeric_limits<double>::max();
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-12) {
            if (o[a] < -halfExtent || o[a] > halfExtent) return -1.0;
            continue;  // parallel to this slab and inside it
        }
        double t1 = (-halfExtent - o[a]) / d[a];
        double t2 = (halfExtent - o[a]) / d[a];
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return -1.0;
    }
    if (tMax < 0.0) return -1.0;
    return (tMin >= 0.0) ? tMin : tMax;  // inside the box: use the exit point
}

// Ray vs sphere, same convention as rayHitsBox.
inline double rayHitsSphere(const Vec3& origin, const Vec3& dir,
                            const BodyTransform& body, double radius) {
    const Vec3 rel = Vec3(body.px, body.py, body.pz) - origin;
    const double along = rel.dot(dir);
    const double perpSq = rel.squaredNorm() - along * along;
    const double rSq = radius * radius;
    if (perpSq > rSq) return -1.0;
    const double half = std::sqrt(rSq - perpSq);
    const double tNear = along - half;
    const double tFar = along + half;
    if (tFar < 0.0) return -1.0;
    return (tNear >= 0.0) ? tNear : tFar;
}

}  // namespace scenemath
