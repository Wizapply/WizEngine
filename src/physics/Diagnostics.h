#pragma once

// 診断ログ: シミュレート開始直後の状態と NaN の検出。物理スレッド専用。

#include <cstddef>

namespace wizengine {
namespace physics {

class BodyRegistry;
class PhysicsSystem;

class Diagnostics {
public:
    Diagnostics(PhysicsSystem& system, BodyRegistry& bodies) : system_(system), bodies_(bodies) {}
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    // 積分の直後に呼ぶ。この呼び出しで初めて NaN を見つけたら true
    // （呼び出し側がジョイントごとの反力を続けて出す）。
    bool afterStep(double dt, std::size_t jointCount);
    // ステップ数と NaN の報告済みの印を戻す（次のシミュレート開始で診断行を出し直す）。
    void restart();
    // restart からのステップ数。
    int steps() const { return diagSteps_; }

private:
    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    int diagSteps_ = 0;
    bool diagNanReported_ = false;
};

}  // namespace physics
}  // namespace wizengine
