// ボディ別の重力オフ。

#include "physics/PhysicsParts.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>

using namespace chrono;
using namespace wizengine::physics::compat;

void PhysicsWorld::setBodyGravity(std::size_t id, bool enabled) {
    if (id >= p_->bodies.count()) return;
    p_->materials.setGravityEnabled(id, enabled);
    if (SoftBody* soft = softOfRoot(id)) {
        for (const std::size_t p : p_->bodies.members(soft->root)) {
            setUseGravity(p_->bodies.body(p).get(), enabled, 0);
        }
        return;
    }
    setUseGravity(p_->bodies.body(id).get(), enabled, 0);
    wakeUp(p_->bodies.body(id).get(), 0);
}
