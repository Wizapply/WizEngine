// ボディの定常荷重（力・トルク）。

#include "physics/PhysicsParts.h"
#include "core/Log.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>
#ifdef WIZ_HAVE_LOADS
#include <chrono/physics/ChLoadContainer.h>
#include <chrono/physics/ChLoadsBody.h>
#endif

using namespace chrono;
using namespace wizengine::physics::compat;

// 定常荷重（<geom force torque>）。荷重コンテナ経由 = Core だけが積分に
// 取り込む（Multicore は IntLoadResidual_F を使わない。ばねと同じ）。
// Multicore で頼まれたら Scene が先に Core へ切り替えているはずだが、
// 万一のときは黙って捨てず 1 回だけ伝える。
void PhysicsWorld::attachBodyLoads(std::size_t id, const BodyOptions& options) {
    const bool hasForce = options.force.Length2() > 0.0;
    const bool hasTorque = options.torque.Length2() > 0.0;
    if (!hasForce && !hasTorque) return;
    static bool warned = false;
#ifdef WIZ_HAVE_LOADS
    if (p_->system.backend() == PhysicsBackend::Multicore || !bodyLoads_) {
        if (!warned) {
            warned = true;
            LOGW("physics", "constant body loads need the core backend - ignored");
        }
        return;
    }
    auto& body = p_->bodies.body(id);
    if (hasForce) {
        auto f = chrono_types::make_shared<ChLoadBodyForce>(body, options.force,
                                                            /*local_force*/ false, VNULL,
                                                            /*local_point*/ true);
        bodyLoads_->Add(f);
        loadsOf_[id].push_back(f);
    }
    if (hasTorque) {
        auto t = chrono_types::make_shared<ChLoadBodyTorque>(body, options.torque,
                                                             /*local_torque*/ false);
        bodyLoads_->Add(t);
        loadsOf_[id].push_back(t);
    }
#else
    if (!warned) {
        warned = true;
        LOGW("physics", "this Chrono build has no ChLoad - constant body loads are ignored");
    }
#endif
}
