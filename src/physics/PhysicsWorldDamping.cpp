// 速度の減衰の設定。

#include "physics/PhysicsWorld.h"
#include "physics/ChronoCompat.h"

using namespace chrono;
using namespace wizengine::physics::compat;

void PhysicsWorld::setDamping(double linearPerSecond, double angularPerSecond) {
    linearDamping_ = linearPerSecond;
    angularDamping_ = angularPerSecond;
}
