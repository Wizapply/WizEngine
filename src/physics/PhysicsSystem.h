#pragma once

// Chrono の系（ChSystem）と、作り直しで当て直す設定の控えを持つ。物理スレッドが
// 触る。例外は名前の取得（下の注記）。
// 系を組む順番は他の担当と交互になるので、create / applyBaseSettings /
// applySolverSettings に分けてあり、呼ぶ順番は PhysicsWorld::createSystem が決める。

#include <memory>
#include <string>

#include "physics/PhysicsTypes.h"

namespace chrono {
class ChSystem;
}

namespace wizengine {
namespace physics {

class PhysicsSystem {
public:
    PhysicsSystem(PhysicsBackend backend, ContactMethod contact);
    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    const std::shared_ptr<chrono::ChSystem>& sys() const { return sys_; }

    PhysicsBackend backend() const { return backend_; }
    ContactMethod contact() const { return contact_; }
    // Chrono の荷重コンテナを積分に取り込む系か（Multicore は IntLoadResidual_F を使わない）。
    bool integratesLoads() const { return backend_ != PhysicsBackend::Multicore; }
    // backendName / contactName / solverName は HTTP スレッド（/scene、/stats）からも
    // ロック無しで読まれる（作り直しや切替の最中と重なりうる。元からの扱い）。
    const char* backendName() const;
    const char* contactName() const;
    const std::string& solverName() const { return solverName_; }

    // 古い系を壊し、次の create で組む種類を決める。
    void destroy(PhysicsBackend backend, ContactMethod contact);
    // 系を作る（Multicore がビルドに無ければ Core へ倒す）。設定はまだ当てない。
    void create();
    // 重力・反復回数・スレッド数を当てる。
    void applyBaseSettings();
    // 許容差・接触・合成方式・積分器・ソルバを当てる。
    void applySolverSettings();

    // 1 ステップ積分する。
    void doStep(double dt);

    void setGravity(double gx, double gy, double gz);
    void setGravityY(double gravityY);
    bool setIntegrator(const std::string& name);
    bool setSolver(const std::string& name);
    void setMaterialCombine(CombineMode mode);
    void setCollisionTolerances(double envelope, double margin);
    void setContactSettings(double recoverySpeed, double tolerance);
    void setContactRecoverySpeed(double recoverySpeed);
    void setNumThreads(int threads);
    void setSolverIterations(int iterations);
    StepTimers timers() const;

private:
    // ChSystemNSC / ChSystemSMC（Core）か ChSystemMulticoreNSC / SMC（Multicore）。
    // どれにするかは実行時に Scene が文書の中身から選ぶ（Multicore はビルドに
    // WIZ_USE_MULTICORE があるときだけ）。上の層は ChSystem の派生の型を直接は触らない。
    std::shared_ptr<chrono::ChSystem> sys_;
    PhysicsBackend backend_ = PhysicsBackend::Core;
    ContactMethod contact_ = ContactMethod::NSC;
    // recreate で当て直すための設定の控え。
    double envelope_ = 0.002;
    double margin_ = 0.001;
    double recoverySpeed_ = 0.2;
    double contactTolerance_ = 1e-3;
    int threads_ = 0;
    int iterations_ = 150;
    chrono::ChVector3d gravity_{0.0, -9.81, 0.0};
    CombineMode combine_ = CombineMode::Min;
    std::string solverName_ = "bb";
    std::string integratorName_ = "euler";
};

}  // namespace physics
}  // namespace wizengine
