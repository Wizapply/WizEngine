// PhysicsWorld の窓口: 構築・1 ステップの順番・全体を起こす・機能の有無・眠りと材質の中継。

#include "physics/PhysicsWorldInternal.h"

#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/physics/ChBody.h>

#include <cmath>

using namespace chrono;
using namespace wizengine::physics::compat;

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

PhysicsWorld::PhysicsWorld(PhysicsBackend backend, ContactMethod contact)
    : p_(std::make_unique<Parts>(backend, contact)) {
    createSystem();
}

// Parts が完全型のここで定義する。メンバは PhysicsWorld.h の宣言の逆順に壊れる
// （p_ の持つ系はほかのメンバの後、staticPairs_ は系の後）。
PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::wakeAll() {
    p_->sleeping.wakeAll();
    // テレインの反力キャッシュも捨てる（シミュレートの開始・Reset で呼ばれる。
    // 前回の走行の最後の反力で次の走行の最初のステップを始めない）。
    for (std::size_t i = 0; i < terrainProxies_.size(); ++i) forgetTerrainContacts(i);
    // Waking from outside ManageSleepingBodies() changes how many bodies the
    // solver has to handle, and Chrono only rebuilds that layout in Setup().
    // Without this the "woken" bodies can stay out of the solve - i.e. they
    // would not fall after a reset.
    p_->system.sys()->Setup();
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
        const ChVector3d g = p_->system.sys()->GetGravitationalAcceleration();
        if (g.Length2() > 0.0) {
            for (std::size_t i = 0; i < p_->bodies.count(); ++i) {
                if (p_->materials.options(i).gravity || !p_->bodies.active(i)) continue;
                auto& b = p_->bodies.body(i);
                if (b->IsFixed() || p_->bodies.isGroupMember(i)) continue;
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
        const auto coll = p_->system.sys()->GetCollisionSystem();
        return coll && coll->IsInitialized();
    };
    if (staticPairsPending()) syncStaticPairs();
#endif

    p_->system.sys()->DoStepDynamics(dt);

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
    for (auto& b : p_->bodies.all()) {
        if (b->IsFixed() || b->IsSleeping()) continue;
        if (linearDamping_ > 0.0) {
            setLinVel(b.get(), getLinVel(b.get(), 0) * linScale, 0);
        }
        if (angularDamping_ > 0.0) {
            setAngVel(b.get(), getAngVel(b.get(), 0) * angScale, 0);
        }
    }
}

// ---- 眠り --------------------------------------------------------------------

void PhysicsWorld::setSleepingEnabled(bool enabled, float seconds,
                                      float minLinVel, float minAngVel) {
    p_->sleeping.setEnabled(enabled, seconds, minLinVel, minAngVel);
}

std::size_t PhysicsWorld::sleepingCount() const { return p_->sleeping.count(); }

// ---- 接触の材質（シーン設定を変えたらテレインの材質も書き直す）--------------

void PhysicsWorld::setSmcDefaults(double young, double poisson) {
    if (p_->materials.setSmcDefaults(young, poisson)) refreshTerrainMaterials();
}

bool PhysicsWorld::setBodySurface(std::size_t id, const BodyOptions& options) {
    return p_->materials.setBodySurface(id, options);
}

void PhysicsWorld::setSurfaceMaterial(float friction, float restitution) {
    p_->materials.setSurfaceMaterial(friction, restitution);
    refreshTerrainMaterials();
}

void PhysicsWorld::setRollingFriction(float rolling, float spinning) {
    p_->materials.setRollingFriction(rolling, spinning);
    refreshTerrainMaterials();
}
