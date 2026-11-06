// Chrono の系と設定: 系の生成・重力・積分器・ソルバ・衝突の許容差・スレッド数・材質の合成方式。

#include "physics/PhysicsSystem.h"
#include "physics/ChronoCompat.h"
#include "core/Log.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/physics/ChContactMaterial.h>  // 材質の合成方式
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
// Chrono::Multicore: OpenMP-parallel solver (APGD) and collision detection.
#include <chrono_multicore/physics/ChSystemMulticore.h>
#endif

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace {

// Smallest contact envelope that the multicore narrowphase behaves with (m).
constexpr double kMulticoreMinEnvelope = 0.01;

// 積分器の種類（名前は editor::integratorNameValid の語彙）。hht / newmark は
// 滑らかな系（FEA・SMC）向け（B の 16）。
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

const char* PhysicsSystem::backendName() const {
    return backend_ == PhysicsBackend::Multicore ? "multicore" : "core";
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

void PhysicsSystem::destroy(PhysicsBackend backend, ContactMethod contact) {
    backend_ = backend;
    contact_ = contact;
    sys_.reset();  // 古い系を先に壊す（Multicore の OpenMP プールを 2 つ持たない）
}

void PhysicsSystem::create() {
    if (backend_ == PhysicsBackend::Multicore && !multicoreAvailable()) {
        std::puts(
            "physics: Chrono::Multicore not built in "
            "(configure with -DWIZ_USE_MULTICORE=ON); falling back to core");
        backend_ = PhysicsBackend::Core;
    }

    // Create every Chrono object with chrono_types::make_shared (aligned memory).
    // Build & run in Release to match the Release-only Chrono install.
    // NSC (complementarity) treats contacts as hard constraints, so boxes do
    // not tunnel through the ground at this timestep.
#ifdef WIZ_USE_MULTICORE
    if (backend_ == PhysicsBackend::Multicore) {
    // ---- Chrono::Multicore ----------------------------------------------
    // Parallel APGD solver plus the module's own parallel collision detection.
    // Its tuning lives in a settings struct rather than in ChSystem setters.
    std::shared_ptr<ChSystemMulticore> mc;
    if (contact_ == ContactMethod::SMC) {
        // ペナルティ法（B の 22）。材質のヤング率・ポアソン比から接触剛性を
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
    // Use the module's own parallel collision system, not Bullet: the two are
    // separate code paths and the multicore solver expects this one.
    mc->SetCollisionSystemType(ChCollisionSystem::Type::MULTICORE);

    auto* st = mc->GetSettings();
    // SPINNING mode also solves rolling/spinning resistance. In SLIDING mode
    // the material's rolling friction is simply ignored, and spheres roll
    // across a flat floor forever.
    st->solver.solver_mode = SolverMode::SPINNING;
    st->solver.max_iteration_normal = 0;
    st->solver.max_iteration_sliding = 60;  // Scene overrides this
    st->solver.max_iteration_spinning = 30;
    st->solver.max_iteration_bilateral = 100;
    st->solver.tolerance = 1e-3;
    st->solver.alpha = 0;
    st->solver.contact_recovery_speed = 0.2;
    st->solver.use_full_inertia_tensor = false;
    st->solver.clamp_bilaterals = true;
    st->solver.bilateral_clamp_speed = 1e8;
    // The multicore narrowphase relies on a non-trivial envelope to generate
    // contacts; the tiny value that suits Bullet makes it miss them entirely
    // (boxes drop straight through the floor). Scene may raise this further.
    st->collision.collision_envelope = kMulticoreMinEnvelope;
    // Broadphase grid: more bins = finer buckets. Roughly match the scene
    // extent divided by the body size.
    st->collision.bins_per_axis = vec3(20, 20, 20);
    // Narrowphase: prefer the analytic primitive routines over MPR. MPR is a
    // general convex algorithm that tends to report a single contact point per
    // pair, which lets stacked boxes rock and sink; the analytic box-box test
    // produces a proper multi-point manifold, much closer to what the Bullet
    // path in the core build does.
    // NOTE: the enum spelling moved between Chrono versions. If this line does
    // not compile, check ChNarrowphase / NarrowPhaseType in the installed
    // headers (older: NarrowPhaseType::NARROWPHASE_HYBRID_MPR).
    st->collision.narrowphase_algorithm = ChNarrowphase::Algorithm::HYBRID;

    sys_ = mc;
    } else
#endif
    {
    // ---- Chrono core (serial) -------------------------------------------
    std::shared_ptr<ChSystem> core;
    if (contact_ == ContactMethod::SMC) {
        auto smc = chrono_types::make_shared<ChSystemSMC>();
        smc->SetContactForceModel(ChSystemSMC::ContactForceModel::Hertz);
        core = smc;
    } else {
        core = chrono_types::make_shared<ChSystemNSC>();
    }
    core->SetCollisionSystemType(ChCollisionSystem::Type::BULLET);
    // BARZILAI-BORWEIN converges much better than the default SOR on stacked
    // contacts. More iterations = steadier stacks, more CPU.
    core->SetSolverType(ChSolver::Type::BARZILAIBORWEIN);
    // How fast overlapping bodies are pushed apart (m/s). The default is high
    // enough to visibly pop resting boxes; a small value settles them quietly.
    core->SetMaxPenetrationRecoverySpeed(0.1);
    sys_ = core;
    }
}

void PhysicsSystem::applyBaseSettings() {
    // 覚えている設定を当て直す（初回は既定値 = 従来どおり）。
    sys_->SetGravitationalAcceleration(gravity_);
    setSolverIterations(iterations_);

    // Default: several cores for the solver and collision detection, leaving a
    // couple for the render thread and the encoder. The Scene can override
    // this (see setNumThreads) to match a pinned set of cores.
    {
        int threads = threads_;
        if (threads < 1) {
            const unsigned hw = std::thread::hardware_concurrency();
            threads = (hw >= 4) ? int(hw) - 2 : 1;
        }
        // 3 つ目は FEA（ケーブルの要素の並列化）。Core だけが使う。
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
    // 直接法はソルバのオブジェクトを作って渡す（B の 16）。直接法と MINRES は
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
    // The multicore collision system keeps the envelope in its settings.
    if (auto* mc = dynamic_cast<ChSystemMulticore*>(sys_.get())) {
        // Never go below the floor value - see the note in PhysicsSystem::create.
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
    // Chrono takes (solver, collision, FEA). FEA はケーブル（Core）が使う。
    sys_->SetNumThreads(threads, threads, threads);
    LOGI("physics", "threads: %d", threads);
}

void PhysicsSystem::setSolverIterations(int iterations) {
    iterations_ = iterations;
#ifdef WIZ_USE_MULTICORE
    // APGD iterates on the sliding (frictional) contacts.
    if (auto* mc = dynamic_cast<ChSystemMulticore*>(sys_.get())) {
        mc->GetSettings()->solver.max_iteration_sliding = iterations;
        return;
    }
#endif
    {
    // The iteration count lives on the solver itself in this Chrono version
    // (ChSystem::SetSolverMaxIterations was removed). If the active solver is
    // not an iterative VI solver, the cast fails and we keep the defaults.
    if (auto iterative =
            std::dynamic_pointer_cast<ChIterativeSolverVI>(sys_->GetSolver())) {
        iterative->SetMaxIterations(iterations);
        enableWarmStart(iterative.get(), 0);
    } else if (auto ls = std::dynamic_pointer_cast<ChIterativeSolver>(sys_->GetSolver())) {
        ls->SetMaxIterations(iterations);  // MINRES（LS 側の反復）
    }
    }
}

void PhysicsSystem::setGravityY(double gravityY) {
    sys_->SetGravitationalAcceleration(ChVector3d(0, gravityY, 0));
}

}  // namespace physics
}  // namespace wizengine
