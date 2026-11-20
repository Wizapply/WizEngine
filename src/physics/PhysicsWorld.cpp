// PhysicsWorld の窓口: 構築・1 ステップの順番・全体を起こす・機能の有無・小さな担当の中継。

#include "physics/PhysicsParts.h"
#include "physics/ChronoCompat.h"  // physicsFeatures の WIZ_HAVE_*
#include "physics/ContactReport.h"
#include "physics/ModalAnalysis.h"

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
    p_->createSystem();
}

// Parts が完全型のここで定義する。壊れる順は PhysicsParts.h（Parts の宣言の逆順）。
PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::wakeAll() {
    p_->sleeping.wakeAll();
    p_->bodies.events().wakeAll();
    // 起こしたボディをソルバに入れる（理由は PhysicsSystem::setup）。
    p_->system.setup();
}

void PhysicsWorld::step(double dt) {
    // ソフトボディのばね: 積分の前に粒子の速度へ織り込む（接触ソルバは
    // この速度を見て解く）。
    p_->soft.solveSprings(dt);
    // Multicore 用の手計算のモータ・ばね（Core では空）。
    p_->joints.applyManual(dt);
    p_->gravity.cancel(dt);
    p_->staticPairs.syncIfPending();
    p_->system.doStep(dt);
    p_->staticPairs.syncIfPending();
    if (p_->diag.afterStep(dt, p_->joints.count())) p_->joints.logReactions();
    // 破断: 反力が閾値を超えたジョイントを外す（このステップの反力を見る）。
    p_->joints.checkBreaks();
    p_->damping.apply(dt);
}

// ---- ジョイント（本体は Joints）---------------------------------------------

std::size_t PhysicsWorld::addJoint(JointType type, std::size_t bodyA,
                                   std::size_t bodyB, const chrono::ChVector3d& anchor,
                                   const chrono::ChVector3d& axis, double distance) {
    JointSpec spec;
    spec.type = type;
    spec.bodyA = bodyA;
    spec.bodyB = bodyB;
    spec.anchor = anchor;
    spec.axis = axis;
    spec.distance = distance;
    return addJoint(spec);
}

std::size_t PhysicsWorld::addJoint(const JointSpec& spec) { return p_->joints.add(spec); }

void PhysicsWorld::removeAllJoints() {
    p_->diag.restart();  // 次のシミュレート開始で診断行を出し直す
    p_->joints.removeAll();
}

std::size_t PhysicsWorld::jointCount() const { return p_->joints.count(); }

bool PhysicsWorld::setJointMotorTarget(std::size_t joint, double target) {
    return p_->joints.setMotorTarget(joint, target);
}

bool PhysicsWorld::jointReaction(std::size_t joint, double& force, double& torque) const {
    return p_->joints.reaction(joint, force, torque);
}

bool PhysicsWorld::jointBroken(std::size_t joint) const { return p_->joints.broken(joint); }

std::vector<std::size_t> PhysicsWorld::takeBrokenJoints() { return p_->joints.takeBroken(); }

// ---- ケーブル（本体は Cables）-----------------------------------------------

std::size_t PhysicsWorld::addCable(const CableSpec& spec) { return p_->cables.add(spec); }

void PhysicsWorld::removeAllCables() { p_->cables.removeAll(); }

std::size_t PhysicsWorld::cableCount() const { return p_->cables.count(); }

void PhysicsWorld::cableNodePositions(std::size_t cable, std::vector<float>& out) const {
    p_->cables.nodePositions(cable, out);
}

double PhysicsWorld::cableTension(std::size_t cable) const { return p_->cables.tension(cable); }

// ---- モーダル解析（本体は ModalAnalysis）------------------------------------

bool PhysicsWorld::modalFrequencies(int count, std::vector<double>& hz, std::string& why) {
    return wizengine::physics::modalFrequencies(p_->system, count, hz, why);
}

// ---- 眠り --------------------------------------------------------------------

void PhysicsWorld::setSleepingEnabled(bool enabled, float seconds,
                                      float minLinVel, float minAngVel) {
    p_->sleeping.setEnabled(enabled, seconds, minLinVel, minAngVel);
}

std::size_t PhysicsWorld::sleepingCount() const { return p_->sleeping.count(); }

// ---- ソフトボディ（本体は SoftBodies）-----------------------------------------

bool PhysicsWorld::isSoftBody(std::size_t id) const { return p_->soft.isRoot(id); }

std::size_t PhysicsWorld::softParticleCount(std::size_t id) const {
    return p_->soft.particleCount(id);
}

void PhysicsWorld::softParticlePositions(std::size_t id, std::vector<float>& out) const {
    p_->soft.particlePositions(id, out);
}

// ---- 接触の材質 --------------------------------------------------------------

void PhysicsWorld::setSmcDefaults(double young, double poisson) {
    p_->materials.setSmcDefaults(young, poisson);
}

bool PhysicsWorld::setBodySurface(std::size_t id, const BodyOptions& options) {
    return p_->materials.setBodySurface(id, options);
}

void PhysicsWorld::setSurfaceMaterial(float friction, float restitution) {
    p_->materials.setSurfaceMaterial(friction, restitution);
}

void PhysicsWorld::setRollingFriction(float rolling, float spinning) {
    p_->materials.setRollingFriction(rolling, spinning);
}

// ---- テレイン（起こすのは窓口。TerrainContact は起こす必要だけを返す）--------

std::size_t PhysicsWorld::setTerrain(std::shared_ptr<const wizengine::terrain::HeightField> field,
                                     const BodyOptions& terrainOptions) {
    if (!field) {
        clearTerrain();
        return kInvalidId;
    }
    const std::size_t id = p_->terrain.set(std::move(field), terrainOptions);
    if (id != kInvalidId) wakeAll();
    return id;
}

void PhysicsWorld::setTerrainFrame(const chrono::ChVector3d& origin,
                                   const chrono::ChQuaternion<>& rot) {
    if (p_->terrain.setFrame(origin, rot)) wakeAll();
}

void PhysicsWorld::clearTerrain() {
    if (p_->terrain.clearField()) wakeAll();
}

bool PhysicsWorld::hasTerrain() const { return p_->terrain.hasTerrain(); }

std::size_t PhysicsWorld::terrainBodyId() const { return p_->terrain.bodyId(); }

bool PhysicsWorld::hasTerrainProxy(std::size_t id) const { return p_->terrain.hasProxy(id); }

PhysicsWorld::TerrainContactShape PhysicsWorld::terrainContactShape(std::size_t id) const {
    return p_->terrain.contactShape(id);
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
