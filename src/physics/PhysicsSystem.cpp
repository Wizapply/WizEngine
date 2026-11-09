// Chrono の系と設定: 系の生成・重力・積分器・ソルバ・衝突の許容差・スレッド数・材質の合成方式。

#include "physics/PhysicsSystem.h"
#include "physics/ChronoCompat.h"
#include "core/Log.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/physics/ChContactMaterial.h>  // 材質の合成方式
#include <chrono/physics/ChLinkBase.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/physics/ChSystemSMC.h>
#include <chrono/solver/ChIterativeSolver.h>
#include <chrono/solver/ChIterativeSolverVI.h>
#include <chrono/solver/ChSolver.h>
#include <chrono/timestepper/ChTimestepper.h>
#ifdef WIZ_HAVE_DIRECT_SOLVERS
#include <chrono/solver/ChDirectSolverLS.h>
#endif
#ifdef WIZ_HAVE_ADMM
#include <chrono/solver/ChSolverADMM.h>
#endif
#ifdef WIZ_HAVE_PARDISO
#include <chrono_pardisomkl/ChSolverPardisoMKL.h>
#endif
#ifdef WIZ_HAVE_MUMPS
#include <chrono_mumps/ChSolverMumps.h>
#endif

#ifdef WIZ_USE_MULTICORE
// Chrono::Multicore: OpenMP で並列のソルバ（APGD）と衝突判定。
#include <chrono_multicore/physics/ChSystemMulticore.h>
#endif

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace {

// Multicore の狭域判定がまともに動く最小の接触エンベロープ（m）。
constexpr double kMulticoreMinEnvelope = 0.01;

// 積分器の種類（名前は editor::integratorNameValid の語彙）。hht / newmark は
// 滑らかな系（FEA・SMC）向けの陰的な積分器。
bool timestepperFromName(const std::string& name, chrono::ChTimestepper::Type& out) {
    using T = chrono::ChTimestepper::Type;
    if (name == "euler") { out = T::EULER_IMPLICIT_LINEARIZED; return true; }
    if (name == "projected") { out = T::EULER_IMPLICIT_PROJECTED; return true; }
    if (name == "implicit") { out = T::EULER_IMPLICIT; return true; }
    if (name == "trapezoidal") { out = T::TRAPEZOIDAL_LINEARIZED; return true; }
    if (name == "hht") { out = T::HHT; return true; }
    if (name == "newmark") { out = T::NEWMARK; return true; }
    return false;
}
// SetSolverType で作れるソルバ。反復の VI（bb / apgd / psor / jacobi と、
// 剛性行列も扱える pminres）と、線形ソルバの minres（Chrono 9 では
// ChIterativeSolverLS 側 = 片側拘束を解けない。専用ヘッダは無いので種類名で
// 作る）。admm と直接法（sparselu / sparseqr / pardiso / mumps）は setSolver が
// オブジェクトを作って渡す。
//
// **FEA（ケーブル）があるときの注意**: bb は剛性行列があると例外を投げる
// （"Do NOT use Barzilai-Borwein solver if there are stiffness matrices"、
// 実機で確認）。apgd / psor / jacobi は Schur 補元で解くので剛性を黙って
// 無視する。剛性と NSC の接触を同時に解けるのは admm と pminres だけ。
bool solverFromName(const std::string& name, chrono::ChSolver::Type& out) {
    using T = chrono::ChSolver::Type;
    if (name == "bb") { out = T::BARZILAIBORWEIN; return true; }
    if (name == "apgd") { out = T::APGD; return true; }
    if (name == "psor") { out = T::PSOR; return true; }
    if (name == "jacobi") { out = T::PJACOBI; return true; }
    if (name == "pminres") { out = T::PMINRES; return true; }
    if (name == "minres") { out = T::MINRES; return true; }
    return false;
}
bool solverNameIsDirect(const std::string& s) {
    return s == "sparselu" || s == "sparseqr" || s == "pardiso" || s == "mumps";
}
// 線形（LS）ソルバ = 片側拘束（NSC の接触・可動範囲）を解けないもの。
bool solverNameIsLinear(const std::string& s) {
    return solverNameIsDirect(s) || s == "minres";
}

// 2 材質の合成方式。Chrono の既定は min（滑りやすい方が勝つ）。average /
// max にすると氷の上のゴムのような組み合わせの手触りが変わる。
// メソッド名は Chrono 9.0 の ChContactMaterialCompositionStrategy に合わせて
// ある。override は付けない: 版によって無い仮想関数（CombineRestitution）が
// あり、付けるとその版でコンパイルが止まる。名前が合わない欄はただ既定
// （min）のままになる。
class CombineStrategy : public chrono::ChContactMaterialCompositionStrategy {
public:
    explicit CombineStrategy(CombineMode mode) : mode_(mode) {}
    float combine(float a, float b) const {
        switch (mode_) {
            case CombineMode::Average: return 0.5f * (a + b);
            case CombineMode::Max: return std::max(a, b);
            case CombineMode::Min: break;
        }
        return std::min(a, b);
    }
    virtual float CombineFriction(float a1, float a2) const { return combine(a1, a2); }
    virtual float CombineRestitution(float a1, float a2) const { return combine(a1, a2); }
    virtual float CombineCohesion(float a1, float a2) const { return combine(a1, a2); }
    virtual float CombineDamping(float a1, float a2) const { return combine(a1, a2); }

private:
    CombineMode mode_;
};

}  // namespace

bool multicoreAvailable() {
#ifdef WIZ_USE_MULTICORE
    return true;
#else
    return false;
#endif
}

namespace wizengine {
namespace physics {

PhysicsSystem::PhysicsSystem(PhysicsBackend backend, ContactMethod contact)
    : backend_(backend), contact_(contact) {}

const char* PhysicsSystem::backendName() const { return backendName(backend_); }

const char* PhysicsSystem::backendName(PhysicsBackend backend) {
    return backend == PhysicsBackend::Multicore ? "multicore" : "core";
}

const char* PhysicsSystem::contactName() const {
    return contact_ == ContactMethod::SMC ? "smc" : "nsc";
}

void PhysicsSystem::setMaterialCombine(CombineMode mode) {
    // min は Chrono の既定そのもの。既定のままなら Chrono 自身の戦略オブジェクト
    // を残す（差し替えるのは average / max を頼まれたときだけ）。
    if (mode == combine_) return;
    combine_ = mode;
    sys_->SetMaterialCompositionStrategy(std::make_unique<CombineStrategy>(mode));
    LOGI("physics", "material composition: %s",
         mode == CombineMode::Average ? "average" : mode == CombineMode::Max ? "max" : "min");
}

void PhysicsSystem::doStep(double dt) { sys_->DoStepDynamics(dt); }

void PhysicsSystem::setup() { sys_->Setup(); }

void PhysicsSystem::removeLink(std::shared_ptr<ChLinkBase>& link) {
    if (auto taken = std::exchange(link, nullptr)) sys_->RemoveLink(taken);
}

void PhysicsSystem::destroy(PhysicsBackend backend, ContactMethod contact) {
    backend_ = backend;
    contact_ = contact;
    sys_.reset();  // 古い系を先に壊す（Multicore の OpenMP プールを 2 つ持たない）
}

void PhysicsSystem::create() {
    if (backend_ == PhysicsBackend::Multicore && !multicoreAvailable()) {
        LOGW("physics", "Chrono::Multicore not built in "
                        "(configure with -DWIZ_USE_MULTICORE=ON); falling back to core");
        backend_ = PhysicsBackend::Core;
    }

    // Chrono のオブジェクトはすべて chrono_types::make_shared で作る（整列したメモリ）。
    // Chrono は Release だけを入れてあるので、合わせて Release でビルド・実行する。
    // NSC（相補性）は接触を硬い拘束として扱うので、この時間刻みでも箱が
    // 地面をすり抜けない。
#ifdef WIZ_USE_MULTICORE
    if (backend_ == PhysicsBackend::Multicore) {
    // ---- Chrono::Multicore ----------------------------------------------
    // 並列の APGD ソルバと、モジュール自前の並列の衝突判定。
    // 調整は ChSystem のセッターではなく設定の構造体にある。
    std::shared_ptr<ChSystemMulticore> mc;
    if (contact_ == ContactMethod::SMC) {
        // ペナルティ法（SMC）。材質のヤング率・ポアソン比から接触剛性を
        // 出す（use_material_properties）。Hertz 接触 + 1 ステップの接線変位。
        auto smc = chrono_types::make_shared<ChSystemMulticoreSMC>();
        smc->GetSettings()->solver.contact_force_model = ChSystemSMC::ContactForceModel::Hertz;
        smc->GetSettings()->solver.tangential_displ_mode =
            ChSystemSMC::TangentialDisplacementModel::OneStep;
        smc->GetSettings()->solver.use_material_properties = true;
        mc = smc;
    } else {
        auto nsc = chrono_types::make_shared<ChSystemMulticoreNSC>();
        nsc->ChangeSolverType(SolverType::APGD);
        mc = nsc;
    }
    // Bullet ではなくモジュール自前の並列の衝突系を使う: 2 つは別々の経路で、
    // Multicore のソルバはこちらを前提にしている。
    mc->SetCollisionSystemType(ChCollisionSystem::Type::MULTICORE);

    auto* st = mc->GetSettings();
    // SPINNING モードは転がり / スピンの抵抗も解く。SLIDING モードでは材質の
    // 転がり摩擦が単に無視され、球が平らな床を永遠に転がる。
    st->solver.solver_mode = SolverMode::SPINNING;
    st->solver.max_iteration_normal = 0;
    st->solver.max_iteration_sliding = 60;  // Scene が上書きする
    st->solver.max_iteration_spinning = 30;
    st->solver.max_iteration_bilateral = 100;
    st->solver.tolerance = 1e-3;
    st->solver.alpha = 0;
    st->solver.contact_recovery_speed = 0.2;
    st->solver.use_full_inertia_tensor = false;
    st->solver.clamp_bilaterals = true;
    st->solver.bilateral_clamp_speed = 1e8;
    // Multicore の狭域判定は、接触を作るのにある程度のエンベロープを当てにする。
    // Bullet に合う小さな値では接触を丸ごと取りこぼす（箱が床をそのまま抜けて
    // 落ちる）。Scene がさらに上げることがある。
    st->collision.collision_envelope = kMulticoreMinEnvelope;
    // 広域判定の格子: ビンが多い = 細かい区切り。おおよそシーンの広がりを
    // ボディの大きさで割った数に合わせる。
    st->collision.bins_per_axis = vec3(20, 20, 20);
    // 狭域判定: MPR より解析的な基本形状の判定を優先する。MPR は一般の凸形状の
    // 方法で、ペアごとに接触点を 1 つしか返さないことが多く、積んだ箱が揺れて
    // 沈む。解析的な箱どうしの判定はきちんとした多点の接触多様体を作り、Core の
    // ビルドの Bullet の経路にずっと近い。
    // 注意: この列挙の綴りは Chrono の版の間で変わった。この行がコンパイル
    // できなければ、入っているヘッダの ChNarrowphase / NarrowPhaseType を見る
    // （旧版: NarrowPhaseType::NARROWPHASE_HYBRID_MPR）。
    st->collision.narrowphase_algorithm = ChNarrowphase::Algorithm::HYBRID;

    sys_ = mc;
    } else
#endif
    {
    // ---- Chrono core（逐次）---------------------------------------------
    std::shared_ptr<ChSystem> core;
    if (contact_ == ContactMethod::SMC) {
        auto smc = chrono_types::make_shared<ChSystemSMC>();
        smc->SetContactForceModel(ChSystemSMC::ContactForceModel::Hertz);
        core = smc;
    } else {
        core = chrono_types::make_shared<ChSystemNSC>();
    }
    core->SetCollisionSystemType(ChCollisionSystem::Type::BULLET);
    // BARZILAI-BORWEIN は積み重なった接触で既定の SOR よりずっとよく収束する。
    // 反復が多い = 積み重ねが落ち着くが、CPU を食う。
    core->SetSolverType(ChSolver::Type::BARZILAIBORWEIN);
    // 重なったボディを引き離す速さ（m/s）。既定は静止した箱が目に見えて弾ける
    // ほど大きい。小さな値なら静かに落ち着く。
    core->SetMaxPenetrationRecoverySpeed(0.1);
    sys_ = core;
    }
}

void PhysicsSystem::applyBaseSettings() {
    // 覚えている設定を当て直す（初回は既定値 = 従来どおり）。
    sys_->SetGravitationalAcceleration(gravity_);
    setSolverIterations(iterations_);

    // 既定: ソルバと衝突判定に数コアを使い、描画スレッドとエンコーダに 2 つほど
    // 残す。Scene は固定したコアの組に合わせてこれを上書きできる
    // （setNumThreads を参照）。
    {
        int threads = threads_;
        if (threads < 1) {
            const unsigned hw = std::thread::hardware_concurrency();
            threads = (hw >= 4) ? int(hw) - 2 : 1;
        }
        // 3 つは Chrono 本体（FEA の内力とヤコビアン）・衝突判定・Eigen（直接法ソルバ）。
        // FEA はケーブル（Core）だけが使う。
        sys_->SetNumThreads(threads, threads, threads);
    }
}

void PhysicsSystem::applySolverSettings() {
    // 許容差・接触・合成方式・積分器・ソルバも当て直す（recreate 用。初回は
    // 既定値なので何も変わらない）。
    setCollisionTolerances(envelope_, margin_);
    setContactSettings(recoverySpeed_, contactTolerance_);
    if (combine_ != CombineMode::Min) {
        sys_->SetMaterialCompositionStrategy(std::make_unique<CombineStrategy>(combine_));
    }
    {
        const std::string integ = integratorName_, solver = solverName_;
        integratorName_ = "euler";
        solverName_ = "bb";
        if (integ != "euler") setIntegrator(integ);
        if (solver != "bb") setSolver(solver);
    }
}

void PhysicsSystem::setGravity(double gx, double gy, double gz) {
    gravity_ = ChVector3d(gx, gy, gz);
    sys_->SetGravitationalAcceleration(gravity_);
}

bool PhysicsSystem::setIntegrator(const std::string& name) {
    if (backend_ == PhysicsBackend::Multicore) return false;  // 自前のステッパ
    ChTimestepper::Type type;
    if (!timestepperFromName(name, type)) return false;
    // 同じ種類なら触らない（シミュレート開始のたびに呼ばれる）。
    if (name == integratorName_ && sys_->GetTimestepperType() == type) return true;
    sys_->SetTimestepperType(type);
    integratorName_ = name;
    LOGI("physics", "integrator: %s", name.c_str());
    return true;
}

bool PhysicsSystem::setSolver(const std::string& name) {
    if (backend_ == PhysicsBackend::Multicore) return false;  // APGD 固定
    if (name == solverName_) return true;  // 毎回作り直さない（反復回数が飛ぶ）
    // 直接法ソルバはオブジェクトを作って渡す。直接法と MINRES は
    // 線形ソルバで片側拘束（NSC の接触・可動範囲）を解けないので、NSC の
    // ままなら bb に戻して伝える。
    std::shared_ptr<ChSolver> solver;
    std::string used = name;
    if (name == "admm") {
#ifdef WIZ_HAVE_ADMM
        // 既定のコンストラクタは内側の直接法が SparseQR（遅いが依存なし）。
        // Pardiso / MUMPS があればそちらを内側に使う。
#ifdef WIZ_HAVE_PARDISO
        solver = chrono_types::make_shared<ChSolverADMM>(
            chrono_types::make_shared<ChSolverPardisoMKL>());
#elif defined(WIZ_HAVE_MUMPS)
        solver = chrono_types::make_shared<ChSolverADMM>(chrono_types::make_shared<ChSolverMumps>());
#else
        solver = chrono_types::make_shared<ChSolverADMM>();
#endif
#else
        LOGW("physics", "this Chrono has no ChSolverADMM - using pminres");
        used = "pminres";
#endif
    } else if (solverNameIsLinear(name)) {
        if (contact_ != ContactMethod::SMC) {
            LOGW("physics", "solver '%s' is a linear solver and cannot handle NSC "
                            "(one-sided) contacts - using bb (set contact=\"smc\" to use it)",
                 name.c_str());
            used = "bb";
        } else if (name == "minres") {
            // 種類名で作る（SetSolverType が ChSolverMINRES を作る）。
        } else if (name == "sparselu" || name == "sparseqr") {
#ifdef WIZ_HAVE_DIRECT_SOLVERS
            if (name == "sparselu") solver = chrono_types::make_shared<ChSolverSparseLU>();
            else solver = chrono_types::make_shared<ChSolverSparseQR>();
#else
            LOGW("physics", "this Chrono has no ChSolverSparseLU / QR - using bb");
            used = "bb";
#endif
        } else if (name == "pardiso") {
#ifdef WIZ_HAVE_PARDISO
            solver = chrono_types::make_shared<ChSolverPardisoMKL>();
#else
            LOGW("physics", "Chrono::PardisoMKL is not built in (-DWIZ_WITH_PARDISO=ON) - "
                            "using bb");
            used = "bb";
#endif
        } else if (name == "mumps") {
#ifdef WIZ_HAVE_MUMPS
            solver = chrono_types::make_shared<ChSolverMumps>();
#else
            LOGW("physics", "Chrono::MUMPS is not built in (-DWIZ_WITH_MUMPS=ON) - using bb");
            used = "bb";
#endif
        }
    }
    if (solver) {
        sys_->SetSolver(solver);
        solverName_ = used;
        LOGI("physics", "solver: %s", used.c_str());
        setSolverIterations(iterations_);
        return true;
    }
    ChSolver::Type type;
    if (!solverFromName(used, type)) return false;
    // SetSolverType はソルバとシステム記述子を作り直すので、同じ種類なら
    // 触らない（反復回数やウォームスタートの設定が飛ぶ）。
    const bool sameType = sys_->GetSolver() && sys_->GetSolver()->GetType() == type;
    if (!sameType) {
        sys_->SetSolverType(type);
        LOGI("physics", "solver: %s", used.c_str());
    }
    solverName_ = used;
    setSolverIterations(iterations_);
    return true;
}

void PhysicsSystem::setCollisionTolerances(double envelope, double margin) {
    envelope_ = envelope;
    margin_ = margin;
#ifdef WIZ_USE_MULTICORE
    // Multicore の衝突系はエンベロープを自分の設定に持つ。
    if (auto* mc = dynamic_cast<ChSystemMulticore*>(sys_.get())) {
        // 下限の値より下げない - PhysicsSystem::create の注記を参照。
        mc->GetSettings()->collision.collision_envelope =
            std::max(envelope, kMulticoreMinEnvelope);
        return;
    }
#endif
    ChCollisionModel::SetDefaultSuggestedEnvelope(envelope);
    ChCollisionModel::SetDefaultSuggestedMargin(margin);
}

StepTimers PhysicsSystem::timers() const {
    StepTimers t;
    t.step = sys_->GetTimerStep();
    t.solver = sys_->GetTimerLSsolve();
    t.collision = sys_->GetTimerCollision();
    t.setup = sys_->GetTimerSetup();
    t.update = sys_->GetTimerUpdate();
    return t;
}

void PhysicsSystem::setContactSettings(double recoverySpeed, double tolerance) {
    recoverySpeed_ = recoverySpeed;
    contactTolerance_ = tolerance;
#ifdef WIZ_USE_MULTICORE
    if (auto* mc = dynamic_cast<ChSystemMulticore*>(sys_.get())) {
        mc->GetSettings()->solver.contact_recovery_speed = recoverySpeed;
        mc->GetSettings()->solver.tolerance = tolerance;
        return;
    }
#endif
    sys_->SetMaxPenetrationRecoverySpeed(recoverySpeed);
}

void PhysicsSystem::setContactRecoverySpeed(double recoverySpeed) {
    recoverySpeed_ = recoverySpeed;
#ifdef WIZ_USE_MULTICORE
    if (auto* mc = dynamic_cast<ChSystemMulticore*>(sys_.get())) {
        mc->GetSettings()->solver.contact_recovery_speed = recoverySpeed;
        return;
    }
#endif
    sys_->SetMaxPenetrationRecoverySpeed(recoverySpeed);
}

void PhysicsSystem::setNumThreads(int threads) {
    if (threads < 1) return;
    threads_ = threads;
    // Chrono の引数は (Chrono 本体 = FEA の内力とヤコビアン, 衝突判定, Eigen = 直接法ソルバ)。
    // FEA はケーブル（Core）が使う。
    sys_->SetNumThreads(threads, threads, threads);
    LOGI("physics", "threads: %d", threads);
}

void PhysicsSystem::setSolverIterations(int iterations) {
    iterations_ = iterations;
#ifdef WIZ_USE_MULTICORE
    // APGD は滑り（摩擦のある）接触について反復する。
    if (auto* mc = dynamic_cast<ChSystemMulticore*>(sys_.get())) {
        mc->GetSettings()->solver.max_iteration_sliding = iterations;
        return;
    }
#endif
    {
    // この Chrono の版では反復回数はソルバ自身が持つ（ChSystem::SetSolverMaxIterations
    // は消えた）。反復の VI ソルバならそこへ、線形の反復ソルバ（MINRES）なら
    // そちらへ入れる。どちらでもない（直接法など）ときは何もしない。
    if (auto iterative =
            std::dynamic_pointer_cast<ChIterativeSolverVI>(sys_->GetSolver())) {
        iterative->SetMaxIterations(iterations);
        enableWarmStart(iterative.get(), 0);
    } else if (auto ls = std::dynamic_pointer_cast<ChIterativeSolver>(sys_->GetSolver())) {
        ls->SetMaxIterations(iterations);  // MINRES（LS 側の反復）
    }
    }
}

}  // namespace physics
}  // namespace wizengine
