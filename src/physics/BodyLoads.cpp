// ボディの定常荷重（力・トルク）。

#include "physics/BodyLoads.h"
#include "core/Log.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/PhysicsSystem.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystem.h>
#ifdef WIZ_HAVE_LOADS
#include <chrono/physics/ChLoadContainer.h>
#include <chrono/physics/ChLoadsBody.h>
#endif

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wizengine {
namespace physics {

void BodyLoads::createContainer() {
#ifdef WIZ_HAVE_LOADS
    if (!system_.integratesLoads()) return;
    bodyLoads_ = chrono_types::make_shared<ChLoadContainer>();
    system_.sys()->Add(bodyLoads_);
#endif
}

void BodyLoads::clear() {
    loadsOf_.clear();
    bodyLoads_.reset();
}

void BodyLoads::onBodyAdded(std::size_t id, const BodyAddInfo& info) {
    loadsOf_.emplace_back();
    attach(id, info.options);
}

// 定常荷重（<geom force torque>）を荷重コンテナへ。コンテナが無い系
// （PhysicsSystem::integratesLoads）では付けられない。そのときは Scene が先に
// Core へ切り替えているはずだが、万一のときは黙って捨てず 1 回だけ伝える。
void BodyLoads::attach(std::size_t id, const BodyOptions& options) {
    const bool hasForce = options.force.Length2() > 0.0;
    const bool hasTorque = options.torque.Length2() > 0.0;
    if (!hasForce && !hasTorque) return;
    static bool warned = false;
#ifdef WIZ_HAVE_LOADS
    if (!bodyLoads_) {
        if (!warned) {
            warned = true;
            LOGW("physics", "constant body loads need the core backend - ignored");
        }
        return;
    }
    auto& body = bodies_.body(id);
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

void BodyLoads::onBodyRetiring(std::size_t id) {
    // 荷重が付くのは Core だけなので、Multicore では一覧が空。

    // 定常荷重を先に外す（ボディの変数が系から消えた後に荷重が残ると、
    // 古いオフセットへ書き込む）。退場の通知は RemoveBody より前に出る。
#ifdef WIZ_HAVE_LOADS
    if (bodyLoads_ && !loadsOf_[id].empty()) {
        bool removed = true;
        for (auto& l : loadsOf_[id]) removed = removeLoadFrom(bodyLoads_.get(), l, 0) && removed;
        if (!removed) {
            // Remove の無い版: 全部捨てて、残っている物のぶんを入れ直す。
            clearLoads(bodyLoads_.get(), 0);
            for (std::size_t i = 0; i < loadsOf_.size(); ++i) {
                if (i == id || !bodies_.active(i)) continue;
                for (auto& l : loadsOf_[i]) bodyLoads_->Add(l);
            }
        }
        loadsOf_[id].clear();
    }
#else
    (void)id;
#endif
}

}  // namespace physics
}  // namespace wizengine
