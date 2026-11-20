// 系の作り直しの順番（担当と交互に組む）と、系の設定の中継。

#include "physics/PhysicsParts.h"
#include "core/Log.h"

#include <exception>
#include <string>

void PhysicsWorld::setMaterialCombine(CombineMode mode) {
    p_->system.setMaterialCombine(mode);
}

// 系をまるごと作り直す（バックエンドの自動切替: Multicore ⇄ Core、NSC ⇄ SMC）。
// ボディ・ジョイント・ケーブル・荷重は全部捨てる - 番号は無効になるので、
// Scene が設計値から作り直す（Scene::rebuildPhysicsWorld）。
void PhysicsWorld::recreate(PhysicsBackend backend, ContactMethod contact) {
    // 新しい部品の片付けは通知（onBodiesCleared / onSystemRecreated）で済む。ここで
    // 名指しする 5 つは既存の例外: joints と cables は出来事の受け手ではない、terrain と
    // loads は系に結び付く物（コールバック・荷重コンテナ）を古い系を壊す前に手放す、
    // diag は診断の数え直し（bodies.clear() は通知を配る側なので数に入れない。joints と
    // cables は名簿を空にする前に片付ける）。
    p_->joints.clear();
    p_->cables.clear();
    // 名簿を空にすると、番号と並ぶ担当の配列（ソフトボディの一覧など）も
    // onBodiesCleared で捨てられる（系を壊す前 = 途中で例外が出ても古い番号が残らない）。
    p_->bodies.clear();
    // clear がコールバックの持ち主を切り離し、ゴーストの番号は状態ごと捨てる
    // （フレームは残す）。Scene が作り直したあとに setTerrain を呼び直す。
    p_->terrain.clear();
    p_->loads.clear();
    p_->diag.restart();
    // 古い系を壊して頼まれた種類で組む（失敗の倒し先も同じ手順）。
    auto build = [&](PhysicsBackend kind) {
        p_->system.destroy(kind, contact);
        p_->createSystem();
    };
    bool failed = false;
    std::string failure;
    try {
        build(backend);
    } catch (const std::exception& e) {
        failed = true;
        failure = e.what();
    } catch (...) {
        failed = true;
        failure = "unknown exception";
    }
    if (failed) {
        // 頼まれた系が作れなかった: Core + 同じ接触法でもう一度作り直して続ける
        // （試したのが Core でも 1 回やり直す = 作りかけの系を残さない）。それも
        // 投げたらそのまま上へ（main の guarded が受ける）。
        const char* core = wizengine::physics::PhysicsSystem::backendName(PhysicsBackend::Core);
        if (p_->system.backend() == PhysicsBackend::Core) {
            LOGE("physics", "recreate %s failed: %s - retrying %s/%s", backendName(),
                 failure.c_str(), core, contactName());
        } else {
            LOGE("physics", "recreate %s/%s failed: %s - falling back to %s/%s", backendName(),
                 contactName(), failure.c_str(), core, contactName());
        }
        build(PhysicsBackend::Core);
    }
    LOGI("physics", "world recreated: backend=%s contact=%s", backendName(), contactName());
}

// 系を組む順番はここが唯一の定義（担当の設定が系の設定と交互に入る）。
void PhysicsWorld::Parts::createSystem() {
    system.create();

    // 固定物どうしのペアを作らせないフィルタ（Core のみ。Multicore では外す）。
    staticPairs.attach();

    // テレインの自前の接触（Core のみ）。setTerrain が呼ばれるまでは何もしない
    // （TerrainContact::collectContacts の先頭で戻る）= 呼ばないシーンの挙動は変わらない。
    terrain.attach();

    system.applyBaseSettings();

    // 眠りと共有の材質（受け手は Parts のコンストラクタ。互いの順番には依存しない）。
    // 荷重コンテナと許容差・ソルバの設定より前。
    bodies.events().systemRecreated();

    // 荷重コンテナ（ChLoad）。積分に取り込む系でだけ作る。
    // 系へ足す順（ジョイント用 → ボディ用）は残差を足す順なので変えない。
    joints.createContainer();
    loads.createContainer();

    system.applySolverSettings();
}

const char* PhysicsWorld::backendName() const { return p_->system.backendName(); }
PhysicsBackend PhysicsWorld::backend() const { return p_->system.backend(); }
const char* PhysicsWorld::contactName() const { return p_->system.contactName(); }
ContactMethod PhysicsWorld::contactMethod() const { return p_->system.contact(); }
const std::string& PhysicsWorld::solverName() const { return p_->system.solverName(); }

void PhysicsWorld::setGravity(double gx, double gy, double gz) {
    p_->system.setGravity(gx, gy, gz);
}

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
