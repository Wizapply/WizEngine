// 診断ログ: シミュレート開始直後の状態と NaN の検出。

#include "physics/Diagnostics.h"
#include "core/Log.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/PhysicsSystem.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactContainer.h>
#include <chrono/physics/ChSystem.h>
#include <chrono/solver/ChIterativeSolverVI.h>
#include <chrono/solver/ChSolver.h>

#include <cmath>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wizengine {
namespace physics {

bool Diagnostics::afterStep(double dt, std::size_t jointCount) {
    // 診断: 拘束が解かれているかを最初の数ステップと 1 秒後に 1 回ずつ出す
    // （「全部が地面を抜けて落ちる」の切り分け用: 接触とリンクの数、
    // ソルバの設定）。ステップ数は removeAllJoints で戻る = シミュレート
    // 開始のたびに出る。
    ++diagSteps_;
    if (diagSteps_ == 2 || diagSteps_ == 120) {
        std::size_t activeBodies = 0;
        for (std::size_t i = 0; i < bodies_.count(); ++i) {
            if (bodies_.active(i) && !bodies_.body(i)->IsFixed()) ++activeBodies;
        }
        LOGI("physics",
             "diag step %d: backend=%s dt=%.4f bodies=%zu (dynamic %zu) links=%d "
             "contacts=%d joints=%zu gravity=(%.2f %.2f %.2f)",
             diagSteps_, system_.backendName(), dt, bodies_.count(), activeBodies,
             linkCountOf(system_.sys().get(), 0),
             contactCountOf(system_.sys()->GetContactContainer().get(), 0), jointCount,
             system_.sys()->GetGravitationalAcceleration().x(),
             system_.sys()->GetGravitationalAcceleration().y(),
             system_.sys()->GetGravitationalAcceleration().z());
        if (auto iterative = std::dynamic_pointer_cast<ChIterativeSolverVI>(system_.sys()->GetSolver())) {
            LOGI("physics", "diag: iterative solver, max iterations %d",
                 iterative->GetMaxIterations());
        } else if (system_.sys()->GetSolver()) {
            LOGI("physics", "diag: non-iterative solver (type %d)", int(system_.sys()->GetSolver()->GetType()));
        } else {
            LOGW("physics", "diag: the system has NO solver");
        }
    }

    // 診断: NaN の検出。1 本の拘束が NaN を出すと反復ソルバ全体が NaN になり、
    // 拘束の付いた物が全部消える（= 落ちたように見える）。最初の 2 秒だけ
    // 見て、出たらどのジョイントの反力が NaN かを 1 回だけ出す。
    if (diagSteps_ <= 120 && !diagNanReported_) {
        std::size_t bad = 0;
        for (std::size_t i = 0; i < bodies_.count(); ++i) {
            if (!bodies_.active(i)) continue;
            const ChVector3d p = bodies_.body(i)->GetPos();
            const ChVector3d v = getLinVel(bodies_.body(i).get(), 0);
            if (!std::isfinite(p.x() + p.y() + p.z() + v.x() + v.y() + v.z())) ++bad;
        }
        if (bad > 0) {
            diagNanReported_ = true;
            LOGE("physics", "diag step %d: %zu bodies have NaN position/velocity - "
                            "the constraint solve is poisoned", diagSteps_, bad);
            return true;
        }
    }
    return false;
}

void Diagnostics::restart() {
    diagSteps_ = 0;
    diagNanReported_ = false;
}

}  // namespace physics
}  // namespace wizengine
