#pragma once

// ボディの運動: 力・速度・質量・姿勢。状態は持たない。グループ（代表番号）は
// 名簿の members で全員へ広げる。物理スレッド専用。

#include <cstddef>

#include "physics/PhysicsTypes.h"

namespace chrono {
class ChBody;
}

namespace wizengine {
namespace physics {

class BodyRegistry;
class Sleeping;

class BodyMotion {
public:
    BodyMotion(BodyRegistry& bodies, Sleeping& sleeping) : bodies_(bodies), sleeping_(sleeping) {}
    BodyMotion(const BodyMotion&) = delete;
    BodyMotion& operator=(const BodyMotion&) = delete;

    void applyForce(std::size_t id, const chrono::ChVector3d& force, double dt);
    void applyForceAtPoint(std::size_t id, const chrono::ChVector3d& force,
                           const chrono::ChVector3d& point, double dt);
    // 角力積（ワールド座標）を与える（固定・グループには何もしない）。
    void applyAngularImpulse(std::size_t id, const chrono::ChVector3d& impulse);
    chrono::ChVector3d velocity(std::size_t id) const;
    chrono::ChVector3d angularVelocity(std::size_t id) const;
    chrono::ChVector3d pointVelocity(std::size_t id, const chrono::ChVector3d& point) const;
    double mass(std::size_t id) const;
    void setVelocity(std::size_t id, const chrono::ChVector3d& linear,
                     const chrono::ChVector3d& angular);
    // 以下は 1 個の剛体の姿勢（グループの静止形状は窓口が扱う）。
    BodyTransform rigidTransform(std::size_t id) const;
    // その場に静止させて置く（落下速度を与えない）。
    void place(std::size_t id, const chrono::ChVector3d& pos, const chrono::ChQuaternion<>& rot);

private:
    // 1 個の剛体の角速度に角力積（ワールド座標）を足す（起こす・固定の判定は呼び出し側）。
    static void addAngularImpulse(chrono::ChBody& b, const chrono::ChVector3d& impulseW);

    BodyRegistry& bodies_;
    Sleeping& sleeping_;
};

}  // namespace physics
}  // namespace wizengine
