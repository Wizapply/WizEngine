#include "PhysicsWorld.h"
#include "Log.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/physics/ChBodyEasy.h>
#include <chrono/geometry/ChTriangleMeshConnected.h>
#include <chrono/physics/ChContactContainer.h>
#include <chrono/physics/ChLinkDistance.h>
#include <chrono/physics/ChLinkLock.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/solver/ChIterativeSolverVI.h>
#include <chrono/solver/ChSolver.h>

#ifdef WIZ_USE_MULTICORE
// Chrono::Multicore: OpenMP-parallel solver (APGD) and collision detection.
#include <chrono_multicore/physics/ChSystemMulticore.h>
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdio>
#include <thread>

using namespace chrono;

bool multicoreAvailable() {
#ifdef WIZ_USE_MULTICORE
    return true;
#else
    return false;
#endif
}

namespace {

// Minimum downward speed given to a re-dropped body (m/s). The actual value
// also scales with the configured sleep threshold, so the two cannot drift out
// of sync if the Scene tightens the limits.
constexpr double kWakeSpeed = 0.1;

// Smallest contact envelope that the multicore narrowphase behaves with (m).
constexpr double kMulticoreMinEnvelope = 0.01;

// Chrono renamed these between versions (SetUseSleeping -> SetSleepingAllowed,
// SetSleeping -> WakeUp). Pick whichever the installed headers provide: the
// first overload is preferred and the second is only considered if it does not
// compile, so this builds against either API.
template <typename T>
auto allowSleeping(T* obj, bool on, int)
    -> decltype(obj->SetSleepingAllowed(on), void()) {
    obj->SetSleepingAllowed(on);
}
template <typename T>
auto allowSleeping(T* obj, bool on, long)
    -> decltype(obj->SetUseSleeping(on), void()) {
    obj->SetUseSleeping(on);
}

// Sleep thresholds. Chrono 9 renamed SetSleepMinSpeed/SetSleepMinWvel to
// SetSleepMinLinVel/SetSleepMinAngVel; accept either.
template <typename T>
auto setSleepLimits(T* b, float lin, float ang, int)
    -> decltype(b->SetSleepMinLinVel(lin), b->SetSleepMinAngVel(ang), void()) {
    b->SetSleepMinLinVel(lin);
    b->SetSleepMinAngVel(ang);
}
template <typename T>
auto setSleepLimits(T* b, float lin, float ang, long)
    -> decltype(b->SetSleepMinSpeed(lin), b->SetSleepMinWvel(ang), void()) {
    b->SetSleepMinSpeed(lin);
    b->SetSleepMinWvel(ang);
}

// Chrono 9 renamed SetPos_dt to SetPosDt; accept either.
template <typename T>
auto setLinVel(T* b, const chrono::ChVector3d& v, int)
    -> decltype(b->SetPosDt(v), void()) {
    b->SetPosDt(v);
}
template <typename T>
auto setLinVel(T* b, const chrono::ChVector3d& v, long)
    -> decltype(b->SetPos_dt(v), void()) {
    b->SetPos_dt(v);
}

// Warm starting reuses the previous step's impulses as the starting guess.
// For stacked contacts it converges far faster, so fewer iterations are needed.
// Named EnableWarmStart in some Chrono versions, SetWarmStart in others.
template <typename T>
auto enableWarmStart(T* solver, int) -> decltype(solver->EnableWarmStart(true), void()) {
    solver->EnableWarmStart(true);
}
template <typename T>
auto enableWarmStart(T* solver, long) -> decltype(solver->SetWarmStart(true), void()) {
    solver->SetWarmStart(true);
}
template <typename T>
void enableWarmStart(T*, ...) {}

template <typename T>
auto getLinVel(T* b, int) -> decltype(b->GetPosDt()) {
    return b->GetPosDt();
}
template <typename T>
auto getLinVel(T* b, long) -> decltype(b->GetPos_dt()) {
    return b->GetPos_dt();
}

template <typename T>
auto getAngVel(T* b, int) -> decltype(b->GetAngVelParent()) {
    return b->GetAngVelParent();
}
template <typename T>
auto getAngVel(T* b, long) -> decltype(b->GetWvel_par()) {
    return b->GetWvel_par();
}
template <typename T>
auto setAngVel(T* b, const chrono::ChVector3d& w, int)
    -> decltype(b->SetAngVelParent(w), void()) {
    b->SetAngVelParent(w);
}
template <typename T>
auto setAngVel(T* b, const chrono::ChVector3d& w, long)
    -> decltype(b->SetWvel_par(w), void()) {
    b->SetWvel_par(w);
}

template <typename T>
auto wakeUp(T* obj, int) -> decltype(obj->SetSleeping(false), void()) {
    obj->SetSleeping(false);
}
template <typename T>
auto wakeUp(T* obj, long) -> decltype(obj->WakeUp(), void()) {
    obj->WakeUp();
}

// ChLinkLock 系の Initialize は Chrono 9 で ChCoordsys<> から ChFrame<> に
// 変わった。どちらでも通るように、コンパイルできるほうを選ぶ（この
// ファイルで既に使っている sleeping/velocity の書き方と同じ手口）。
template <typename L, typename B>
auto initLink(L* link, const B& b1, const B& b2, const chrono::ChVector3d& pos,
              const chrono::ChQuaternion<>& rot, int)
    -> decltype(link->Initialize(b1, b2, chrono::ChFrame<>(pos, rot)), void()) {
    link->Initialize(b1, b2, chrono::ChFrame<>(pos, rot));
}
template <typename L, typename B>
auto initLink(L* link, const B& b1, const B& b2, const chrono::ChVector3d& pos,
              const chrono::ChQuaternion<>& rot, long)
    -> decltype(link->Initialize(b1, b2, chrono::ChCoordsys<>(pos, rot)),
                void()) {
    link->Initialize(b1, b2, chrono::ChCoordsys<>(pos, rot));
}

// ChLinkDistance の Initialize は版によって引数が違う。合わなければ
// 「作らない」で済ませる（アプリを落とすほどの機能ではない）。
template <typename L, typename B>
auto initDistance(L* link, const B& b1, const B& b2,
                  const chrono::ChVector3d& p1, const chrono::ChVector3d& p2,
                  double distance, int)
    -> decltype(link->Initialize(b1, b2, false, p1, p2, false, distance),
                bool()) {
    link->Initialize(b1, b2, false, p1, p2, false, distance);
    return true;
}
template <typename L, typename B>
bool initDistance(L*, const B&, const B&, const chrono::ChVector3d&,
                  const chrono::ChVector3d&, double, ...) {
    return false;
}

// 接触コンテナの走査で、ボディ同士の接触だけを physId のペアに集める。
// ChContactable* から ChBody* へは dynamic_cast（Chrono 自身が
// SumAllContactForces で使っている手）。同じペアに複数の接触点があるので、
// 呼び出し側（activeContactPairs）で 1 本化する。
class ContactPairCollector
    : public chrono::ChContactContainer::ReportContactCallback {
public:
    ContactPairCollector(
        const std::unordered_map<const chrono::ChBody*, std::size_t>& index,
        std::vector<std::pair<std::size_t, std::size_t>>& out)
        : index_(index), out_(out) {}

    bool OnReportContact(const ChVector3d&, const ChVector3d&,
                         const ChMatrix33<>&, const double&, const double&,
                         const ChVector3d&, const ChVector3d&,
                         ChContactable* objA, ChContactable* objB) override {
        const auto* bodyA = dynamic_cast<const ChBody*>(objA);
        const auto* bodyB = dynamic_cast<const ChBody*>(objB);
        if (!bodyA || !bodyB) return true;  // ボディ以外（FEA 等）は対象外
        const auto ia = index_.find(bodyA);
        const auto ib = index_.find(bodyB);
        if (ia == index_.end() || ib == index_.end()) return true;
        std::size_t a = ia->second;
        std::size_t b = ib->second;
        if (a == b) return true;
        if (a > b) std::swap(a, b);
        out_.push_back({a, b});
        return true;  // 続けて最後まで走査する
    }

private:
    const std::unordered_map<const chrono::ChBody*, std::size_t>& index_;
    std::vector<std::pair<std::size_t, std::size_t>>& out_;
};

// +Z をこの向きに合わせる回転。ChLinkLockRevolute はリンク座標系の Z 軸まわり
// に回り、ChLinkLockPrismatic は Z 軸方向にスライドするので、ユーザーが指定した
// ワールド軸をリンクの Z に持ってくる必要がある。
// Chrono のベクトル演算子は版ごとに名前が違うので、成分計算で書いてある。
chrono::ChQuaternion<> quatFromZAxis(const chrono::ChVector3d& axis) {
    double ax = axis.x(), ay = axis.y(), az = axis.z();
    const double n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n < 1e-9) return chrono::ChQuaternion<>(1, 0, 0, 0);
    ax /= n;
    ay /= n;
    az /= n;

    const double c = az;  // dot((0,0,1), axis)
    if (c < -0.999999) {  // 真後ろ: X 軸まわりに 180 度
        return chrono::ChQuaternion<>(0, 1, 0, 0);
    }
    // 最短回転。cross((0,0,1), axis) = (-ay, ax, 0)
    chrono::ChQuaternion<> q(1.0 + c, -ay, ax, 0.0);
    q.Normalize();
    return q;
}

}  // namespace

const char* PhysicsWorld::backendName() const {
    return backend_ == PhysicsBackend::Multicore ? "multicore" : "core";
}

void PhysicsWorld::registerBody(const std::shared_ptr<chrono::ChBody>& body) {
    bodyIndex_[body.get()] = bodies_.size();
    bodies_.push_back(body);
    active_.push_back(true);
}

std::vector<std::pair<std::size_t, std::size_t>>
PhysicsWorld::activeContactPairs() const {
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    const auto container = sys_->GetContactContainer();
    if (!container) return pairs;
    auto collector =
        chrono_types::make_shared<ContactPairCollector>(bodyIndex_, pairs);
    container->ReportAllContacts(collector);
    // 1 ペアに接触点は複数あるのが普通（箱同士は最大 4 点）。ここで 1 本化。
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

PhysicsWorld::PhysicsWorld(PhysicsBackend backend) {
    backend_ = backend;
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
    auto mc = chrono_types::make_shared<ChSystemMulticoreNSC>();
    // Use the module's own parallel collision system, not Bullet: the two are
    // separate code paths and the multicore solver expects this one.
    mc->SetCollisionSystemType(ChCollisionSystem::Type::MULTICORE);
    mc->ChangeSolverType(SolverType::APGD);

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
    auto core = chrono_types::make_shared<ChSystemNSC>();
    core->SetCollisionSystemType(ChCollisionSystem::Type::BULLET);
    // BARZILAI-BORWEIN converges much better than the default SOR on stacked
    // contacts. More iterations = steadier stacks, more CPU.
    core->SetSolverType(ChSolver::Type::BARZILAIBORWEIN);
    // How fast overlapping bodies are pushed apart (m/s). The default is high
    // enough to visibly pop resting boxes; a small value settles them quietly.
    core->SetMaxPenetrationRecoverySpeed(0.1);
    sys_ = core;
    }

    sys_->SetGravitationalAcceleration(ChVector3d(0, -9.81, 0));
    setSolverIterations(150);  // Scene overrides this

    // Default: several cores for the solver and collision detection, leaving a
    // couple for the render thread and the encoder. The Scene can override
    // this (see setNumThreads) to match a pinned set of cores.
    {
        const unsigned hw = std::thread::hardware_concurrency();
        const int threads = (hw >= 4) ? int(hw) - 2 : 1;
        sys_->SetNumThreads(threads, threads, 1);
    }

    // Sleeping is off until the Scene enables it (see setSleepingEnabled).
    // Note: Chrono::Multicore does not support sleeping - the call is harmless
    // there, but sleepingCount() will simply stay at 0.
    allowSleeping(sys_.get(), false, 0);

    mat_ = chrono_types::make_shared<ChContactMaterialNSC>();
    mat_->SetFriction(0.6f);
    // No bounce: even a little restitution keeps settled boxes micro-bouncing.
    mat_->SetRestitution(0.0f);
}

void PhysicsWorld::setSleepingEnabled(bool enabled, float seconds,
                                      float minLinVel, float minAngVel) {
    sleepingEnabled_ = enabled;
    sleepSeconds_ = seconds;
    sleepMinLinVel_ = minLinVel;
    sleepMinAngVel_ = minAngVel;

    allowSleeping(sys_.get(), enabled, 0);
    for (auto& b : bodies_) {
        allowSleeping(b.get(), enabled, 0);
        if (!enabled) {
            wakeUp(b.get(), 0);
            continue;
        }
        b->SetSleepTime(seconds);
        setSleepLimits(b.get(), minLinVel, minAngVel, 0);
    }
}

std::size_t PhysicsWorld::sleepingCount() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        if (!active_[i]) continue;  // 退場済みは数えない
        if (bodies_[i]->IsSleeping()) ++n;
    }
    return n;
}

void PhysicsWorld::wakeAll() {
    for (auto& b : bodies_) wakeUp(b.get(), 0);
    // Waking from outside ManageSleepingBodies() changes how many bodies the
    // solver has to handle, and Chrono only rebuilds that layout in Setup().
    // Without this the "woken" bodies can stay out of the solve - i.e. they
    // would not fall after a reset.
    sys_->Setup();
}

void PhysicsWorld::setCollisionTolerances(double envelope, double margin) {
#ifdef WIZ_USE_MULTICORE
    // The multicore collision system keeps the envelope in its settings.
    if (auto* mc = dynamic_cast<ChSystemMulticoreNSC*>(sys_.get())) {
        // Never go below the floor value - see the note in the constructor.
        mc->GetSettings()->collision.collision_envelope =
            std::max(envelope, kMulticoreMinEnvelope);
        return;
    }
#endif
    ChCollisionModel::SetDefaultSuggestedEnvelope(envelope);
    ChCollisionModel::SetDefaultSuggestedMargin(margin);
}

StepTimers PhysicsWorld::timers() const {
    StepTimers t;
    t.step = sys_->GetTimerStep();
    t.solver = sys_->GetTimerLSsolve();
    t.collision = sys_->GetTimerCollision();
    t.setup = sys_->GetTimerSetup();
    t.update = sys_->GetTimerUpdate();
    return t;
}

void PhysicsWorld::setContactSettings(double recoverySpeed, double tolerance) {
#ifdef WIZ_USE_MULTICORE
    if (auto* mc = dynamic_cast<ChSystemMulticoreNSC*>(sys_.get())) {
        mc->GetSettings()->solver.contact_recovery_speed = recoverySpeed;
        mc->GetSettings()->solver.tolerance = tolerance;
        return;
    }
#endif
    sys_->SetMaxPenetrationRecoverySpeed(recoverySpeed);
}

void PhysicsWorld::setContactRecoverySpeed(double recoverySpeed) {
#ifdef WIZ_USE_MULTICORE
    if (auto* mc = dynamic_cast<ChSystemMulticoreNSC*>(sys_.get())) {
        mc->GetSettings()->solver.contact_recovery_speed = recoverySpeed;
        return;
    }
#endif
    sys_->SetMaxPenetrationRecoverySpeed(recoverySpeed);
}

void PhysicsWorld::applyForce(std::size_t id, const ChVector3d& force,
                              double dt) {
    if (id >= bodies_.size()) return;
    auto& b = bodies_[id];
    const double mass = b->GetMass();
    if (mass <= 0.0) return;

    // A sleeping body ignores everything until something touches it, so wake it
    // first - otherwise pushing a settled box does nothing at all.
    if (b->IsSleeping()) wakeUp(b.get(), 0);

    const ChVector3d v = getLinVel(b.get(), 0);
    setLinVel(b.get(), v + force * (dt / mass), 0);
}

chrono::ChVector3d PhysicsWorld::bodyVelocity(std::size_t id) const {
    if (id >= bodies_.size()) return chrono::ChVector3d(0, 0, 0);
    return getLinVel(bodies_[id].get(), 0);
}

double PhysicsWorld::bodyMass(std::size_t id) const {
    return id < bodies_.size() ? bodies_[id]->GetMass() : 0.0;
}

void PhysicsWorld::setNumThreads(int threads) {
    if (threads < 1) return;
    // Chrono takes (solver, collision, FEA); the last stays at 1 since no FEA
    // is used here.
    sys_->SetNumThreads(threads, threads, 1);
    LOGI("physics", "threads: %d", threads);
}

void PhysicsWorld::setSolverIterations(int iterations) {
#ifdef WIZ_USE_MULTICORE
    // APGD iterates on the sliding (frictional) contacts.
    if (auto* mc = dynamic_cast<ChSystemMulticoreNSC*>(sys_.get())) {
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
    }
    }
}

void PhysicsWorld::step(double dt) {
    sys_->DoStepDynamics(dt);

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

std::size_t PhysicsWorld::addSphere(double radius, double density,
                                    const ChVector3d& pos,
                                    const ChQuaternion<>& rot, bool fixed) {
    auto b = chrono_types::make_shared<ChBodyEasySphere>(
        radius, density, /*visualize*/ true, /*collide*/ true, mat_);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    b->EnableCollision(true);
    allowSleeping(b.get(), sleepingEnabled_, 0);
    if (sleepingEnabled_) {
        b->SetSleepTime(sleepSeconds_);
        setSleepLimits(b.get(), sleepMinLinVel_, sleepMinAngVel_, 0);
    }
    sys_->AddBody(b);
    registerBody(b);
    return bodies_.size() - 1;
}

void PhysicsWorld::setDamping(double linearPerSecond, double angularPerSecond) {
    linearDamping_ = linearPerSecond;
    angularDamping_ = angularPerSecond;
}

void PhysicsWorld::setSurfaceMaterial(float friction, float restitution) {
    mat_->SetFriction(friction);
    mat_->SetRestitution(restitution);
}

void PhysicsWorld::setRollingFriction(float rolling, float spinning) {
    // NSC materials expose these directly; they are ignored by solvers that do
    // not model rolling resistance, which is harmless.
    mat_->SetRollingFriction(rolling);
    mat_->SetSpinningFriction(spinning);
}

std::size_t PhysicsWorld::addBox(double sx, double sy, double sz, double density,
                                 const ChVector3d& pos,
                                 const ChQuaternion<>& rot, bool fixed) {
    auto b = chrono_types::make_shared<ChBodyEasyBox>(
        sx, sy, sz, density, /*visualize*/ true, /*collide*/ true, mat_);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    b->EnableCollision(true);
    allowSleeping(b.get(), sleepingEnabled_, 0);
    if (sleepingEnabled_) {
        b->SetSleepTime(sleepSeconds_);
        setSleepLimits(b.get(), sleepMinLinVel_, sleepMinAngVel_, 0);
    }
    sys_->AddBody(b);
    registerBody(b);
    return bodies_.size() - 1;
}

std::size_t PhysicsWorld::addConvexHull(
    const std::vector<ChVector3d>& points, double density,
    const ChVector3d& pos, const ChQuaternion<>& rot) {
    // visualize=false: our renderer draws the glTF model itself; Chrono's
    // visual assets are unused here.
    //
    // Chrono's constructor takes the points by NON-const reference and
    // translates them in place (it moves the barycentre onto the centre of
    // mass) - hence the local copy: the caller's point cloud is shared by
    // every body and must not be mutated.
    std::vector<ChVector3d> local = points;
    auto b = chrono_types::make_shared<ChBodyEasyConvexHull>(
        local, density, /*visualize*/ false, /*collide*/ true, mat_);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(false);
    b->EnableCollision(true);
    allowSleeping(b.get(), sleepingEnabled_, 0);
    if (sleepingEnabled_) {
        b->SetSleepTime(sleepSeconds_);
        setSleepLimits(b.get(), sleepMinLinVel_, sleepMinAngVel_, 0);
    }
    sys_->AddBody(b);
    registerBody(b);
    return bodies_.size() - 1;
}

std::size_t PhysicsWorld::addStaticMesh(
    std::shared_ptr<chrono::ChTriangleMeshConnected> mesh,
    const ChVector3d& pos, const ChQuaternion<>& rot) {
    // compute_mass=false: the body is fixed, its mass is irrelevant. The
    // small sphere-swept thickness rounds each triangle slightly, which is
    // what makes NSC contacts against a raw mesh behave.
    auto b = chrono_types::make_shared<ChBodyEasyMesh>(
        mesh, 1000.0, /*compute_mass*/ false, /*visualize*/ false,
        /*collide*/ true, mat_, /*sphere_swept*/ 0.002);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(true);
    b->EnableCollision(true);
    sys_->AddBody(b);
    registerBody(b);
    return bodies_.size() - 1;
}

std::size_t PhysicsWorld::bodyCount() const {
    return bodies_.size();
}

BodyTransform PhysicsWorld::bodyTransform(std::size_t id) const {
    const auto& b = bodies_[id];
    const ChVector3d p = b->GetPos();
    const ChQuaternion<> q = b->GetRot();
    return {p.x(), p.y(), p.z(), q.e0(), q.e1(), q.e2(), q.e3()};
}

void PhysicsWorld::setBodyPose(std::size_t id, const ChVector3d& pos,
                               const ChQuaternion<>& rot) {
    bodies_[id]->SetPos(pos);
    bodies_[id]->SetRot(rot);
    bodies_[id]->ForceToRest();  // zero linear + angular velocity and accel
    wakeUp(bodies_[id].get(), 0);  // a re-dropped body must not stay asleep

    // ...and it must not fall straight back asleep. Chrono only refreshes a
    // body's "last moving" timestamp while it is above the sleep speed, so a
    // body we just stopped still looks like it has been still for ages and
    // would sleep again on the very next step - before gravity gets a chance
    // to speed it up. A small initial downward velocity resets that timer.
    const double wakeSpeed =
        std::max(kWakeSpeed, 3.0 * static_cast<double>(sleepMinLinVel_));
    setLinVel(bodies_[id].get(), ChVector3d(0, -wakeSpeed, 0), 0);
}

// ---- エディタ用 -----------------------------------------------------------

void PhysicsWorld::setGravityY(double gravityY) {
    sys_->SetGravitationalAcceleration(ChVector3d(0, gravityY, 0));
}

void PhysicsWorld::placeBody(std::size_t id, const ChVector3d& pos,
                             const ChQuaternion<>& rot) {
    if (id >= bodies_.size()) return;
    auto& b = bodies_[id];
    b->SetPos(pos);
    b->SetRot(rot);
    b->ForceToRest();
    wakeUp(b.get(), 0);
    // setBodyPose と違って落下速度は与えない。エディタで置いた物は、
    // シミュレートを始めるまでその場に止まっていてほしい。
}

void PhysicsWorld::setBodyFixed(std::size_t id, bool fixed) {
    if (id >= bodies_.size()) return;
    bodies_[id]->SetFixed(fixed);
    if (!fixed) wakeUp(bodies_[id].get(), 0);
}

void PhysicsWorld::disableBody(std::size_t id) {
    if (id >= bodies_.size() || !active_[id]) return;
    auto& b = bodies_[id];
    b->SetFixed(true);
    b->EnableCollision(false);
    b->ForceToRest();
    // 地面のはるか下へ。当たり判定を切っても衝突系がまだ形状を持っている
    // 版があるので、位置でも確実に無関係にしておく。
    b->SetPos(ChVector3d(0, -1000.0, 0));
    active_[id] = false;
}

bool PhysicsWorld::bodyActive(std::size_t id) const {
    return id < active_.size() && active_[id];
}

// ---- ジョイント -----------------------------------------------------------

std::size_t PhysicsWorld::addJoint(JointType type, std::size_t bodyA,
                                   std::size_t bodyB, const ChVector3d& anchor,
                                   const ChVector3d& axis, double distance) {
    if (bodyA >= bodies_.size() || bodyB >= bodies_.size()) {
        LOGW("physics", "joint: body id out of range (%zu, %zu)", bodyA, bodyB);
        return kInvalidJoint;
    }
    if (bodyA == bodyB) {
        LOGW("physics", "joint: both ends are the same body (%zu)", bodyA);
        return kInvalidJoint;
    }
    if (!active_[bodyA] || !active_[bodyB]) {
        LOGW("physics", "joint: body has been removed (%zu, %zu)", bodyA, bodyB);
        return kInvalidJoint;
    }

    auto a = bodies_[bodyA];
    auto b = bodies_[bodyB];
    // 眠ったままだと拘束が効かないので、両端とも起こしておく。
    wakeUp(a.get(), 0);
    wakeUp(b.get(), 0);

    // リンク座標系。Revolute は Z 軸まわりに回り、Prismatic は Z 軸方向へ
    // スライドするので、指定されたワールド軸を Z に合わせる。
    const ChQuaternion<> frameRot = quatFromZAxis(axis);

    std::shared_ptr<chrono::ChLinkBase> link;
    switch (type) {
        case JointType::Fixed: {
            auto l = chrono_types::make_shared<ChLinkLockLock>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Revolute: {
            auto l = chrono_types::make_shared<ChLinkLockRevolute>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Spherical: {
            auto l = chrono_types::make_shared<ChLinkLockSpherical>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Prismatic: {
            auto l = chrono_types::make_shared<ChLinkLockPrismatic>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Distance: {
            // 2 点は各ボディの現在位置。distance が 0 なら今の間隔を保つ。
            const ChVector3d p1 = a->GetPos();
            const ChVector3d p2 = b->GetPos();
            double len = distance;
            if (len <= 0.0) {
                const ChVector3d d = p2 - p1;
                len = std::sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
            }
            auto l = chrono_types::make_shared<ChLinkDistance>();
            if (!initDistance(l.get(), a, b, p1, p2, len, 0)) {
                LOGW("physics",
                     "distance joint: this Chrono version has a different "
                     "ChLinkDistance::Initialize - skipped");
                return kInvalidJoint;
            }
            link = l;
            break;
        }
    }

    if (!link) return kInvalidJoint;
    sys_->AddLink(link);
    joints_.push_back(link);
    return joints_.size() - 1;
}

void PhysicsWorld::removeAllJoints() {
    for (auto& l : joints_) sys_->RemoveLink(l);
    joints_.clear();
    // 拘束の増減はソルバの構成を変えるので、Setup で作り直させる
    // （wakeAll と同じ理由）。
    sys_->Setup();
}

std::size_t PhysicsWorld::jointCount() const {
    return joints_.size();
}
