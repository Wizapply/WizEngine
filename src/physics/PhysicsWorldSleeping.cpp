// 眠り: 設定と計時。

#include "physics/PhysicsWorld.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>

using namespace chrono;
using namespace wizengine::physics::compat;

void PhysicsWorld::setSleepingEnabled(bool enabled, float seconds,
                                      float minLinVel, float minAngVel) {
    sleepingEnabled_ = enabled;
    sleepSeconds_ = seconds;
    sleepMinLinVel_ = minLinVel;
    sleepMinAngVel_ = minAngVel;

    allowSleeping(sys_.get(), enabled, 0);
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        auto& b = bodies_[i];
        // ソフトボディの粒子は常に起きている（addSoftBody 参照）。
        if (softOf_[i] != kNoSoft) {
            allowSleeping(b.get(), false, 0);
            continue;
        }
        allowSleeping(b.get(), enabled, 0);
        if (!enabled) {
            wakeUp(b.get(), 0);
            continue;
        }
        b->SetSleepTime(seconds);
        setSleepLimits(b.get(), minLinVel, minAngVel, 0);
    }
}

std::size_t PhysicsWorld::sleepingCount() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        if (!active_[i]) continue;  // 退場済みは数えない
        if (bodies_[i]->IsSleeping()) ++n;
    }
    return n;
}

// 眠りの計時をやり直す（Core。Multicore は眠らない）。
// Chrono の sleep_starttime（protected）は「最後に速度がしきい値を超えた
// 時刻」で、TrySleeping() の中でしか更新されない。置き直して静止させた
// ボディや作ったばかりのボディはこれが前回の走行のまま（新規は 0）なので、
// 最初のステップの ManageSleepingBodies（積分の前に走る）が
// (now - starttime) > sleep_time を満たし、宙に浮いたまま眠る（Core の実機で
// 確認: シミュレート開始で一部の物がその場で止まる）。しきい値を超える
// 速度を一瞬与えて TrySleeping を呼び、時刻を今にしてから速度を戻す。
void PhysicsWorld::resetSleepTimer(ChBody& b) {
    if (!sleepingEnabled_ || !b.IsSleepingAllowed() || b.IsFixed() || b.IsSleeping()) return;
    // 項目の時計は Update() でしか進まないので、作ったばかりのボディは 0。
    setItemTime(&b, sys_->GetChTime(), 0);
    const ChVector3d v = getLinVel(&b, 0);
    setLinVel(&b, ChVector3d(1e6, 0, 0), 0);
    trySleeping(&b, 0);  // 速度が大きい = 眠れない = sleep_starttime を今にする
    setLinVel(&b, v, 0);
}
