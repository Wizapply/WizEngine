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
// 半サイズは軸ごとに指定する（エディタで置いた箱は縦横高さが別々のため）。
inline double rayHitsBox(const Vec3& origin, const Vec3& dir,
                         const BodyTransform& box, const Vec3& halfExtent) {
    const Quat rot(box.qw, box.qx, box.qy, box.qz);
    const Quat inv = rot.conjugate();
    const Vec3 o = inv * (origin - Vec3(box.px, box.py, box.pz));
    const Vec3 d = inv * dir;

    double tMin = 0.0, tMax = std::numeric_limits<double>::max();
    for (int a = 0; a < 3; ++a) {
        const double h = halfExtent[a];
        if (std::abs(d[a]) < 1e-12) {
            if (o[a] < -h || o[a] > h) return -1.0;
            continue;  // parallel to this slab and inside it
        }
        double t1 = (-h - o[a]) / d[a];
        double t2 = (h - o[a]) / d[a];
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return -1.0;
    }
    if (tMax < 0.0) return -1.0;
    return (tMin >= 0.0) ? tMin : tMax;  // inside the box: use the exit point
}

// 立方体むけの短縮形。
inline double rayHitsBox(const Vec3& origin, const Vec3& dir,
                         const BodyTransform& box, double halfExtent) {
    return rayHitsBox(origin, dir, box,
                      Vec3(halfExtent, halfExtent, halfExtent));
}

// 光線と水平面 y = height の交点までの距離。エディタで「地面のここに置く」を
// 決めるのに使う。平行または後方なら負。
inline double rayHitsGround(const Vec3& origin, const Vec3& dir, double height) {
    if (std::abs(dir.y()) < 1e-9) return -1.0;
    const double t = (height - origin.y()) / dir.y();
    return (t > 0.0) ? t : -1.0;
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

// 任意の平面との交点までの距離。normal は単位ベクトルでなくてもよい。
// 平面と平行、または後ろ側なら負。
inline double rayHitsPlane(const Vec3& origin, const Vec3& dir,
                           const Vec3& planePoint, const Vec3& normal) {
    const double denom = dir.dot(normal);
    if (std::abs(denom) < 1e-9) return -1.0;
    const double t = (planePoint - origin).dot(normal) / denom;
    return (t > 0.0) ? t : -1.0;
}

// 直線（point + s*axis）と光線（origin + t*dir）が最も近づく点の、直線側の
// パラメータ s。ギズモの軸ハンドルを引っぱるときの「軸に沿った移動量」は
// これで出す。ほぼ平行なときは false（そのまま使うと発散する）。
inline bool closestOnAxis(const Vec3& point, const Vec3& axis,
                          const Vec3& origin, const Vec3& dir, double& s) {
    const Vec3 w0 = point - origin;
    const double b = axis.dot(dir);
    const double denom = 1.0 - b * b;  // axis も dir も単位ベクトル
    if (std::abs(denom) < 1e-4) return false;  // 視線と軸がほぼ重なっている
    s = (b * dir.dot(w0) - axis.dot(w0)) / denom;
    return true;
}

// 2D の点-線分距離。ハンドルの当たり判定は NDC 上で行う。解像度に依存せず、
// 「画面上でこのくらい近ければ掴んだ」という感覚のまま書けるため。
inline double distanceToSegment2D(double px, double py, double ax, double ay,
                                  double bx, double by) {
    const double dx = bx - ax, dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    double t = 0.0;
    if (len2 > 1e-12) {
        t = ((px - ax) * dx + (py - ay) * dy) / len2;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    }
    const double qx = ax + dx * t, qy = ay + dy * t;
    return std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
}

// ---- オイラー角（度, X→Y→Z）と四元数 --------------------------------------
// エディタは姿勢をオイラー角で持ち、物理と描画は四元数で受け取る。変換の
// 向き（R = Rz*Ry*Rx）はここが唯一の定義。インスペクタの数字・ギズモの回転・
// Chrono に渡す姿勢がずれないよう、必ずこの 2 つを通すこと。
inline Quat quatFromEulerDegrees(double xDeg, double yDeg, double zDeg) {
    return Quat(Eigen::AngleAxisd(radians(zDeg), Vec3::UnitZ()) *
                Eigen::AngleAxisd(radians(yDeg), Vec3::UnitY()) *
                Eigen::AngleAxisd(radians(xDeg), Vec3::UnitX()));
}

inline Vec3 eulerDegreesFromQuat(const Quat& q) {
    const Eigen::Matrix3d m = q.normalized().toRotationMatrix();
    const double r00 = m(0, 0), r10 = m(1, 0);
    const double r20 = m(2, 0), r21 = m(2, 1), r22 = m(2, 2);

    const double clamped = std::max(-1.0, std::min(1.0, -r20));
    const double y = std::asin(clamped);
    double x, z;
    if (std::abs(r20) < 0.999999) {
        x = std::atan2(r21, r22);
        z = std::atan2(r10, r00);
    } else {
        // ジンバルロック（真上/真下を向いた）。Z を 0 に決めて X に寄せる。
        x = std::atan2(-m(1, 2), m(1, 1));
        z = 0.0;
    }
    const double toDeg = 180.0 / kPi;
    return Vec3(x * toDeg, y * toDeg, z * toDeg);
}

// ---- ライトの向き -----------------------------------------------------------
// ライトは「回転ゼロ = 真下 (0,-1,0)」。rotation（オイラー角・度）から方向
// ベクトルへ、また既存設定の方向ベクトルから rotation へ（初期値の取り込み用）。
inline Vec3 lightDirection(double xDeg, double yDeg, double zDeg) {
    return quatFromEulerDegrees(xDeg, yDeg, zDeg) * Vec3(0.0, -1.0, 0.0);
}
inline Vec3 eulerDegreesFromDirection(const Vec3& dir) {
    const double len = dir.norm();
    if (len < 1e-9) return Vec3(0.0, 0.0, 0.0);
    const Quat q = Quat::FromTwoVectors(Vec3(0.0, -1.0, 0.0), dir / len);
    return eulerDegreesFromQuat(q);
}

}  // namespace scenemath
