// ボディ別の重力オフ。

#include "physics/PhysicsWorld.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>

using namespace chrono;
using namespace wizengine::physics::compat;

void PhysicsWorld::setBodyGravity(std::size_t id, bool enabled) {
    if (id >= bodies_.size()) return;
    options_[id].gravity = enabled;
    if (SoftBody* soft = softOfRoot(id)) {
        for (const std::size_t p : soft->particles) setUseGravity(bodies_[p].get(), enabled, 0);
        return;
    }
    setUseGravity(bodies_[id].get(), enabled, 0);
    wakeUp(bodies_[id].get(), 0);
}
