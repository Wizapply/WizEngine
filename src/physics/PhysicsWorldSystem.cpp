// 系の作り直しの順番（担当と交互に組む）と、系の設定の中継。

#include "physics/PhysicsWorldInternal.h"
#include "core/Log.h"

#include <chrono/physics/ChSystem.h>
#ifdef WIZ_HAVE_LOADS
#include <chrono/physics/ChLoadContainer.h>
#endif

#include <memory>
#include <string>

using namespace chrono;
using namespace wizengine::physics::compat;

void PhysicsWorld::setMaterialCombine(CombineMode mode) {
    p_->system.setMaterialCombine(mode);
}

// 系をまるごと作り直す（B の自動切替: Multicore ⇄ Core、NSC ⇄ SMC）。
// ボディ・ジョイント・ケーブル・荷重は全部捨てる - 番号は無効になるので、
// Scene が設計値から作り直す（Scene::rebuildPhysicsWorld）。
void PhysicsWorld::recreate(PhysicsBackend backend, ContactMethod contact) {
    joints_.clear();
    brokenPending_.clear();
    cables_.clear();
    p_->bodies.clear();
    mats_.clear();
    options_.clear();
    loadsOf_.clear();
    softBodies_.clear();
    // テレインのゴーストとコールバックは系と一緒に消える。Scene が作り直した
    // あとに setTerrain を呼び直す。
    terrainProxies_.clear();
    terrain_.reset();
    terrainCallback_.reset();
    jointLoads_.reset();
    bodyLoads_.reset();
    diagSteps_ = 0;
    diagNanReported_ = false;
    p_->system.destroy(backend, contact);
    createSystem();
    LOGI("physics", "world recreated: backend=%s contact=%s", backendName(), contactName());
}

// 系を組む順番はここが唯一の定義（担当の設定が系の設定と交互に入る）。
void PhysicsWorld::createSystem() {
    p_->system.create();

    // 固定物どうしのペアを作らせないフィルタ（Core のみ。Multicore では外す）。
    attachStaticPairFilter();

    // テレインの自前の接触（Core のみ）。setTerrain が呼ばれるまでは何もしない
    // （collectTerrainContacts の先頭で戻る）= 呼ばないシーンの挙動は変わらない。
    terrainCallback_.reset();
    if (p_->system.backend() != PhysicsBackend::Multicore) {
        terrainCallback_ = std::make_shared<TerrainCallback>(this);
        p_->system.sys()->RegisterCustomCollisionCallback(terrainCallback_);
    }

    p_->system.applyBaseSettings();

    // Sleeping is off until the Scene enables it (see setSleepingEnabled).
    // Note: Chrono::Multicore does not support sleeping - the call is harmless
    // there, but sleepingCount() will simply stay at 0.
    allowSleeping(p_->system.sys().get(), sleepingEnabled_, 0);

    mat_ = makeMaterial();
    // No bounce: even a little restitution keeps settled boxes micro-bouncing.
    applySurface(*mat_, BodyOptions{});

    // 荷重コンテナ（ChLoad）。Core だけが積分に取り込むので、そちらでだけ作る。
#ifdef WIZ_HAVE_LOADS
    if (p_->system.backend() != PhysicsBackend::Multicore) {
        jointLoads_ = chrono_types::make_shared<ChLoadContainer>();
        bodyLoads_ = chrono_types::make_shared<ChLoadContainer>();
        p_->system.sys()->Add(jointLoads_);
        p_->system.sys()->Add(bodyLoads_);
    }
#endif

    p_->system.applySolverSettings();
}

const char* PhysicsWorld::backendName() const { return p_->system.backendName(); }
PhysicsBackend PhysicsWorld::backend() const { return p_->system.backend(); }
const char* PhysicsWorld::contactName() const { return p_->system.contactName(); }
ContactMethod PhysicsWorld::contactMethod() const { return p_->system.contact(); }
const std::string& PhysicsWorld::solverName() const { return p_->system.solverName(); }

void PhysicsWorld::setGravity(double gx, double gy, double gz) {
    p_->system.setGravity(gx, gy, gz);
}

void PhysicsWorld::setGravityY(double gravityY) { p_->system.setGravityY(gravityY); }

bool PhysicsWorld::setIntegrator(const std::string& name) {
    return p_->system.setIntegrator(name);
}

bool PhysicsWorld::setSolver(const std::string& name) { return p_->system.setSolver(name); }

void PhysicsWorld::setCollisionTolerances(double envelope, double margin) {
    p_->system.setCollisionTolerances(envelope, margin);
}

StepTimers PhysicsWorld::timers() const { return p_->system.timers(); }

void PhysicsWorld::setContactSettings(double recoverySpeed, double tolerance) {
    p_->system.setContactSettings(recoverySpeed, tolerance);
}

void PhysicsWorld::setContactRecoverySpeed(double recoverySpeed) {
    p_->system.setContactRecoverySpeed(recoverySpeed);
}

void PhysicsWorld::setNumThreads(int threads) { p_->system.setNumThreads(threads); }

void PhysicsWorld::setSolverIterations(int iterations) {
    p_->system.setSolverIterations(iterations);
}
