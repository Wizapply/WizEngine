#pragma once

// 眠り: 設定と計時。物理スレッド専用。

#include <cstddef>
#include <vector>

#include "physics/PhysicsPart.h"

namespace chrono {
class ChBody;
}

namespace wizengine {
namespace physics {

class BodyRegistry;
class PhysicsSystem;

class Sleeping : public PhysicsPart {
public:
    Sleeping(PhysicsSystem& system, BodyRegistry& bodies) : system_(system), bodies_(bodies) {}
    Sleeping(const Sleeping&) = delete;
    Sleeping& operator=(const Sleeping&) = delete;

    void setEnabled(bool enabled, float seconds, float minLinVel, float minAngVel);
    // 作ったボディへ今の設定を当てる（系へ追加する前に呼ぶ）。
    void applyTo(chrono::ChBody& body) const;
    // allow = false のボディは眠らせない。それ以外は applyTo と同じ。
    void configure(chrono::ChBody& body, bool allow) const;
    // 眠りの計時を今からにする（置き直し・固定解除・新規・全体を起こすとき）。
    void resetTimer(std::size_t id);
    // 全部のボディを起こして計時をやり直す。
    void wakeAll();
    std::size_t count() const;

    void onSystemRecreated() override;
    void onBodiesCleared() override;
    void onBodyAdded(std::size_t id, const BodyAddInfo& info) override;

private:
    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    bool enabled_ = false;
    float seconds_ = 1.0f;
    float minLinVel_ = 0.02f;
    float minAngVel_ = 0.02f;
    // 名簿の番号と並ぶ: 眠らないボディの印（allowSleeping = false で足された物）。
    std::vector<char> neverSleeps_;
};

}  // namespace physics
}  // namespace wizengine
