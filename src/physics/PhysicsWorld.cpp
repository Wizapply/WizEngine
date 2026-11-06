// PhysicsWorld の窓口: 構築・1 ステップの順番・全体を起こす・機能の有無・小さな担当の中継。

#include "physics/PhysicsWorldInternal.h"
#include "physics/ContactReport.h"

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
    : p_(std::make_unique<Parts>(*this, backend, contact)) {
    createSystem();
}

// Parts が完全型のここで定義する。メンバは PhysicsWorld.h の宣言の逆順に壊れる
// （p_ の持つ系はほかのメンバの後。Parts の中の順は PhysicsParts.h）。
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
    p_->gravity.cancel(dt);
    p_->staticPairs.syncIfPending();
    p_->system.doStep(dt);
    p_->staticPairs.syncIfPending();
    if (p_->diag.afterStep(dt, joints_.size())) logJointReactions();
    // 破断: 反力が閾値を超えたジョイントを外す（このステップの反力を見る）。
    checkJointBreaks();
    p_->damping.apply(dt);
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

// ---- 重力オフ・減衰・接触の報告 ----------------------------------------------

void PhysicsWorld::setBodyGravity(std::size_t id, bool enabled) {
    p_->gravity.setEnabled(id, enabled);
}

void PhysicsWorld::setDamping(double linearPerSecond, double angularPerSecond) {
    p_->damping.set(linearPerSecond, angularPerSecond);
}

std::vector<ContactInfo> PhysicsWorld::activeContacts() const {
    return wizengine::physics::activeContacts(p_->system, p_->bodies);
}

std::vector<std::pair<std::size_t, std::size_t>> PhysicsWorld::activeContactPairs() const {
    return wizengine::physics::activeContactPairs(p_->system, p_->bodies);
}
