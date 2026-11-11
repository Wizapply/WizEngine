// PhysicsWorld の窓口: 構築・1 ステップの順番・全体を起こす・バックエンドの名前と機能の有無。

#include "physics/PhysicsWorldInternal.h"

#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/physics/ChBody.h>

#include <cmath>

using namespace chrono;
using namespace wizengine::physics::compat;

bool multicoreAvailable() {
#ifdef WIZ_USE_MULTICORE
    return true;
#else
    return false;
#endif
}

PhysicsFeatures physicsFeatures() {
    PhysicsFeatures f;
#ifdef WIZ_HAVE_FEA
    f.fea = true;
#endif
#ifdef WIZ_HAVE_LOADS
    f.loads = true;
#endif
#ifdef WIZ_HAVE_DIRECT_SOLVERS
    f.directSolvers = true;
#else
    f.directSolvers = false;
#endif
#ifdef WIZ_HAVE_PARDISO
    f.pardiso = true;
#endif
#ifdef WIZ_HAVE_MUMPS
    f.mumps = true;
#endif
#ifdef WIZ_HAVE_MODAL
    f.modal = true;
#endif
    return f;
}

const char* PhysicsWorld::backendName() const {
    return backend_ == PhysicsBackend::Multicore ? "multicore" : "core";
}

const char* PhysicsWorld::contactName() const {
    return contact_ == ContactMethod::SMC ? "smc" : "nsc";
}

PhysicsWorld::PhysicsWorld(PhysicsBackend backend, ContactMethod contact) {
    backend_ = backend;
    contact_ = contact;
    createSystem();
}

void PhysicsWorld::wakeAll() {
    for (auto& b : bodies_) {
        wakeUp(b.get(), 0);
        resetSleepTimer(*b);
    }
    // テレインの反力キャッシュも捨てる（シミュレートの開始・Reset で呼ばれる。
    // 前回の走行の最後の反力で次の走行の最初のステップを始めない）。
    for (std::size_t i = 0; i < terrainProxies_.size(); ++i) forgetTerrainContacts(i);
    // Waking from outside ManageSleepingBodies() changes how many bodies the
    // solver has to handle, and Chrono only rebuilds that layout in Setup().
    // Without this the "woken" bodies can stay out of the solve - i.e. they
    // would not fall after a reset.
    sys_->Setup();
}

void PhysicsWorld::step(double dt) {
    // ソフトボディのばね: 積分の前に粒子の速度へ織り込む（接触ソルバは
    // この速度を見て解く）。
    for (auto& soft : softBodies_) solveSoftSprings(soft, dt);
    // Multicore 用の手計算のモータ・ばね（Core では空）。
    applyManualJoints(dt);
    // ボディ別の重力オフ: Chrono 9 には per-body の口が無い（SetNoGravity は
    // 消えた）ので、重力ぶんを毎ステップ打ち消す（両バックエンド共通）。
    {
        const ChVector3d g = sys_->GetGravitationalAcceleration();
        if (g.Length2() > 0.0) {
            for (std::size_t i = 0; i < bodies_.size(); ++i) {
                if (options_[i].gravity || !active_[i]) continue;
                auto& b = bodies_[i];
                if (b->IsFixed() || softOf_[i] != kNoSoft) continue;
                if (b->IsSleeping()) continue;
                setLinVel(b.get(), getLinVel(b.get(), 0) - g * dt, 0);
            }
        }
    }

    // 固定物どうしのペアの掃除: 衝突系の初期化（BindAll）は最初の
    // DoStepDynamics の中で起きるので、その前後で「初期化済みなのに集合が
    // まだ」を見て 1 回だけ作る（初期化を別の呼び出しが先に済ませていても
    // 拾えるよう、前でも見る）。最初の 1 ステップだけは固定どうしの判定が
    // 走る。
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    auto staticPairsPending = [this]() {
        if (!staticPairs_ || !staticPairs_->world || staticPairs_->ready) return false;
        const auto coll = sys_->GetCollisionSystem();
        return coll && coll->IsInitialized();
    };
    if (staticPairsPending()) syncStaticPairs();
#endif

    sys_->DoStepDynamics(dt);

#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    if (staticPairsPending()) syncStaticPairs();
#endif

    logStepDiagnostics(dt);

    // 破断: 反力が閾値を超えたジョイントを外す（このステップの反力を見る）。
    checkJointBreaks();

    // Damping after the solve: scale each body's velocity towards zero. exp()
    // makes the decay frame-rate independent, so changing the physics rate
    // does not change how quickly things slow down. Sleeping bodies are left
    // alone - they are not moving anyway, and touching them would wake the
    // bookkeeping for nothing.
    if (linearDamping_ <= 0.0 && angularDamping_ <= 0.0) return;
    const double linScale = std::exp(-linearDamping_ * dt);
    const double angScale = std::exp(-angularDamping_ * dt);
    for (auto& b : bodies_) {
        if (b->IsFixed() || b->IsSleeping()) continue;
        if (linearDamping_ > 0.0) {
            setLinVel(b.get(), getLinVel(b.get(), 0) * linScale, 0);
        }
        if (angularDamping_ > 0.0) {
            setAngVel(b.get(), getAngVel(b.get(), 0) * angScale, 0);
        }
    }
}
