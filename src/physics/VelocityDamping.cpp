// 速度の減衰。

#include "physics/VelocityDamping.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>

#include <cmath>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wizengine {
namespace physics {

void VelocityDamping::set(double linearPerSecond, double angularPerSecond) {
    linearDamping_ = linearPerSecond;
    angularDamping_ = angularPerSecond;
}

void VelocityDamping::apply(double dt) {
    // 解いた後の減衰: 各ボディの速度をゼロへ向けて縮める。exp() で減り方が
    // フレームレートに依存しないので、物理のレートを変えても止まる速さは
    // 変わらない。眠っているボディは触らない - どうせ動いておらず、触ると
    // 意味もなく眠りの管理を起こしてしまう。
    if (linearDamping_ <= 0.0 && angularDamping_ <= 0.0) return;
    const double linScale = std::exp(-linearDamping_ * dt);
    const double angScale = std::exp(-angularDamping_ * dt);
    for (auto& b : bodies_.all()) {
        if (b->IsFixed() || b->IsSleeping()) continue;
        if (linearDamping_ > 0.0) {
            setLinVel(b.get(), getLinVel(b.get(), 0) * linScale, 0);
        }
        if (angularDamping_ > 0.0) {
            setAngVel(b.get(), getAngVel(b.get(), 0) * angScale, 0);
        }
    }
}

}  // namespace physics
}  // namespace wizengine
