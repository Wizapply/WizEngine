#pragma once

// ボディ別の重力オフ: どのボディの重力を切るかの印と、毎ステップの打ち消し。
// 物理スレッド専用。

#include <cstddef>
#include <vector>

#include "physics/PhysicsPart.h"

namespace wizengine {
namespace physics {

class BodyRegistry;
class PhysicsSystem;

class GravityOverride : public PhysicsPart {
public:
    GravityOverride(PhysicsSystem& system, BodyRegistry& bodies)
        : system_(system), bodies_(bodies) {}
    GravityOverride(const GravityOverride&) = delete;
    GravityOverride& operator=(const GravityOverride&) = delete;

    // 意味は PhysicsWorld::setBodyGravity の宣言。
    void setEnabled(std::size_t id, bool enabled);
    // 印の付いたボディから、このステップの重力ぶんの速度を引く（積分の前）。
    void cancel(double dt);

    void onBodiesCleared() override;
    void onBodyAdded(std::size_t id, const BodyAddInfo& info) override;

private:
    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    // 名簿の番号と並ぶ: 重力を切る印。setEnabled が書くのは渡された番号（代表）だけ。
    std::vector<char> noGravity_;
};

}  // namespace physics
}  // namespace wizengine
