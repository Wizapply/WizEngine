// 系の作り直しの順番（担当と交互に組む）と、系の設定の中継。

#include "physics/PhysicsWorldInternal.h"
#include "core/Log.h"

#include <chrono/physics/ChSystem.h>

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
    p_->joints.clear();
    cables_.clear();
    p_->bodies.clear();
    // ソフトボディの一覧は名簿と一緒に、系を壊す前に捨てる（途中で例外が出ても
    // 古い代表番号が残らない = 次の add が昇順を崩さない）。
    p_->soft.clear();
    // clear がコールバックの持ち主を切り離し、ゴーストの番号は状態ごと捨てる
    // （フレームは残す）。Scene が作り直したあとに setTerrain を呼び直す。
    p_->terrain.clear();
    p_->loads.clear();
    p_->diag.restart();
    p_->system.destroy(backend, contact);
    createSystem();
    LOGI("physics", "world recreated: backend=%s contact=%s", backendName(), contactName());
}

// 系を組む順番はここが唯一の定義（担当の設定が系の設定と交互に入る）。
void PhysicsWorld::createSystem() {
    p_->system.create();

    // 固定物どうしのペアを作らせないフィルタ（Core のみ。Multicore では外す）。
    p_->staticPairs.attach();

    // テレインの自前の接触（Core のみ）。setTerrain が呼ばれるまでは何もしない
    // （TerrainContact::collectContacts の先頭で戻る）= 呼ばないシーンの挙動は変わらない。
    p_->terrain.attach();

    p_->system.applyBaseSettings();

    // 眠りと共有の材質（受け手は Parts のコンストラクタ。互いの順番には依存しない）。
    // 荷重コンテナと許容差・ソルバの設定より前。
    p_->bodies.events().systemRecreated();

    // 荷重コンテナ（ChLoad）。積分に取り込む系でだけ作る。
    // 系へ足す順（ジョイント用 → ボディ用）は残差を足す順なので変えない。
    p_->joints.createContainer();
    p_->loads.createContainer();

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
