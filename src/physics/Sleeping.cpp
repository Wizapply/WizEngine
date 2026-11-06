// 眠り: 設定と計時。

#include "physics/Sleeping.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/PhysicsSystem.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystem.h>

#include <algorithm>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace {

// Minimum downward speed given to a re-dropped body (m/s). The actual value
// also scales with the configured sleep threshold, so the two cannot drift out
// of sync if the Scene tightens the limits.
constexpr double kWakeSpeed = 0.1;

}  // namespace

namespace wizengine {
namespace physics {

void Sleeping::setEnabled(bool enabled, float seconds, float minLinVel, float minAngVel) {
    enabled_ = enabled;
    seconds_ = seconds;
    minLinVel_ = minLinVel;
    minAngVel_ = minAngVel;

    allowSleeping(system_.sys().get(), enabled, 0);
    for (std::size_t i = 0; i < bodies_.count(); ++i) {
        auto& b = bodies_.body(i);
        // ソフトボディの粒子は常に起きている（addSoftBody 参照）。
        if (neverSleeps_[i]) {
            allowSleeping(b.get(), false, 0);
            continue;
        }
        applyTo(*b);
        if (!enabled) wakeUp(b.get(), 0);
    }
}

void Sleeping::applyTo(ChBody& body) const {
    allowSleeping(&body, enabled_, 0);
    if (enabled_) {
        body.SetSleepTime(seconds_);
        setSleepLimits(&body, minLinVel_, minAngVel_, 0);
    }
}

std::size_t Sleeping::count() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < bodies_.count(); ++i) {
        if (!bodies_.active(i)) continue;  // 退場済みは数えない
        if (bodies_.body(i)->IsSleeping()) ++n;
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
void Sleeping::resetTimer(std::size_t id) {
    ChBody& b = *bodies_.body(id);
    if (!enabled_ || !b.IsSleepingAllowed() || b.IsFixed() || b.IsSleeping()) return;
    // 項目の時計は Update() でしか進まないので、作ったばかりのボディは 0。
    setItemTime(&b, system_.sys()->GetChTime(), 0);
    const ChVector3d v = getLinVel(&b, 0);
    setLinVel(&b, ChVector3d(1e6, 0, 0), 0);
    trySleeping(&b, 0);  // 速度が大きい = 眠れない = sleep_starttime を今にする
    setLinVel(&b, v, 0);
}

void Sleeping::wakeAll() {
    for (std::size_t i = 0; i < bodies_.count(); ++i) {
        wakeUp(bodies_.body(i).get(), 0);
        resetTimer(i);
    }
}

double Sleeping::wakeSpeed() const {
    return std::max(kWakeSpeed, 3.0 * static_cast<double>(minLinVel_));
}

void Sleeping::onSystemRecreated() {
    neverSleeps_.clear();
    // Sleeping is off until the Scene enables it (see setEnabled).
    // Note: Chrono::Multicore does not support sleeping - the call is harmless
    // there, but sleepingCount() will simply stay at 0.
    allowSleeping(system_.sys().get(), enabled_, 0);
}

void Sleeping::onBodyAdded(std::size_t id, const BodyAddInfo& info) {
    (void)id;
    neverSleeps_.push_back(info.allowSleeping ? 0 : 1);
}

}  // namespace physics
}  // namespace wizengine
