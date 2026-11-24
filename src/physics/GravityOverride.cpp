// ボディ別の重力オフ。

#include "physics/GravityOverride.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/PhysicsSystem.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystem.h>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wizengine {
namespace physics {

void GravityOverride::setEnabled(std::size_t id, bool enabled) {
    if (id >= bodies_.count()) return;
    noGravity_[id] = enabled ? 0 : 1;
    if (bodies_.isGroupRoot(id)) {
        for (const std::size_t p : bodies_.members(id)) {
            setUseGravity(bodies_.body(p).get(), enabled, 0);
        }
        return;
    }
    setUseGravity(bodies_.body(id).get(), enabled, 0);
    wakeUp(bodies_.body(id).get(), 0);
}

void GravityOverride::cancel(double dt) {
    // ボディ別の重力オフ: Chrono 9 には per-body の口が無い（SetNoGravity は
    // 消えた）ので、重力ぶんを毎ステップ打ち消す（両バックエンド共通）。
    const ChVector3d g = system_.sys()->GetGravitationalAcceleration();
    if (g.Length2() > 0.0) {
        for (std::size_t i = 0; i < bodies_.count(); ++i) {
            if (!noGravity_[i] || !bodies_.active(i)) continue;
            auto& b = bodies_.body(i);
            if (b->IsFixed() || bodies_.isGroupMember(i)) continue;
            if (b->IsSleeping()) continue;
            setLinVel(b.get(), getLinVel(b.get(), 0) - g * dt, 0);
        }
    }
}

void GravityOverride::onBodiesCleared() { noGravity_.clear(); }

void GravityOverride::onBodyAdded(std::size_t id, const BodyAddInfo& info) {
    (void)id;
    noGravity_.push_back(info.options.gravity ? 0 : 1);
}

}  // namespace physics
}  // namespace wizengine
