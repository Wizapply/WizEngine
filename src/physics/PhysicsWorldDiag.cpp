// 診断ログ: シミュレート開始直後の状態と NaN の検出。

#include "physics/PhysicsParts.h"
#include "core/Log.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactContainer.h>
#include <chrono/solver/ChIterativeSolverVI.h>
#include <chrono/solver/ChSolver.h>

#include <cmath>

using namespace chrono;
using namespace wizengine::physics::compat;

void PhysicsWorld::logStepDiagnostics(double dt) {
    // 診断: 拘束が解かれているかを最初の数ステップと 1 秒後に 1 回ずつ出す
    // （「全部が地面を抜けて落ちる」の切り分け用: 接触とリンクの数、
    // ソルバの設定）。ステップ数は removeAllJoints で戻る = シミュレート
    // 開始のたびに出る。
    ++diagSteps_;
    if (diagSteps_ == 2 || diagSteps_ == 120) {
        std::size_t activeBodies = 0;
        for (std::size_t i = 0; i < p_->bodies.count(); ++i) {
            if (p_->bodies.active(i) && !p_->bodies.body(i)->IsFixed()) ++activeBodies;
        }
        LOGI("physics",
             "diag step %d: backend=%s dt=%.4f bodies=%zu (dynamic %zu) links=%d "
             "contacts=%d joints=%zu gravity=(%.2f %.2f %.2f)",
             diagSteps_, backendName(), dt, p_->bodies.count(), activeBodies,
             linkCountOf(p_->system.sys().get(), 0),
             contactCountOf(p_->system.sys()->GetContactContainer().get(), 0), joints_.size(),
             p_->system.sys()->GetGravitationalAcceleration().x(),
             p_->system.sys()->GetGravitationalAcceleration().y(),
             p_->system.sys()->GetGravitationalAcceleration().z());
        if (auto iterative = std::dynamic_pointer_cast<ChIterativeSolverVI>(p_->system.sys()->GetSolver())) {
            LOGI("physics", "diag: iterative solver, max iterations %d",
                 iterative->GetMaxIterations());
        } else if (p_->system.sys()->GetSolver()) {
            LOGI("physics", "diag: non-iterative solver (type %d)", int(p_->system.sys()->GetSolver()->GetType()));
        } else {
            LOGW("physics", "diag: the system has NO solver");
        }
    }

    // 診断: NaN の検出。1 本の拘束が NaN を出すと反復ソルバ全体が NaN になり、
    // 拘束の付いた物が全部消える（= 落ちたように見える）。最初の 2 秒だけ
    // 見て、出たらどのジョイントの反力が NaN かを 1 回だけ出す。
    if (diagSteps_ <= 120 && !diagNanReported_) {
        std::size_t bad = 0;
        for (std::size_t i = 0; i < p_->bodies.count(); ++i) {
            if (!p_->bodies.active(i)) continue;
            const ChVector3d p = p_->bodies.body(i)->GetPos();
            const ChVector3d v = getLinVel(p_->bodies.body(i).get(), 0);
            if (!std::isfinite(p.x() + p.y() + p.z() + v.x() + v.y() + v.z())) ++bad;
        }
        if (bad > 0) {
            diagNanReported_ = true;
            LOGE("physics", "diag step %d: %zu bodies have NaN position/velocity - "
                            "the constraint solve is poisoned", diagSteps_, bad);
            for (std::size_t j = 0; j < joints_.size(); ++j) {
                double f = 0.0, t = 0.0;
                const bool ok = jointReaction(j, f, t);
                LOGE("physics", "  joint #%zu type %d: reaction %s (force %.3g torque %.3g)%s",
                     j, int(joints_[j].type),
                     ok ? (std::isfinite(f + t) ? "finite" : "NaN") : "n/a", f, t,
                     joints_[j].manual.kind != JointRec::Manual::Kind::None ? " [manual]" : "");
            }
        }
    }
}
