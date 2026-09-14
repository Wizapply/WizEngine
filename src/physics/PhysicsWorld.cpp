#include "physics/PhysicsWorld.h"
#include "core/Log.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionSystem.h>
// 複合形状（プレハブの collide 部品）。Chrono 9 の形状クラス。ヘッダが無い
// 版では複合形状を諦めて警告だけ出す（ビルドは止めない）。
#if __has_include(<chrono/collision/ChCollisionShapeBox.h>)
#include <chrono/collision/ChCollisionShapeBox.h>
#include <chrono/collision/ChCollisionShapeSphere.h>
#define WIZ_HAVE_COLLISION_SHAPES 1
#endif
// 円柱の当たり形状（プレハブの円柱部品）と三角メッシュ（glTF の凹形状）。
// どちらも Chrono 9 の形状クラスで、無い版ではその形だけ諦めて警告する。
#if __has_include(<chrono/collision/ChCollisionShapeCylinder.h>)
#include <chrono/collision/ChCollisionShapeCylinder.h>
#define WIZ_HAVE_CYLINDER_SHAPE 1
#endif
#if __has_include(<chrono/collision/ChCollisionShapeTriangleMesh.h>)
#include <chrono/collision/ChCollisionShapeTriangleMesh.h>
#define WIZ_HAVE_TRIMESH_SHAPE 1
#endif
#include <chrono/collision/bullet/ChCollisionUtilsBullet.h>  // 凸包の体積重心
#include <chrono/core/ChMatrix33.h>
#include <chrono/geometry/ChTriangleMeshConnected.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChBodyEasy.h>
#include <chrono/physics/ChContactContainer.h>
#include <chrono/physics/ChContactMaterial.h>  // 材質の合成方式
#include <chrono/physics/ChContactMaterialNSC.h>
#include <chrono/physics/ChContactMaterialSMC.h>
#include <chrono/physics/ChLinkDistance.h>
#include <chrono/physics/ChLinkLock.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/physics/ChSystemSMC.h>
#include <chrono/solver/ChIterativeSolver.h>
#include <chrono/solver/ChIterativeSolverVI.h>
#include <chrono/solver/ChSolver.h>
#include <chrono/timestepper/ChTimestepper.h>
// ---- B の導入で足したヘッダ。どれも __has_include で見て、無い版・無い
// ビルドではその機能だけ「無い」と答える（physicsFeatures）。
// 直接法（Eigen の SparseLU / SparseQR は Core 組み込み）。
#if __has_include(<chrono/solver/ChDirectSolverLS.h>)
#include <chrono/solver/ChDirectSolverLS.h>
#define WIZ_HAVE_DIRECT_SOLVERS 1
#endif
// ADMM（FEA の剛性行列と NSC の接触を同時に解ける VI ソルバ。内側は直接法）。
#if __has_include(<chrono/solver/ChSolverADMM.h>)
#include <chrono/solver/ChSolverADMM.h>
#define WIZ_HAVE_ADMM 1
#endif
#if __has_include(<chrono_pardisomkl/ChSolverPardisoMKL.h>) && defined(WIZ_WITH_PARDISO)
#include <chrono_pardisomkl/ChSolverPardisoMKL.h>
#define WIZ_HAVE_PARDISO 1
#endif
#if __has_include(<chrono_mumps/ChSolverMumps.h>) && defined(WIZ_WITH_MUMPS)
#include <chrono_mumps/ChSolverMumps.h>
#define WIZ_HAVE_MUMPS 1
#endif
// 荷重（ChLoad）: ブッシュと定常荷重。
#if __has_include(<chrono/physics/ChLoadContainer.h>) && __has_include(<chrono/physics/ChLoadsBody.h>)
#include <chrono/physics/ChLoadContainer.h>
#include <chrono/physics/ChLoadsBody.h>
#define WIZ_HAVE_LOADS 1
#endif
// FEA のケーブル（ANCF）。Chrono 9 は ChLinkNodeFrame、旧版は ChLinkPointFrame。
#if __has_include(<chrono/fea/ChBuilderBeam.h>) && __has_include(<chrono/fea/ChMesh.h>)
#include <chrono/fea/ChBeamSectionCable.h>
#include <chrono/fea/ChBuilderBeam.h>
#include <chrono/fea/ChContactSurfaceNodeCloud.h>
#include <chrono/fea/ChElementCableANCF.h>
#include <chrono/fea/ChMesh.h>
#include <chrono/fea/ChNodeFEAxyzD.h>
#if __has_include(<chrono/fea/ChLinkNodeFrame.h>)
#include <chrono/fea/ChLinkNodeFrame.h>
using WizNodeFrameLink = chrono::fea::ChLinkNodeFrame;
#else
#include <chrono/fea/ChLinkPointFrame.h>
using WizNodeFrameLink = chrono::fea::ChLinkPointFrame;
#endif
#define WIZ_HAVE_FEA 1
#endif
// モーダル解析（Chrono::Modal。CMake の WIZ_WITH_CHRONO_MODAL）。
#if defined(WIZ_WITH_CHRONO_MODAL) && __has_include(<chrono_modal/ChEigenvalueSolver.h>)
#include <chrono_modal/ChEigenvalueSolver.h>
#define WIZ_HAVE_MODAL 1
#endif
// ジョイントの拡張（A の導入）: モータ・自在継手・歯車・ねじ・ばね。
// Chrono 9 のヘッダ名。歯車とねじは版で置き場が変わったので __has_include
// で見て、無ければその種類だけ「作らない」で済ませる（Distance と同じ扱い）。
#include <chrono/physics/ChLinkMotorRotationSpeed.h>
#include <chrono/physics/ChLinkMotorRotationAngle.h>
#include <chrono/physics/ChLinkMotorRotationTorque.h>
#include <chrono/physics/ChLinkMotorLinearSpeed.h>
#include <chrono/physics/ChLinkMotorLinearPosition.h>
#include <chrono/physics/ChLinkMotorLinearForce.h>
#include <chrono/physics/ChLinkUniversal.h>
#include <chrono/physics/ChLinkTSDA.h>
#include <chrono/physics/ChLinkRSDA.h>
#if __has_include(<chrono/physics/ChLinkLockGear.h>)
#include <chrono/physics/ChLinkLockGear.h>
#define WIZ_HAVE_GEAR 1
#endif
#if __has_include(<chrono/physics/ChLinkLockScrew.h>)
#include <chrono/physics/ChLinkLockScrew.h>
#define WIZ_HAVE_SCREW 1
#endif
// 定数関数（モータの目標値）。Chrono 9 は functions/ChFunctionConst.h、
// 旧版は motion_functions/ChFunction_Const.h。
#if __has_include(<chrono/functions/ChFunctionConst.h>)
#include <chrono/functions/ChFunctionConst.h>
#include <chrono/functions/ChFunctionRamp.h>
using WizConstFunction = chrono::ChFunctionConst;
using WizRampFunction = chrono::ChFunctionRamp;
#else
#include <chrono/motion_functions/ChFunction_Const.h>
#include <chrono/motion_functions/ChFunction_Ramp.h>
using WizConstFunction = chrono::ChFunction_Const;
using WizRampFunction = chrono::ChFunction_Ramp;
#endif

#ifdef WIZ_USE_MULTICORE
// Chrono::Multicore: OpenMP-parallel solver (APGD) and collision detection.
#include <chrono_multicore/physics/ChSystemMulticore.h>
#endif

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <Eigen/SVD>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <cstdio>
#include <thread>
#include <type_traits>

using namespace chrono;

bool multicoreAvailable() {
#ifdef WIZ_USE_MULTICORE
    return true;
#else
    return false;
#endif
}

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

// 眠りの計時のやり直しに使う 2 つ（下の resetSleepTimer）。項目ごとの時計
// ChObj::SetChTime と ChBody::TrySleeping は Chrono 9.0 にあるが、無い版では
// 何もしない（そのときは計時のやり直しが効かないだけで、動作は従来どおり）。
template <typename T>
auto setItemTime(T* obj, double t, int) -> decltype(obj->SetChTime(t), void()) {
    obj->SetChTime(t);
}
template <typename T>
void setItemTime(T*, double, long) {}

template <typename T>
auto trySleeping(T* b, int) -> decltype(b->TrySleeping(), void()) {
    b->TrySleeping();
}
template <typename T>
void trySleeping(T*, long) {}

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

// ---- A の導入で足した版差の吸収（同じ SFINAE の手口）------------------------
// 定数関数の値: Chrono 9 は SetConstant、旧版は Set_yconst。
template <typename F>
auto setConstValue(F* f, double v, int) -> decltype(f->SetConstant(v), void()) {
    f->SetConstant(v);
}
template <typename F>
auto setConstValue(F* f, double v, long) -> decltype(f->Set_yconst(v), void()) {
    f->Set_yconst(v);
}

// ボディごとの重力オフ: Chrono 9 は SetUseGravity、旧版は SetNoGravity。
// どちらも無ければ何もしない（false を返して呼び出し側が警告する）。
template <typename B>
auto setUseGravity(B* b, bool on, int) -> decltype(b->SetUseGravity(on), bool()) {
    b->SetUseGravity(on);
    return true;
}
template <typename B>
auto setUseGravity(B* b, bool on, long) -> decltype(b->SetNoGravity(!on), bool()) {
    b->SetNoGravity(!on);
    return true;
}
template <typename B>
bool setUseGravity(B*, bool, ...) {
    return false;
}

// 衝突ファミリ（レイヤ）。SetFamily は両版共通、「そのファミリと当てない」は
// Chrono 9 が DisallowCollisionsWith、旧版が SetFamilyMaskNoCollisionWithFamily。
template <typename M>
auto setFamilyOf(M* model, int family, int) -> decltype(model->SetFamily(family), bool()) {
    model->SetFamily(family);
    return true;
}
template <typename M>
bool setFamilyOf(M*, int, long) {
    return false;
}
template <typename M>
auto disallowFamily(M* model, int family, int)
    -> decltype(model->DisallowCollisionsWith(family), bool()) {
    model->DisallowCollisionsWith(family);
    return true;
}
template <typename M>
auto disallowFamily(M* model, int family, long)
    -> decltype(model->SetFamilyMaskNoCollisionWithFamily(family), bool()) {
    model->SetFamilyMaskNoCollisionWithFamily(family);
    return true;
}
template <typename M>
bool disallowFamily(M*, int, ...) {
    return false;
}

// ChLinkLock の可動範囲: Chrono 9 は LimitRz() が ChLinkLimit& を返し
// SetActive / SetMin / SetMax、旧版は GetLimit_Rz() が ポインタで Set_active /
// Set_min / Set_max。
template <typename L>
auto setLimitRz(L* link, double lo, double hi, int)
    -> decltype(link->LimitRz().SetActive(true), bool()) {
    link->LimitRz().SetActive(true);
    link->LimitRz().SetMin(lo);
    link->LimitRz().SetMax(hi);
    return true;
}
template <typename L>
auto setLimitRz(L* link, double lo, double hi, long)
    -> decltype(link->GetLimit_Rz().Set_active(true), bool()) {
    link->GetLimit_Rz().Set_active(true);
    link->GetLimit_Rz().Set_min(lo);
    link->GetLimit_Rz().Set_max(hi);
    return true;
}
template <typename L>
bool setLimitRz(L*, double, double, ...) {
    return false;
}
template <typename L>
auto setLimitZ(L* link, double lo, double hi, int)
    -> decltype(link->LimitZ().SetActive(true), bool()) {
    link->LimitZ().SetActive(true);
    link->LimitZ().SetMin(lo);
    link->LimitZ().SetMax(hi);
    return true;
}
template <typename L>
auto setLimitZ(L* link, double lo, double hi, long)
    -> decltype(link->GetLimit_Z().Set_active(true), bool()) {
    link->GetLimit_Z().Set_active(true);
    link->GetLimit_Z().Set_min(lo);
    link->GetLimit_Z().Set_max(hi);
    return true;
}
template <typename L>
bool setLimitZ(L*, double, double, ...) {
    return false;
}

// 拘束の反力（ボディ 2 側）。Chrono 9 は GetReaction2() が ChWrenchd
// （force / torque）、旧版は Get_react_force() / Get_react_torque()。
template <typename L>
auto linkReaction(const L* link, double& force, double& torque, int)
    -> decltype(link->GetReaction2().force.Length(), bool()) {
    const auto r = link->GetReaction2();
    force = r.force.Length();
    torque = r.torque.Length();
    return true;
}
template <typename L>
auto linkReaction(const L* link, double& force, double& torque, long)
    -> decltype(link->Get_react_force().Length(), bool()) {
    force = link->Get_react_force().Length();
    torque = link->Get_react_torque().Length();
    return true;
}
template <typename L>
bool linkReaction(const L*, double&, double&, ...) {
    return false;
}

// 歯車: 変速比とシャフトの座標系（ボディローカル）。Chrono 9 は
// SetTransmissionRatio / SetFrameShaft1、旧版は Set_tau / Set_local_shaft1。
template <typename L>
auto setGearParams(L* link, double ratio, const chrono::ChFrame<>& s1,
                   const chrono::ChFrame<>& s2, int)
    -> decltype(link->SetTransmissionRatio(ratio), link->SetFrameShaft1(s1), bool()) {
    link->SetTransmissionRatio(ratio);
    link->SetFrameShaft1(s1);
    link->SetFrameShaft2(s2);
    return true;
}
template <typename L>
auto setGearParams(L* link, double ratio, const chrono::ChFrame<>& s1,
                   const chrono::ChFrame<>& s2, long)
    -> decltype(link->Set_tau(ratio), link->Set_local_shaft1(s1), bool()) {
    link->Set_tau(ratio);
    link->Set_local_shaft1(s1);
    link->Set_local_shaft2(s2);
    return true;
}
template <typename L>
bool setGearParams(L*, double, const chrono::ChFrame<>&, const chrono::ChFrame<>&, ...) {
    return false;
}

// ねじ: 1 回転あたりの前進。Chrono 9 は SetThread、旧版は Set_thread。
template <typename L>
auto setScrewThread(L* link, double pitch, int) -> decltype(link->SetThread(pitch), bool()) {
    link->SetThread(pitch);
    return true;
}
template <typename L>
auto setScrewThread(L* link, double pitch, long) -> decltype(link->Set_thread(pitch), bool()) {
    link->Set_thread(pitch);
    return true;
}
template <typename L>
bool setScrewThread(L*, double, ...) {
    return false;
}

// 接触の座標系の X 軸 = 法線。Chrono 9 は GetAxisX、旧版は Get_A_Xaxis。
template <typename M>
auto matrixXAxis(const M& m, int) -> decltype(m.GetAxisX()) {
    return m.GetAxisX();
}
template <typename M>
auto matrixXAxis(const M& m, long) -> decltype(m.Get_A_Xaxis()) {
    return m.Get_A_Xaxis();
}

// リンクの無効化（破断）。Chrono 9 / 旧版とも SetDisabled。無い版は false。
template <typename L>
auto disableLink(L* link, int) -> decltype(link->SetDisabled(true), bool()) {
    link->SetDisabled(true);
    return true;
}
template <typename L>
bool disableLink(L*, ...) {
    return false;
}

// 診断用: 接触数とリンク数。Chrono 9 は GetNumContacts / GetLinks、旧版は
// GetNcontacts / Get_linklist。無ければ -1。
template <typename C>
auto contactCountOf(C* c, int) -> decltype(c->GetNumContacts(), int()) {
    return int(c->GetNumContacts());
}
template <typename C>
auto contactCountOf(C* c, long) -> decltype(c->GetNcontacts(), int()) {
    return int(c->GetNcontacts());
}
template <typename C>
int contactCountOf(C*, ...) {
    return -1;
}
template <typename S>
auto linkCountOf(S* s, int) -> decltype(s->GetLinks().size(), int()) {
    return int(s->GetLinks().size());
}
template <typename S>
auto linkCountOf(S* s, long) -> decltype(s->Get_linklist().size(), int()) {
    return int(s->Get_linklist().size());
}
template <typename S>
int linkCountOf(S*, ...) {
    return -1;
}

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

// ---- 荷重（ChLoad）の版差 --------------------------------------------------
#ifdef WIZ_HAVE_LOADS
// コンテナから外す。Chrono 9.0 の ChLoadContainer には Remove が無く、
// GetLoadList() が配列をそのまま返す（非 const）ので、そこから消す。
// Remove を持つ版が来たらそちらを使う。
template <typename C>
auto removeLoadFrom(C* c, const std::shared_ptr<chrono::ChLoadBase>& l, int)
    -> decltype(c->Remove(l), bool()) {
    c->Remove(l);
    return true;
}
template <typename C>
auto removeLoadFrom(C* c, const std::shared_ptr<chrono::ChLoadBase>& l, long)
    -> decltype(c->GetLoadList().erase(c->GetLoadList().begin()), bool()) {
    auto& list = c->GetLoadList();
    const auto it = std::find(list.begin(), list.end(), l);
    if (it == list.end()) return false;
    list.erase(it);
    return true;
}
template <typename C>
bool removeLoadFrom(C*, const std::shared_ptr<chrono::ChLoadBase>&, ...) {
    return false;
}
template <typename C>
auto clearLoads(C* c, int) -> decltype(c->RemoveAllLoads(), void()) {
    c->RemoveAllLoads();
}
template <typename C>
auto clearLoads(C* c, long) -> decltype(c->GetLoadList().clear(), void()) {
    c->GetLoadList().clear();
}
template <typename C>
void clearLoads(C*, ...) {}
// ブッシュの荷重の力・トルク（ChLoadBodyBody::GetForce / GetTorque）。
template <typename L>
auto loadReaction(const L* l, double& force, double& torque, int)
    -> decltype(l->GetForce().Length(), l->GetTorque().Length(), bool()) {
    force = l->GetForce().Length();
    torque = l->GetTorque().Length();
    return true;
}
template <typename L>
bool loadReaction(const L*, double&, double&, long) {
    return false;
}
// ブッシュの生成: Chrono 9 は取り付け座標系が ChFrame<>、旧版は ChCoordsys<>。
template <typename L>
auto makeBushing(const std::shared_ptr<chrono::ChBody>& a,
                 const std::shared_ptr<chrono::ChBody>& b, const chrono::ChFrame<>& f,
                 const chrono::ChVector3d& k, const chrono::ChVector3d& c,
                 const chrono::ChVector3d& kr, const chrono::ChVector3d& cr, int)
    -> decltype(chrono_types::make_shared<L>(a, b, f, k, c, kr, cr)) {
    return chrono_types::make_shared<L>(a, b, f, k, c, kr, cr);
}
template <typename L>
auto makeBushing(const std::shared_ptr<chrono::ChBody>& a,
                 const std::shared_ptr<chrono::ChBody>& b, const chrono::ChFrame<>& f,
                 const chrono::ChVector3d& k, const chrono::ChVector3d& c,
                 const chrono::ChVector3d& kr, const chrono::ChVector3d& cr, long)
    -> decltype(chrono_types::make_shared<L>(a, b, chrono::ChCoordsys<>(f.GetPos(), f.GetRot()),
                                             k, c, kr, cr)) {
    return chrono_types::make_shared<L>(a, b, chrono::ChCoordsys<>(f.GetPos(), f.GetRot()), k,
                                        c, kr, cr);
}
#endif

// ---- FEA（ケーブル）の版差 --------------------------------------------------
#ifdef WIZ_HAVE_FEA
template <typename S>
auto setSectionDensity(S* s, double d, int) -> decltype(s->SetDensity(d), void()) {
    s->SetDensity(d);
}
template <typename S>
void setSectionDensity(S*, double, long) {}
// Rayleigh 減衰: Chrono 9 は SetRayleighDamping、旧版は SetBeamRaleyghDamping。
template <typename S>
auto setSectionDamping(S* s, double r, int) -> decltype(s->SetRayleighDamping(r), void()) {
    s->SetRayleighDamping(r);
}
template <typename S>
auto setSectionDamping(S* s, double r, long) -> decltype(s->SetBeamRaleyghDamping(r), void()) {
    s->SetBeamRaleyghDamping(r);
}
template <typename S>
void setSectionDamping(S*, double, ...) {}
// 節点雲の接触面: Chrono 9 は AddAllNodes(mesh, radius)、旧版は AddAllNodes(radius)。
template <typename C, typename M>
auto addAllNodes(C* c, M& mesh, double r, int) -> decltype(c->AddAllNodes(mesh, r), void()) {
    c->AddAllNodes(mesh, r);
}
template <typename C, typename M>
auto addAllNodes(C* c, M&, double r, long) -> decltype(c->AddAllNodes(r), void()) {
    c->AddAllNodes(r);
}
template <typename M>
auto setMeshGravity(M* m, bool on, int) -> decltype(m->SetAutomaticGravity(on), void()) {
    m->SetAutomaticGravity(on);
}
template <typename M>
void setMeshGravity(M*, bool, long) {}
template <typename S, typename M>
auto removeMeshFrom(S* s, const M& mesh, int) -> decltype(s->RemoveMesh(mesh), void()) {
    s->RemoveMesh(mesh);
}
template <typename S, typename M>
auto removeMeshFrom(S* s, const M& mesh, long) -> decltype(s->Remove(mesh), void()) {
    s->Remove(mesh);
}
#endif

// ---- モーダル解析の版差（質量・剛性・拘束ヤコビアンの取り出し）----------
#ifdef WIZ_HAVE_MODAL
template <typename S>
auto systemMatrices(S* s, chrono::ChSparseMatrix& M, chrono::ChSparseMatrix& K,
                    chrono::ChSparseMatrix& Cq, int)
    -> decltype(s->GetMassMatrix(M), s->GetStiffnessMatrix(K),
                s->GetConstraintJacobianMatrix(Cq), bool()) {
    s->GetMassMatrix(M);
    s->GetStiffnessMatrix(K);
    s->GetConstraintJacobianMatrix(Cq);
    return true;
}
template <typename S>
auto systemMatrices(S* s, chrono::ChSparseMatrix& M, chrono::ChSparseMatrix& K,
                    chrono::ChSparseMatrix& Cq, long)
    -> decltype(s->GetMassMatrix(&M), s->GetStiffnessMatrix(&K),
                s->GetConstraintJacobianMatrix(&Cq), bool()) {
    s->GetMassMatrix(&M);
    s->GetStiffnessMatrix(&K);
    s->GetConstraintJacobianMatrix(&Cq);
    return true;
}
template <typename S>
bool systemMatrices(S*, chrono::ChSparseMatrix&, chrono::ChSparseMatrix&,
                    chrono::ChSparseMatrix&, ...) {
    return false;
}
#endif

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

// ワールド座標の座標系をボディのローカルへ（歯車のシャフト座標系用）。
// Chrono の TransformParentToLocal は版で引数が違うので四元数で手計算。
chrono::ChFrame<> worldToBodyLocal(const chrono::ChBody& body, const chrono::ChVector3d& pos,
                                   const chrono::ChQuaternion<>& rot) {
    const chrono::ChQuaternion<> q = body.GetRot();
    const chrono::ChVector3d lp = q.RotateBack(pos - body.GetPos());
    const chrono::ChQuaternion<> lr = q.GetConjugate() * rot;
    return chrono::ChFrame<>(lp, lr);
}

// 衝突ファミリ: 同じソフトボディの粒子どうしを当てない（ばねで結んだ隣が
// 接触で押し合うと硬さが二重になる）。Chrono 9 は SetFamily +
// DisallowCollisionsWith、旧版は SetFamilyMaskNoCollisionWithFamily。
// どちらも無い版では粒子どうしも当たる（半径をセルより小さくしてあるので
// 静止状態では触れない）。
template <typename M>
auto setNoSelfCollision(M* model, int family, int)
    -> decltype(model->SetFamily(family), model->DisallowCollisionsWith(family),
                void()) {
    model->SetFamily(family);
    model->DisallowCollisionsWith(family);
}
template <typename M>
auto setNoSelfCollision(M* model, int family, long)
    -> decltype(model->SetFamily(family),
                model->SetFamilyMaskNoCollisionWithFamily(family), void()) {
    model->SetFamily(family);
    model->SetFamilyMaskNoCollisionWithFamily(family);
}
template <typename M>
bool setNoSelfCollision(M*, int, ...) {
    return false;
}
// 上の 2 つは void を返すので、戻り値で「できたか」を判定できるように包む。
template <typename M>
bool applyNoSelfCollision(M* model, int family) {
    using R = decltype(setNoSelfCollision(model, family, 0));
    if constexpr (std::is_same<R, bool>::value) {
        return setNoSelfCollision(model, family, 0);
    } else {
        setNoSelfCollision(model, family, 0);
        return true;
    }
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

// 接触コンテナの走査で、ボディ同士の接触だけを physId のペア + 力・法線・
// めり込みで集める。ChContactable* から ChBody* へは dynamic_cast（Chrono
// 自身が SumAllContactForces で使っている手）。同じペアに複数の接触点が
// あるので、ペアだけ欲しい側（activeContactPairs）が 1 本化する。
// react_forces は接触座標系（X = 法線）の力 (N)。NSC でも Chrono が
// 力積 / dt に直して渡してくる。
class ContactCollector : public chrono::ChContactContainer::ReportContactCallback {
public:
    ContactCollector(const std::unordered_map<const chrono::ChBody*, std::size_t>& index,
                     std::vector<ContactInfo>& out)
        : index_(index), out_(out) {}

    bool OnReportContact(const ChVector3d&, const ChVector3d&,
                         const ChMatrix33<>& plane, const double& distance,
                         const double&, const ChVector3d& reactForce,
                         const ChVector3d&, ChContactable* objA,
                         ChContactable* objB) override {
        const auto* bodyA = dynamic_cast<const ChBody*>(objA);
        const auto* bodyB = dynamic_cast<const ChBody*>(objB);
        if (!bodyA || !bodyB) return true;  // ボディ以外（FEA 等）は対象外
        const auto ia = index_.find(bodyA);
        const auto ib = index_.find(bodyB);
        if (ia == index_.end() || ib == index_.end()) return true;
        ContactInfo c;
        c.a = ia->second;
        c.b = ib->second;
        if (c.a == c.b) return true;
        const ChVector3d n = matrixXAxis(plane, 0);  // A から B へ向く法線
        if (c.a > c.b) {
            std::swap(c.a, c.b);
            c.nx = -n.x(); c.ny = -n.y(); c.nz = -n.z();
        } else {
            c.nx = n.x(); c.ny = n.y(); c.nz = n.z();
        }
        c.force = reactForce.Length();
        c.normalForce = reactForce.x();
        c.depth = -distance;  // Chrono は「重なり = 負の距離」
        out_.push_back(c);
        return true;  // 続けて最後まで走査する
    }

private:
    const std::unordered_map<const chrono::ChBody*, std::size_t>& index_;
    std::vector<ContactInfo>& out_;
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

// +X をこの向きに合わせる回転。ChLinkMotorLinear* は X 軸方向に動くので、
// 直動モータだけはこちら（ChLinkLockPrismatic は Z）。
chrono::ChQuaternion<> quatFromXAxis(const chrono::ChVector3d& axis) {
    double ax = axis.x(), ay = axis.y(), az = axis.z();
    const double n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n < 1e-9) return chrono::ChQuaternion<>(1, 0, 0, 0);
    ax /= n;
    ay /= n;
    az /= n;
    const double c = ax;  // dot((1,0,0), axis)
    if (c < -0.999999) {  // 真後ろ: Y 軸まわりに 180 度
        return chrono::ChQuaternion<>(0, 0, 1, 0);
    }
    // 最短回転。cross((1,0,0), axis) = (0, -az, ay)
    chrono::ChQuaternion<> q(1.0 + c, 0.0, -az, ay);
    q.Normalize();
    return q;
}

// 零ベクトルでない軸か。
bool axisUsable(const chrono::ChVector3d& a) {
    return a.x() * a.x() + a.y() * a.y() + a.z() * a.z() > 1e-12;
}

}  // namespace

const char* PhysicsWorld::backendName() const {
    return backend_ == PhysicsBackend::Multicore ? "multicore" : "core";
}

const char* PhysicsWorld::contactName() const {
    return contact_ == ContactMethod::SMC ? "smc" : "nsc";
}

void PhysicsWorld::registerBody(const std::shared_ptr<chrono::ChBody>& body) {
    bodyIndex_[body.get()] = bodies_.size();
    bodies_.push_back(body);
    active_.push_back(true);
    softOf_.push_back(kNoSoft);  // addSoftBody が粒子ぶんを後から書き換える
    alias_.push_back(kNoSoft);
    mats_.push_back(nullptr);  // finishBody が専用材質なら書く
    options_.push_back(BodyOptions{});
    loadsOf_.emplace_back();
    bindCollision(body);
    resetSleepTimer(*body);  // 新規ボディの計時は 0 = 最初のステップで眠る
}

// ---- 接触の物性・レイヤ・重力（A の導入）------------------------------------
// 材質は contact_ で NSC / SMC のどちらか。共通の欄（摩擦・反発）は基底に、
// 転がり・スピン・粘着は NSC に、ヤング率・ポアソン比・付着は SMC にある。

void PhysicsWorld::applySurface(ChContactMaterial& m, const BodyOptions& o) const {
    m.SetFriction(o.friction >= 0.0f ? o.friction : friction_);
    m.SetRestitution(o.restitution >= 0.0f ? o.restitution : restitution_);
    if (auto* n = dynamic_cast<ChContactMaterialNSC*>(&m)) {
        n->SetRollingFriction(o.rolling >= 0.0f ? o.rolling : rolling_);
        n->SetSpinningFriction(o.rolling >= 0.0f ? o.rolling : spinning_);
        n->SetCohesion(o.cohesion);
    } else if (auto* s = dynamic_cast<ChContactMaterialSMC*>(&m)) {
        s->SetYoungModulus(float(o.young >= 0.0f ? o.young : smcYoung_));
        s->SetPoissonRatio(float(o.poisson >= 0.0f ? o.poisson : smcPoisson_));
        s->SetAdhesion(o.cohesion);  // 粘着 = SMC の付着力（同じ意味の欄）
    }
}

std::shared_ptr<ChContactMaterial> PhysicsWorld::makeMaterial() const {
    if (contact_ == ContactMethod::SMC) {
        return chrono_types::make_shared<ChContactMaterialSMC>();
    }
    return chrono_types::make_shared<ChContactMaterialNSC>();
}

std::shared_ptr<ChContactMaterial> PhysicsWorld::materialFor(const BodyOptions& o) {
    const bool custom = o.friction >= 0.0f || o.restitution >= 0.0f ||
                        o.rolling >= 0.0f || o.cohesion != 0.0f || o.young >= 0.0f ||
                        o.poisson >= 0.0f;
    if (!custom) return mat_;
    auto m = makeMaterial();
    applySurface(*m, o);
    return m;
}

void PhysicsWorld::setSmcDefaults(double young, double poisson) {
    smcYoung_ = young;
    smcPoisson_ = poisson;
    if (contact_ != ContactMethod::SMC) return;
    applySurface(*mat_, BodyOptions{});
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        if (mats_[i]) applySurface(*mats_[i], options_[i]);
    }
}

void PhysicsWorld::prepareBody(ChBody& body, const BodyOptions& options) {
    // 衝突レイヤ（ファミリ 0〜7）。レイヤ 0 で除外無しなら既定のまま触らない。
    // 衝突系へ登録する前（AddBody の前）に呼ぶこと - 登録後の変更は Bullet が
    // モデルを Remove / Add し直し、Multicore の衝突系は Remove が未実装で
    // 例外を投げる。
    if (options.layer != 0 || options.nocollide != 0) {
        static bool warned = false;
        bool ok = true;
        if (auto model = body.GetCollisionModel()) {
            const int layer = std::max(0, std::min(kCollisionLayerCount - 1, options.layer));
            // レイヤ 0 は Chrono の既定ファミリなので触らない（ソフトボディの
            // 粒子は先に 8〜15 を付けてここへ来る - 上書きしない）。
            if (layer != 0) ok = setFamilyOf(&*model, layer, 0) && ok;
            for (int L = 0; L < kCollisionLayerCount; ++L) {
                if (options.nocollide & (1u << L)) ok = disallowFamily(&*model, L, 0) && ok;
            }
        }
        if (!ok && !warned) {
            warned = true;
            LOGW("physics", "this Chrono has no collision family API - collision "
                            "layers are ignored");
        }
    }
    // 重力オフ: Chrono に per-body の口があれば使う（9.0 には無い）。無くても
    // step() が重力ぶんを毎ステップ打ち消すので、どちらでも効く。
    if (!options.gravity) setUseGravity(&body, false, 0);
}

void PhysicsWorld::finishBody(const std::shared_ptr<chrono::ChBody>& body,
                              const std::shared_ptr<chrono::ChContactMaterial>& mat,
                              const BodyOptions& options) {
    registerBody(body);
    const std::size_t id = bodies_.size() - 1;
    mats_[id] = (mat == mat_) ? nullptr : mat;
    options_[id] = options;
    attachBodyLoads(id, options);
}

// 定常荷重（<geom force torque>）。荷重コンテナ経由 = Core だけが積分に
// 取り込む（Multicore は IntLoadResidual_F を使わない。ばねと同じ）。
// Multicore で頼まれたら Scene が先に Core へ切り替えているはずだが、
// 万一のときは黙って捨てず 1 回だけ伝える。
void PhysicsWorld::attachBodyLoads(std::size_t id, const BodyOptions& options) {
    const bool hasForce = options.force.Length2() > 0.0;
    const bool hasTorque = options.torque.Length2() > 0.0;
    if (!hasForce && !hasTorque) return;
    static bool warned = false;
#ifdef WIZ_HAVE_LOADS
    if (backend_ == PhysicsBackend::Multicore || !bodyLoads_) {
        if (!warned) {
            warned = true;
            LOGW("physics", "constant body loads need the core backend - ignored");
        }
        return;
    }
    auto& body = bodies_[id];
    if (hasForce) {
        auto f = chrono_types::make_shared<ChLoadBodyForce>(body, options.force,
                                                            /*local_force*/ false, VNULL,
                                                            /*local_point*/ true);
        bodyLoads_->Add(f);
        loadsOf_[id].push_back(f);
    }
    if (hasTorque) {
        auto t = chrono_types::make_shared<ChLoadBodyTorque>(body, options.torque,
                                                             /*local_torque*/ false);
        bodyLoads_->Add(t);
        loadsOf_[id].push_back(t);
    }
#else
    if (!warned) {
        warned = true;
        LOGW("physics", "this Chrono build has no ChLoad - constant body loads are ignored");
    }
#endif
}

void PhysicsWorld::setBodyGravity(std::size_t id, bool enabled) {
    if (id >= bodies_.size()) return;
    options_[id].gravity = enabled;
    if (SoftBody* soft = softOfRoot(id)) {
        for (const std::size_t p : soft->particles) setUseGravity(bodies_[p].get(), enabled, 0);
        return;
    }
    setUseGravity(bodies_[id].get(), enabled, 0);
    wakeUp(bodies_[id].get(), 0);
}

bool PhysicsWorld::setBodySurface(std::size_t id, const BodyOptions& options) {
    if (id >= bodies_.size()) return false;
    const bool custom = options.friction >= 0.0f || options.restitution >= 0.0f ||
                        options.rolling >= 0.0f || options.cohesion != 0.0f;
    // 物性以外（レイヤ・重力）は options_ に写すだけ。レイヤは衝突モデルの
    // 作り直しが要るので呼び出し側が判断する。
    BodyOptions& cur = options_[id];
    cur.friction = options.friction;
    cur.restitution = options.restitution;
    cur.rolling = options.rolling;
    cur.cohesion = options.cohesion;
    if (SoftBody* soft = softOfRoot(id)) {
        bool ok = true;
        for (const std::size_t p : soft->particles) {
            if (mats_[p]) applySurface(*mats_[p], cur);
            else if (custom) ok = false;
            options_[p].friction = cur.friction;
            options_[p].restitution = cur.restitution;
            options_[p].rolling = cur.rolling;
            options_[p].cohesion = cur.cohesion;
        }
        return ok;
    }
    if (mats_[id]) {
        applySurface(*mats_[id], cur);
        return true;
    }
    // 共有材質のまま「シーン任せ」に戻すだけなら、何も変えずに済む。
    return !custom;
}

void PhysicsWorld::setMaterialCombine(CombineMode mode) {
    // min は Chrono の既定そのもの。既定のままなら Chrono 自身の戦略オブジェクト
    // を残す（差し替えるのは average / max を頼まれたときだけ）。
    if (mode == combine_) return;
    combine_ = mode;
    sys_->SetMaterialCompositionStrategy(std::make_unique<CombineStrategy>(mode));
    LOGI("physics", "material composition: %s",
         mode == CombineMode::Average ? "average" : mode == CombineMode::Max ? "max" : "min");
}

void PhysicsWorld::setAlias(std::size_t id, std::size_t owner) {
    if (id >= alias_.size() || owner >= bodies_.size() || id == owner) return;
    alias_[id] = owner;
}

// ---- ソフトボディ -----------------------------------------------------------

std::size_t PhysicsWorld::representative(std::size_t id) const {
    if (id >= softOf_.size() || softOf_[id] == kNoSoft) return id;
    return softBodies_[softOf_[id]].root;
}

const PhysicsWorld::SoftBody* PhysicsWorld::softOfRoot(std::size_t id) const {
    if (id >= softOf_.size() || softOf_[id] == kNoSoft) return nullptr;
    const SoftBody& s = softBodies_[softOf_[id]];
    return s.root == id ? &s : nullptr;
}

PhysicsWorld::SoftBody* PhysicsWorld::softOfRoot(std::size_t id) {
    if (id >= softOf_.size() || softOf_[id] == kNoSoft) return nullptr;
    SoftBody& s = softBodies_[softOf_[id]];
    return s.root == id ? &s : nullptr;
}

bool PhysicsWorld::isSoftBody(std::size_t id) const {
    return softOfRoot(id) != nullptr;
}

std::size_t PhysicsWorld::softParticleCount(std::size_t id) const {
    const SoftBody* s = softOfRoot(id);
    return s ? s->particles.size() : 0;
}

void PhysicsWorld::softParticlePositions(std::size_t id,
                                         std::vector<float>& out) const {
    out.clear();
    const SoftBody* s = softOfRoot(id);
    if (!s) return;
    out.reserve(s->particles.size() * 3);
    for (const std::size_t p : s->particles) {
        const ChVector3d v = bodies_[p]->GetPos();
        out.push_back(float(v.x()));
        out.push_back(float(v.y()));
        out.push_back(float(v.z()));
    }
}

std::size_t PhysicsWorld::addSoftBody(const SoftBodySpec& spec,
                                      const ChVector3d& pos,
                                      const ChQuaternion<>& rot) {
    if (spec.rest.empty()) return static_cast<std::size_t>(-1);

    SoftBody soft;
    soft.rest = spec.rest;
    soft.springs = spec.springs;
    soft.iterations = std::max(1, spec.iterations);
    soft.restCentroid = ChVector3d(0, 0, 0);
    for (const auto& r : soft.rest) soft.restCentroid += r;
    soft.restCentroid *= 1.0 / double(soft.rest.size());

    // 衝突ファミリは 8〜15 を順に使う（0〜7 は剛体の衝突レイヤ）。9 個目
    // 以降は番号を使い回すので、その組は互いに当たらない - 現実的な数では
    // 起きない。剛体側の nocollide でソフトボディを除外することはできない
    // （レイヤは 0〜7 だけ）。
    const int family = kCollisionLayerCount + int(softBodies_.size() % kCollisionLayerCount);
    const std::size_t softIndex = softBodies_.size();
    const double radius = std::max(spec.radius, 1e-4);
    const double volume = (4.0 / 3.0) * 3.14159265358979323846 * radius * radius * radius;
    const double density = std::max(spec.particleMass, 1e-9) / volume;
    // 物性は粒子全部で 1 個の材質を共有する（専用が要るときだけ作る）。
    const auto mat = materialFor(spec.options);
    // 粒子のレイヤは家族番号で決まるので、剛体レイヤは付けない（nocollide と
    // 重力だけ効かせる）。
    BodyOptions particleOpts = spec.options;
    particleOpts.layer = 0;

    bool familyOk = true;
    for (std::size_t i = 0; i < spec.rest.size(); ++i) {
        auto b = chrono_types::make_shared<ChBodyEasySphere>(
            radius, density, /*visualize*/ false, /*collide*/ true, mat);
        b->SetPos(pos + rot.Rotate(spec.rest[i]));
        b->SetRot(rot);
        b->SetFixed(spec.fixed);
        b->EnableCollision(true);
        // 粒子は眠らせない: 一部だけ眠ると、ばねの相手が動いても起きずに
        // 形が固まる（起こすのは接触だけ、という Chrono の約束のため）。
        allowSleeping(b.get(), false, 0);
        if (auto model = b->GetCollisionModel()) {
            if (!applyNoSelfCollision(&*model, family)) familyOk = false;
        }
        prepareBody(*b, particleOpts);
        sys_->AddBody(b);
        finishBody(b, mat, particleOpts);
        const std::size_t id = bodies_.size() - 1;
        softOf_[id] = softIndex;
        soft.particles.push_back(id);
    }
    soft.root = soft.particles.front();
    softBodies_.push_back(std::move(soft));
    if (!familyOk && softIndex == 0) {
        LOGW("physics",
             "soft body: this Chrono has no collision family API - particles of "
             "the same body also collide with each other");
    }
    LOGI("physics", "soft body #%zu: %zu particles, %zu springs (root body %zu)",
         softIndex, spec.rest.size(), spec.springs.size(),
         softBodies_.back().root);
    return softBodies_.back().root;
}

void PhysicsWorld::placeSoftBody(SoftBody& soft, const ChVector3d& pos,
                                 const ChQuaternion<>& rot) {
    for (std::size_t i = 0; i < soft.particles.size(); ++i) {
        auto& b = bodies_[soft.particles[i]];
        b->SetPos(pos + rot.Rotate(soft.rest[i]));
        b->SetRot(rot);
        b->ForceToRest();
        wakeUp(b.get(), 0);
    }
}

// 粒子群に「剛体だったらどこにあるか」を当てはめる。重心はそのまま平均、
// 回転は静止形状との相関行列の極分解（SVD で U V^T）= 最小二乗の回転。
// 選択の当たり判定・ギズモ・引っぱり線・保存される姿勢がこれを使う。
BodyTransform PhysicsWorld::softTransform(const SoftBody& soft) const {
    const std::size_t n = soft.particles.size();
    ChVector3d c(0, 0, 0);
    for (const std::size_t p : soft.particles) c += bodies_[p]->GetPos();
    c *= 1.0 / double(n);

    Eigen::Matrix3d A = Eigen::Matrix3d::Zero();
    for (std::size_t i = 0; i < n; ++i) {
        const ChVector3d p = bodies_[soft.particles[i]]->GetPos() - c;
        const ChVector3d q = soft.rest[i] - soft.restCentroid;
        const Eigen::Vector3d pv(p.x(), p.y(), p.z());
        const Eigen::Vector3d qv(q.x(), q.y(), q.z());
        A += pv * qv.transpose();
    }
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(A, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d U = svd.matrixU();
    const Eigen::Matrix3d V = svd.matrixV();
    Eigen::Matrix3d R = U * V.transpose();
    if (R.determinant() < 0.0) {  // 鏡映になったら最小の特異値の軸を反転
        U.col(2) *= -1.0;
        R = U * V.transpose();
    }
    const Eigen::Quaterniond q(R);
    // 粒子 ≈ pos + R * rest なので、pos = 重心 - R * 静止重心。
    const Eigen::Vector3d c0(soft.restCentroid.x(), soft.restCentroid.y(),
                             soft.restCentroid.z());
    const Eigen::Vector3d pos = Eigen::Vector3d(c.x(), c.y(), c.z()) - R * c0;
    return {pos.x(), pos.y(), pos.z(), q.w(), q.x(), q.y(), q.z()};
}

// ばね 1 本を implicit Euler で解いて速度を直す（vehicle のクラッチと同じ
// 「陰解法の粘性要素」の考え方をばねに広げたもの）:
//   相対座標 x（伸び）、相対速度 v、換算質量 m とすると
//   m v' = v - dt (k (x + dt v') + c v')
//   → v' = (v - dt k x / m) / (1 + dt (c + dt k) / m)
// 1 本ずつは無条件安定で、ガウス・ザイデルで回すと網全体も暴れない。
// 位置はステップ開始時の値、速度は直しながら読む。
void PhysicsWorld::solveSoftSprings(SoftBody& soft, double dt) {
    if (!soft.active || dt <= 0.0 || soft.springs.empty()) return;
    const std::size_t n = soft.particles.size();
    std::vector<ChVector3d> pos(n), vel(n);
    std::vector<double> invMass(n, 0.0);
    bool anyFree = false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& b = bodies_[soft.particles[i]];
        pos[i] = b->GetPos();
        vel[i] = getLinVel(b.get(), 0);
        const double m = b->GetMass();
        invMass[i] = (b->IsFixed() || m <= 0.0) ? 0.0 : 1.0 / m;
        if (invMass[i] > 0.0) anyFree = true;
    }
    if (!anyFree) return;  // 固定されたソフトボディは動かない

    for (int iter = 0; iter < soft.iterations; ++iter) {
        for (const auto& s : soft.springs) {
            const std::size_t a = s.a, b = s.b;
            if (a >= n || b >= n) continue;
            const double w = invMass[a] + invMass[b];
            if (w <= 0.0) continue;
            const ChVector3d d = pos[b] - pos[a];
            const double len = d.Length();
            if (len < 1e-9) continue;
            const ChVector3d dir = d * (1.0 / len);
            const double meff = 1.0 / w;
            const double vrel = (vel[b] - vel[a]).Dot(dir);
            const double x = len - s.rest;
            const double denom = 1.0 + dt * (s.c + dt * s.k) / meff;
            const double vNew = (vrel - dt * s.k * x / meff) / denom;
            const double impulse = meff * (vNew - vrel);
            vel[a] -= dir * (impulse * invMass[a]);
            vel[b] += dir * (impulse * invMass[b]);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (invMass[i] <= 0.0) continue;
        setLinVel(bodies_[soft.particles[i]].get(), vel[i], 0);
    }
}

// Chrono 9 は衝突モデルを「衝突系の初期化（最初の DoStepDynamics）で
// まとめて登録（BindAll）」する。それ以降に AddBody したボディは
// ChSystem::AddBody も ChSystemMulticore::AddBody も衝突系へ渡さないので、
// 自分で BindItem を呼ばないと**形はあるのに当たり判定が無い**ボディに
// なる（エディタで足した箱が床を抜ける・シーン読込で作り直した床に何も
// 乗らない、の正体。Core / Multicore どちらも同じ）。初期化前に足した
// ぶんは BindAll が拾うので触らない（Multicore の Add は二重登録を
// 想定していない）。
void PhysicsWorld::bindCollision(const std::shared_ptr<chrono::ChBody>& body) {
    auto coll = sys_->GetCollisionSystem();
    if (!coll || !coll->IsInitialized()) return;
    const auto model = body->GetCollisionModel();
    if (!model || model->HasImplementation() || !body->IsCollisionEnabled()) {
        return;
    }
    coll->BindItem(body);
}

std::vector<ContactInfo> PhysicsWorld::activeContacts() const {
    std::vector<ContactInfo> contacts;
    const auto container = sys_->GetContactContainer();
    if (!container) return contacts;
    auto collector = chrono_types::make_shared<ContactCollector>(bodyIndex_, contacts);
    container->ReportAllContacts(collector);
    // ソフトボディの粒子は代表番号に、子ボディ（setAlias）は持ち主に寄せる
    // （Scene はその番号しか知らない）。同じ物どうしは一致するので落ちる。
    auto owner = [this](std::size_t id) {
        id = representative(id);
        return (id < alias_.size() && alias_[id] != kNoSoft) ? alias_[id] : id;
    };
    std::size_t kept = 0;
    for (auto& c : contacts) {
        const std::size_t a = owner(c.a);
        const std::size_t b = owner(c.b);
        if (a == b) continue;
        ContactInfo out = c;
        out.a = a;
        out.b = b;
        if (a > b) {  // 番号順に揃え、法線も a → b のまま保つ
            std::swap(out.a, out.b);
            out.nx = -out.nx; out.ny = -out.ny; out.nz = -out.nz;
        }
        contacts[kept++] = out;
    }
    contacts.resize(kept);
    return contacts;
}

std::vector<std::pair<std::size_t, std::size_t>>
PhysicsWorld::activeContactPairs() const {
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (const ContactInfo& c : activeContacts()) pairs.push_back({c.a, c.b});
    // 1 ペアに接触点は複数あるのが普通（箱同士は最大 4 点）。ここで 1 本化。
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

PhysicsWorld::PhysicsWorld(PhysicsBackend backend, ContactMethod contact) {
    backend_ = backend;
    contact_ = contact;
    createSystem();
}

// 系をまるごと作り直す（B の自動切替: Multicore ⇄ Core、NSC ⇄ SMC）。
// ボディ・ジョイント・ケーブル・荷重は全部捨てる - 番号は無効になるので、
// Scene が設計値から作り直す（Scene::rebuildPhysicsWorld）。
void PhysicsWorld::recreate(PhysicsBackend backend, ContactMethod contact) {
    joints_.clear();
    brokenPending_.clear();
    cables_.clear();
    bodies_.clear();
    mats_.clear();
    options_.clear();
    loadsOf_.clear();
    bodyIndex_.clear();
    active_.clear();
    softOf_.clear();
    alias_.clear();
    softBodies_.clear();
    jointLoads_.reset();
    bodyLoads_.reset();
    diagSteps_ = 0;
    diagNanReported_ = false;
    backend_ = backend;
    contact_ = contact;
    sys_.reset();  // 古い系を先に壊す（Multicore の OpenMP プールを 2 つ持たない）
    createSystem();
    LOGI("physics", "world recreated: backend=%s contact=%s", backendName(), contactName());
}

void PhysicsWorld::createSystem() {
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

    // Sleeping is off until the Scene enables it (see setSleepingEnabled).
    // Note: Chrono::Multicore does not support sleeping - the call is harmless
    // there, but sleepingCount() will simply stay at 0.
    allowSleeping(sys_.get(), sleepingEnabled_, 0);

    mat_ = makeMaterial();
    // No bounce: even a little restitution keeps settled boxes micro-bouncing.
    applySurface(*mat_, BodyOptions{});

    // 荷重コンテナ（ChLoad）。Core だけが積分に取り込むので、そちらでだけ作る。
#ifdef WIZ_HAVE_LOADS
    if (backend_ != PhysicsBackend::Multicore) {
        jointLoads_ = chrono_types::make_shared<ChLoadContainer>();
        bodyLoads_ = chrono_types::make_shared<ChLoadContainer>();
        sys_->Add(jointLoads_);
        sys_->Add(bodyLoads_);
    }
#endif

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

void PhysicsWorld::setGravity(double gx, double gy, double gz) {
    gravity_ = ChVector3d(gx, gy, gz);
    sys_->SetGravitationalAcceleration(gravity_);
}

void PhysicsWorld::setBodyVelocity(std::size_t id, const ChVector3d& linear,
                                   const ChVector3d& angular) {
    if (id >= bodies_.size()) return;
    if (SoftBody* soft = softOfRoot(id)) {
        // 粒子群には並進だけ（回転は粒子の配置が決める）。
        for (const std::size_t p : soft->particles) {
            if (bodies_[p]->IsFixed()) continue;
            setLinVel(bodies_[p].get(), linear, 0);
        }
        return;
    }
    auto& b = bodies_[id];
    if (b->IsFixed()) return;  // 固定ボディの速度は「動く床」になるので触らない
    wakeUp(b.get(), 0);
    setLinVel(b.get(), linear, 0);
    setAngVel(b.get(), angular, 0);
}

bool PhysicsWorld::setIntegrator(const std::string& name) {
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

bool PhysicsWorld::setSolver(const std::string& name) {
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

void PhysicsWorld::setSleepingEnabled(bool enabled, float seconds,
                                      float minLinVel, float minAngVel) {
    sleepingEnabled_ = enabled;
    sleepSeconds_ = seconds;
    sleepMinLinVel_ = minLinVel;
    sleepMinAngVel_ = minAngVel;

    allowSleeping(sys_.get(), enabled, 0);
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        auto& b = bodies_[i];
        // ソフトボディの粒子は常に起きている（addSoftBody 参照）。
        if (softOf_[i] != kNoSoft) {
            allowSleeping(b.get(), false, 0);
            continue;
        }
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

// 眠りの計時をやり直す（Core。Multicore は眠らない）。
// Chrono の sleep_starttime（protected）は「最後に速度がしきい値を超えた
// 時刻」で、TrySleeping() の中でしか更新されない。置き直して静止させた
// ボディや作ったばかりのボディはこれが前回の走行のまま（新規は 0）なので、
// 最初のステップの ManageSleepingBodies（積分の前に走る）が
// (now - starttime) > sleep_time を満たし、宙に浮いたまま眠る（Core の実機で
// 確認: シミュレート開始で一部の物がその場で止まる）。しきい値を超える
// 速度を一瞬与えて TrySleeping を呼び、時刻を今にしてから速度を戻す。
void PhysicsWorld::resetSleepTimer(ChBody& b) {
    if (!sleepingEnabled_ || !b.IsSleepingAllowed() || b.IsFixed() || b.IsSleeping()) return;
    // 項目の時計は Update() でしか進まないので、作ったばかりのボディは 0。
    setItemTime(&b, sys_->GetChTime(), 0);
    const ChVector3d v = getLinVel(&b, 0);
    setLinVel(&b, ChVector3d(1e6, 0, 0), 0);
    trySleeping(&b, 0);  // 速度が大きい = 眠れない = sleep_starttime を今にする
    setLinVel(&b, v, 0);
}

void PhysicsWorld::wakeAll() {
    for (auto& b : bodies_) {
        wakeUp(b.get(), 0);
        resetSleepTimer(*b);
    }
    // Waking from outside ManageSleepingBodies() changes how many bodies the
    // solver has to handle, and Chrono only rebuilds that layout in Setup().
    // Without this the "woken" bodies can stay out of the solve - i.e. they
    // would not fall after a reset.
    sys_->Setup();
}

void PhysicsWorld::setCollisionTolerances(double envelope, double margin) {
    envelope_ = envelope;
    margin_ = margin;
#ifdef WIZ_USE_MULTICORE
    // The multicore collision system keeps the envelope in its settings.
    if (auto* mc = dynamic_cast<ChSystemMulticore*>(sys_.get())) {
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

void PhysicsWorld::setContactRecoverySpeed(double recoverySpeed) {
    recoverySpeed_ = recoverySpeed;
#ifdef WIZ_USE_MULTICORE
    if (auto* mc = dynamic_cast<ChSystemMulticore*>(sys_.get())) {
        mc->GetSettings()->solver.contact_recovery_speed = recoverySpeed;
        return;
    }
#endif
    sys_->SetMaxPenetrationRecoverySpeed(recoverySpeed);
}

void PhysicsWorld::applyForce(std::size_t id, const ChVector3d& force,
                              double dt) {
    if (id >= bodies_.size()) return;
    if (SoftBody* soft = softOfRoot(id)) {
        // 全体の質量で速度変化を出し、全粒子へ同じだけ足す（剛体に力を
        // 掛けたときと同じ並進になる）。
        const double total = bodyMass(id);
        if (total <= 0.0) return;
        const ChVector3d dv = force * (dt / total);
        for (const std::size_t p : soft->particles) {
            auto& pb = bodies_[p];
            if (pb->IsFixed()) continue;
            setLinVel(pb.get(), getLinVel(pb.get(), 0) + dv, 0);
        }
        return;
    }
    auto& b = bodies_[id];
    const double mass = b->GetMass();
    // 固定ボディには掛けない（applyForceAtPoint と同じ）。固定の物は位置を
    // 積分しないので速度を足しても動かないが、**足した速度は残る**。Multicore
    // の接触拘束は固定ボディの速度も右辺に入れる（= 動く床として扱う）ので、
    // 固定の台を掴んで毎ステップ速度を積むと、台は動かないまま上の物が
    // 全部その速度で押し出されて発散する。Core は非アクティブな変数を
    // 無視するので出ないが、速度が溜まるのは同じ。
    if (mass <= 0.0 || b->IsFixed()) return;

    // A sleeping body ignores everything until something touches it, so wake it
    // first - otherwise pushing a settled box does nothing at all.
    if (b->IsSleeping()) wakeUp(b.get(), 0);

    const ChVector3d v = getLinVel(b.get(), 0);
    setLinVel(b.get(), v + force * (dt / mass), 0);
}

chrono::ChVector3d PhysicsWorld::bodyVelocity(std::size_t id) const {
    if (id >= bodies_.size()) return chrono::ChVector3d(0, 0, 0);
    if (const SoftBody* soft = softOfRoot(id)) {
        ChVector3d v(0, 0, 0);
        for (const std::size_t p : soft->particles) v += getLinVel(bodies_[p].get(), 0);
        return v * (1.0 / double(soft->particles.size()));
    }
    return getLinVel(bodies_[id].get(), 0);
}

void PhysicsWorld::applyForceAtPoint(std::size_t id, const ChVector3d& force,
                                     const ChVector3d& point, double dt) {
    if (id >= bodies_.size()) return;
    if (isSoftBody(id)) {
        // ソフトボディに「点」の力は無い（回転は粒子の配置が決める）。
        // 並進だけ全体へ。
        applyForce(id, force, dt);
        return;
    }
    auto& b = bodies_[id];
    const double mass = b->GetMass();
    if (mass <= 0.0 || b->IsFixed()) return;
    if (b->IsSleeping()) wakeUp(b.get(), 0);

    const ChVector3d impulse = force * dt;
    setLinVel(b.get(), getLinVel(b.get(), 0) + impulse / mass, 0);

    // 角運動量の変化 L = r × J をワールド → ローカルへ回し、ローカルの逆慣性
    // テンソルを掛けてからワールドへ戻す（ChBody の慣性はローカル表現）。
    const ChVector3d r = point - b->GetPos();
    const ChVector3d angImpulseW = r.Cross(impulse);
    const ChQuaternion<> q = b->GetRot();
    const ChVector3d angImpulseL = q.RotateBack(angImpulseW);
    const ChVector3d dwL = b->GetInvInertia() * angImpulseL;
    const ChVector3d dwW = q.Rotate(dwL);
    setAngVel(b.get(), getAngVel(b.get(), 0) + dwW, 0);
}

chrono::ChVector3d PhysicsWorld::bodyAngularVelocity(std::size_t id) const {
    if (id >= bodies_.size()) return chrono::ChVector3d(0, 0, 0);
    if (isSoftBody(id)) return chrono::ChVector3d(0, 0, 0);
    return getAngVel(bodies_[id].get(), 0);
}

chrono::ChVector3d PhysicsWorld::bodyPointVelocity(
    std::size_t id, const chrono::ChVector3d& point) const {
    if (id >= bodies_.size()) return chrono::ChVector3d(0, 0, 0);
    if (isSoftBody(id)) return bodyVelocity(id);
    const auto& b = bodies_[id];
    const ChVector3d r = point - b->GetPos();
    return getLinVel(b.get(), 0) + getAngVel(b.get(), 0).Cross(r);
}

double PhysicsWorld::bodyMass(std::size_t id) const {
    if (id >= bodies_.size()) return 0.0;
    if (const SoftBody* soft = softOfRoot(id)) {
        double total = 0.0;
        for (const std::size_t p : soft->particles) total += bodies_[p]->GetMass();
        return total;
    }
    return bodies_[id]->GetMass();
}

void PhysicsWorld::setNumThreads(int threads) {
    if (threads < 1) return;
    threads_ = threads;
    // Chrono takes (solver, collision, FEA). FEA はケーブル（Core）が使う。
    sys_->SetNumThreads(threads, threads, threads);
    LOGI("physics", "threads: %d", threads);
}

void PhysicsWorld::setSolverIterations(int iterations) {
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

void PhysicsWorld::step(double dt) {
    // ソフトボディのばね: 積分の前に粒子の速度へ織り込む（接触ソルバは
    // この速度を見て解く）。
    for (auto& soft : softBodies_) solveSoftSprings(soft, dt);
    // Multicore 用の手計算のモータ・ばね（Core では空）。
    applyManualJoints(dt);
    // ボディ別の重力オフ: Chrono 9 には per-body の口が無い（SetNoGravity は
    // 消えた）ので、重力ぶんを毎ステップ打ち消す（両バックエンド共通）。
    {
        const ChVector3d g = sys_->GetGravitationalAcceleration();
        if (g.Length2() > 0.0) {
            for (std::size_t i = 0; i < bodies_.size(); ++i) {
                if (options_[i].gravity || !active_[i]) continue;
                auto& b = bodies_[i];
                if (b->IsFixed() || softOf_[i] != kNoSoft) continue;
                if (b->IsSleeping()) continue;
                setLinVel(b.get(), getLinVel(b.get(), 0) - g * dt, 0);
            }
        }
    }

    sys_->DoStepDynamics(dt);

    // 診断: 拘束が解かれているかを最初の数ステップと 1 秒後に 1 回ずつ出す
    // （「全部が地面を抜けて落ちる」の切り分け用: 接触とリンクの数、
    // ソルバの設定）。ステップ数は removeAllJoints で戻る = シミュレート
    // 開始のたびに出る。
    ++diagSteps_;
    if (diagSteps_ == 2 || diagSteps_ == 120) {
        std::size_t activeBodies = 0;
        for (std::size_t i = 0; i < bodies_.size(); ++i) {
            if (active_[i] && !bodies_[i]->IsFixed()) ++activeBodies;
        }
        LOGI("physics",
             "diag step %d: backend=%s dt=%.4f bodies=%zu (dynamic %zu) links=%d "
             "contacts=%d joints=%zu gravity=(%.2f %.2f %.2f)",
             diagSteps_, backendName(), dt, bodies_.size(), activeBodies,
             linkCountOf(sys_.get(), 0),
             contactCountOf(sys_->GetContactContainer().get(), 0), joints_.size(),
             sys_->GetGravitationalAcceleration().x(),
             sys_->GetGravitationalAcceleration().y(),
             sys_->GetGravitationalAcceleration().z());
        if (auto iterative = std::dynamic_pointer_cast<ChIterativeSolverVI>(sys_->GetSolver())) {
            LOGI("physics", "diag: iterative solver, max iterations %d",
                 iterative->GetMaxIterations());
        } else if (sys_->GetSolver()) {
            LOGI("physics", "diag: non-iterative solver (type %d)", int(sys_->GetSolver()->GetType()));
        } else {
            LOGW("physics", "diag: the system has NO solver");
        }
    }

    // 診断: NaN の検出。1 本の拘束が NaN を出すと反復ソルバ全体が NaN になり、
    // 拘束の付いた物が全部消える（= 落ちたように見える）。最初の 2 秒だけ
    // 見て、出たらどのジョイントの反力が NaN かを 1 回だけ出す。
    if (diagSteps_ <= 120 && !diagNanReported_) {
        std::size_t bad = 0;
        for (std::size_t i = 0; i < bodies_.size(); ++i) {
            if (!active_[i]) continue;
            const ChVector3d p = bodies_[i]->GetPos();
            const ChVector3d v = getLinVel(bodies_[i].get(), 0);
            if (!std::isfinite(p.x() + p.y() + p.z() + v.x() + v.y() + v.z())) ++bad;
        }
        if (bad > 0) {
            diagNanReported_ = true;
            LOGE("physics", "diag step %d: %zu bodies have NaN position/velocity - "
                            "the constraint solve is poisoned", diagSteps_, bad);
            for (std::size_t j = 0; j < joints_.size(); ++j) {
                double f = 0.0, t = 0.0;
                const bool ok = jointReaction(j, f, t);
                LOGE("physics", "  joint #%zu type %d: reaction %s (force %.3g torque %.3g)%s",
                     j, int(joints_[j].type),
                     ok ? (std::isfinite(f + t) ? "finite" : "NaN") : "n/a", f, t,
                     joints_[j].manual.kind != JointRec::Manual::Kind::None ? " [manual]" : "");
            }
        }
    }

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

namespace {
#ifdef WIZ_HAVE_COLLISION_SHAPES
// ChBody::AddCollisionShape(shape, frame) は Chrono 9 の口。無い版では
// false を返して呼び出し側が警告する（スリープ / ジョイントと同じ SFINAE）。
template <class B>
auto addShapeImpl(B& body, std::shared_ptr<chrono::ChCollisionShape> shape,
                  const chrono::ChFrame<>& frame, int)
    -> decltype(body.AddCollisionShape(shape, frame), bool()) {
    body.AddCollisionShape(shape, frame);
    return true;
}
template <class B>
bool addShapeImpl(B&, std::shared_ptr<chrono::ChCollisionShape>, const chrono::ChFrame<>&,
                  long) {
    return false;
}
#endif
}  // namespace

void PhysicsWorld::attachExtraShapes(ChBody& body, const std::vector<ExtraShape>& extras,
                                     const std::shared_ptr<ChContactMaterial>& mat) {
    if (extras.empty()) return;
    static bool warned = false;
    static bool warnedCylinder = false;
    bool ok = true;
#ifdef WIZ_HAVE_COLLISION_SHAPES
    for (const ExtraShape& s : extras) {
        std::shared_ptr<ChCollisionShape> shape;
        ChQuaternion<> rot = s.rot;
        if (s.cylinder) {
            // Chrono の円柱は形状座標系の Z 軸が軸。部品の円柱は X 軸なので
            // 「Z を X へ」の回転を部品の回転に重ねる。
#ifdef WIZ_HAVE_CYLINDER_SHAPE
            shape = chrono_types::make_shared<ChCollisionShapeCylinder>(
                mat, s.size.y() * 0.5, s.size.x());
            rot = s.rot * quatFromZAxis(ChVector3d(1, 0, 0));
#else
            if (!warnedCylinder) {
                warnedCylinder = true;
                LOGW("physics", "this Chrono build has no cylinder collision shape - "
                                "cylinder parts with collide=\"true\" are visual only");
            }
            continue;
#endif
        } else if (s.sphere) {
            shape = chrono_types::make_shared<ChCollisionShapeSphere>(mat, s.size.x() * 0.5);
        } else {
            shape = chrono_types::make_shared<ChCollisionShapeBox>(mat, s.size.x(),
                                                                   s.size.y(), s.size.z());
        }
        ok = addShapeImpl(body, shape, ChFrame<>(s.pos, rot), 0) && ok;
    }
#else
    ok = false;
#endif
    if (!ok && !warned) {
        warned = true;
        LOGW("physics", "this Chrono build has no compound collision shapes - "
                        "prefab parts with collide=\"true\" are visual only");
    }
}

std::size_t PhysicsWorld::addSphere(double radius, double density,
                                    const ChVector3d& pos,
                                    const ChQuaternion<>& rot, bool fixed,
                                    const std::vector<ExtraShape>& extras,
                                    const BodyOptions& options) {
    const auto mat = materialFor(options);
    auto b = chrono_types::make_shared<ChBodyEasySphere>(
        radius, density, /*visualize*/ true, /*collide*/ true, mat);
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    b->EnableCollision(true);
    allowSleeping(b.get(), sleepingEnabled_, 0);
    if (sleepingEnabled_) {
        b->SetSleepTime(sleepSeconds_);
        setSleepLimits(b.get(), sleepMinLinVel_, sleepMinAngVel_, 0);
    }
    prepareBody(*b, options);
    sys_->AddBody(b);
    finishBody(b, mat, options);
    return bodies_.size() - 1;
}

void PhysicsWorld::setDamping(double linearPerSecond, double angularPerSecond) {
    linearDamping_ = linearPerSecond;
    angularDamping_ = angularPerSecond;
}

void PhysicsWorld::setSurfaceMaterial(float friction, float restitution) {
    friction_ = friction;
    restitution_ = restitution;
    applySurface(*mat_, BodyOptions{});
    // 専用材質のボディも「シーン任せ」の欄はこの値に追従する。
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        if (mats_[i]) applySurface(*mats_[i], options_[i]);
    }
}

void PhysicsWorld::setRollingFriction(float rolling, float spinning) {
    // NSC materials expose these directly; they are ignored by solvers that do
    // not model rolling resistance, which is harmless. SMC の材質には無い
    // （applySurface が種類を見て入れる）。
    rolling_ = rolling;
    spinning_ = spinning;
    applySurface(*mat_, BodyOptions{});
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        if (mats_[i]) applySurface(*mats_[i], options_[i]);
    }
}

std::size_t PhysicsWorld::addBox(double sx, double sy, double sz, double density,
                                 const ChVector3d& pos,
                                 const ChQuaternion<>& rot, bool fixed,
                                 const std::vector<ExtraShape>& extras,
                                 const BodyOptions& options) {
    const auto mat = materialFor(options);
    auto b = chrono_types::make_shared<ChBodyEasyBox>(
        sx, sy, sz, density, /*visualize*/ true, /*collide*/ true, mat);
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    b->EnableCollision(true);
    allowSleeping(b.get(), sleepingEnabled_, 0);
    if (sleepingEnabled_) {
        b->SetSleepTime(sleepSeconds_);
        setSleepLimits(b.get(), sleepMinLinVel_, sleepMinAngVel_, 0);
    }
    prepareBody(*b, options);
    sys_->AddBody(b);
    finishBody(b, mat, options);
    return bodies_.size() - 1;
}

std::size_t PhysicsWorld::addTriangleMesh(const std::vector<ChVector3d>& vertices,
                                          const std::vector<std::array<int, 3>>& triangles,
                                          double mass, const ChVector3d& pos,
                                          const ChQuaternion<>& rot, bool fixed,
                                          const std::vector<ExtraShape>& extras,
                                          const BodyOptions& options) {
#if defined(WIZ_HAVE_COLLISION_SHAPES) && defined(WIZ_HAVE_TRIMESH_SHAPE)
    if (vertices.empty() || triangles.empty()) return kInvalidId;
    auto mesh = chrono_types::make_shared<ChTriangleMeshConnected>();
    mesh->GetCoordsVertices() = vertices;
    auto& tris = mesh->GetIndicesVertexes();
    tris.reserve(triangles.size());
    double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
    for (const auto& v : vertices) {
        const double c[3] = {v.x(), v.y(), v.z()};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], c[a]);
            hi[a] = std::max(hi[a], c[a]);
        }
    }
    for (const auto& t : triangles) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || std::size_t(t[0]) >= vertices.size() ||
            std::size_t(t[1]) >= vertices.size() || std::size_t(t[2]) >= vertices.size()) {
            continue;
        }
        tris.push_back(ChVector3i(t[0], t[1], t[2]));
    }
    if (tris.empty()) return kInvalidId;

    const auto mat = materialFor(options);
    auto b = chrono_types::make_shared<ChBody>();
    const double m = std::max(mass, 1e-3);
    b->SetMass(m);
    // 慣性は外接箱の均質な箱として（凹メッシュの体積は当てにならない）。
    double L[3];
    for (int a = 0; a < 3; ++a) L[a] = std::max(hi[a] - lo[a], 0.01);
    b->SetInertiaXX(ChVector3d(m / 12.0 * (L[1] * L[1] + L[2] * L[2]),
                               m / 12.0 * (L[0] * L[0] + L[2] * L[2]),
                               m / 12.0 * (L[0] * L[0] + L[1] * L[1])));
    // 三角メッシュ形状: is_static = 固定なら true（Bullet が静的メッシュとして
    // 最適化する）、is_convex = false（凹形状のため）。sphere_swept は 0。
    auto shape = chrono_types::make_shared<ChCollisionShapeTriangleMesh>(
        mat, mesh, fixed, /*is_convex*/ false, /*sphere_swept*/ 0.0);
    if (!addShapeImpl(*b, shape, ChFrame<>(), 0)) return kInvalidId;
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    b->EnableCollision(true);
    allowSleeping(b.get(), sleepingEnabled_, 0);
    if (sleepingEnabled_) {
        b->SetSleepTime(sleepSeconds_);
        setSleepLimits(b.get(), sleepMinLinVel_, sleepMinAngVel_, 0);
    }
    prepareBody(*b, options);
    sys_->AddBody(b);
    finishBody(b, mat, options);
    if (!fixed) {
        LOGW("physics", "triangle-mesh body is not fixed - concave mesh vs mesh "
                        "collision is slow (GImpact); prefer fixed=\"true\"");
    }
    return bodies_.size() - 1;
#else
    (void)vertices; (void)triangles; (void)mass; (void)pos; (void)rot; (void)fixed;
    (void)extras; (void)options;
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOGW("physics", "this Chrono build has no triangle-mesh collision shape - "
                        "collision=\"trimesh\" falls back to the convex hull");
    }
    return kInvalidId;
#endif
}

std::size_t PhysicsWorld::addFrame(double mass, const ChVector3d& pos,
                                   const ChQuaternion<>& rot, bool fixed,
                                   const std::vector<ExtraShape>& extras,
                                   const BodyOptions& options) {
    const auto mat = materialFor(options);
    auto b = chrono_types::make_shared<ChBody>();
    const double m = std::max(mass, 1e-3);
    b->SetMass(m);
    // 慣性: 追加形状の外接箱（回転は無視、ローカルの軸に沿った箱）を
    // 均質な箱として。無ければ 0.5 m の箱（フレームだけなら動かないので
    // 値はほぼ効かない）。
    double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
    for (const ExtraShape& e : extras) {
        const double hx = e.sphere ? e.size.x() * 0.5 : e.size.x() * 0.5;
        const double hy = e.sphere ? e.size.x() * 0.5 : e.size.y() * 0.5;
        const double hz = e.sphere ? e.size.x() * 0.5 : e.size.z() * 0.5;
        const double c[3] = {e.pos.x(), e.pos.y(), e.pos.z()};
        const double h[3] = {hx, hy, hz};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], c[a] - h[a]);
            hi[a] = std::max(hi[a], c[a] + h[a]);
        }
    }
    double L[3] = {0.5, 0.5, 0.5};
    if (!extras.empty()) {
        for (int a = 0; a < 3; ++a) L[a] = std::max(hi[a] - lo[a], 0.01);
    }
    b->SetInertiaXX(ChVector3d(m / 12.0 * (L[1] * L[1] + L[2] * L[2]),
                               m / 12.0 * (L[0] * L[0] + L[2] * L[2]),
                               m / 12.0 * (L[0] * L[0] + L[1] * L[1])));
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    if (!extras.empty()) b->EnableCollision(true);
    allowSleeping(b.get(), sleepingEnabled_, 0);
    if (sleepingEnabled_) {
        b->SetSleepTime(sleepSeconds_);
        setSleepLimits(b.get(), sleepMinLinVel_, sleepMinAngVel_, 0);
    }
    prepareBody(*b, options);
    sys_->AddBody(b);
    finishBody(b, mat, options);
    return bodies_.size() - 1;
}

std::size_t PhysicsWorld::addConvexHull(
    const std::vector<ChVector3d>& points, double mass,
    const ChVector3d& pos, const ChQuaternion<>& rot,
    const std::vector<ExtraShape>& extras, ChVector3d* hullCenter,
    const BodyOptions& options) {
    const auto mat = materialFor(options);
    // 凸包の体積重心を先に求める。ChBodyEasyConvexHull は内部で同じ計算を
    // して頂点をここへ寄せる（ボディの原点 = 重心）ので、呼び出し側が
    // 見た目を同じだけずらせるように返す。Chrono と同じ道具（Bullet の
    // 凸包 + 三角メッシュの質量特性）で出すから、寄せ量と一致する。
    ChVector3d center(0.0, 0.0, 0.0);
    if (points.size() >= 4) {
        chrono::ChTriangleMeshConnected hull;
        chrono::bt_utils::ChConvexHullLibraryWrapper wrapper;
        wrapper.ComputeHull(points, hull);
        double volume = 0.0;
        chrono::ChMatrix33<> inertia;
        hull.ComputeMassProperties(true, volume, center, inertia);
    }
    if (hullCenter) *hullCenter = center;

    // visualize=false: our renderer draws the glTF model itself; Chrono's
    // visual assets are unused here.
    //
    // Chrono's constructor takes the points by NON-const reference and
    // translates them in place (it moves the barycentre onto the centre of
    // mass) - hence the local copy: the caller's point cloud is shared by
    // every body and must not be mutated.
    //
    // 密度 1 で作って、あとから文書の質量へ合わせる。凸包の体積はモデルの
    // 大きさ次第（文書の size とは無関係）なので、密度から出すと質量が
    // 桁違いになる - 以前は 1 m のモデルに size 0.05 を書くと 100 kg 超の
    // 「りんご」ができて、触れた物を弾き飛ばしていた。
    std::vector<ChVector3d> local = points;
    auto b = chrono_types::make_shared<ChBodyEasyConvexHull>(
        local, /*density*/ 1.0, /*visualize*/ false, /*collide*/ true, mat);
    const double unitMass = b->GetMass();  // 密度 1 なので = 凸包の体積
    if (unitMass > 1e-12 && std::isfinite(unitMass)) {
        // 慣性は Chrono が凸包から出した形（密度 1）のまま、質量の比で伸縮。
        b->SetInertia(b->GetInertia() * (mass / unitMass));
    } else {
        // 退化した凸包（平ら・点が 4 個未満）: 外接半径の球として扱う。
        double r2 = 1e-6;
        for (const auto& p : points) {
            const ChVector3d d = p - center;
            r2 = std::max(r2, d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
        }
        const double i = 0.4 * mass * r2;
        b->SetInertiaXX(ChVector3d(i, i, i));
        LOGW("physics",
             "convex hull is degenerate (%zu points) - using a sphere inertia",
             points.size());
    }
    b->SetMass(mass);
    // 部品の位置は「寄せる前の原点」基準のまま = 原点が重心から離れた
    // モデルではそのぶんずれる（見た目は hullCenter で合わせるが、複合形状
    // の部品までは動かさない）。
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(false);
    b->EnableCollision(true);
    allowSleeping(b.get(), sleepingEnabled_, 0);
    if (sleepingEnabled_) {
        b->SetSleepTime(sleepSeconds_);
        setSleepLimits(b.get(), sleepMinLinVel_, sleepMinAngVel_, 0);
    }
    prepareBody(*b, options);
    sys_->AddBody(b);
    finishBody(b, mat, options);
    return bodies_.size() - 1;
}

std::size_t PhysicsWorld::bodyCount() const {
    return bodies_.size();
}

BodyTransform PhysicsWorld::bodyTransform(std::size_t id) const {
    if (const SoftBody* soft = softOfRoot(id)) return softTransform(*soft);
    const auto& b = bodies_[id];
    const ChVector3d p = b->GetPos();
    const ChQuaternion<> q = b->GetRot();
    return {p.x(), p.y(), p.z(), q.e0(), q.e1(), q.e2(), q.e3()};
}

void PhysicsWorld::setBodyPose(std::size_t id, const ChVector3d& pos,
                               const ChQuaternion<>& rot) {
    if (SoftBody* soft = softOfRoot(id)) {
        // 静止形状のまま置き直す。粒子は眠らないので落下速度は要らない。
        placeSoftBody(*soft, pos, rot);
        return;
    }
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
    if (SoftBody* soft = softOfRoot(id)) {
        placeSoftBody(*soft, pos, rot);
        return;
    }
    auto& b = bodies_[id];
    b->SetPos(pos);
    b->SetRot(rot);
    b->ForceToRest();
    wakeUp(b.get(), 0);
    resetSleepTimer(*b);  // 静止させた直後に眠らせない
    // setBodyPose と違って落下速度は与えない。エディタで置いた物は、
    // シミュレートを始めるまでその場に止まっていてほしい。
}

void PhysicsWorld::setBodyFixed(std::size_t id, bool fixed) {
    if (id >= bodies_.size()) return;
    // 固定するときは速度も捨てる（ForceToRest）。走行中に固定された物が
    // 速度を持ったままだと、Multicore の接触拘束がそれを「動く床」として
    // 上の物へ伝え続ける（applyForce の注記と同じ理屈）。
    if (SoftBody* soft = softOfRoot(id)) {
        // 全粒子を固定 = 形を保ったまま動かない土台になる。
        for (const std::size_t p : soft->particles) {
            bodies_[p]->SetFixed(fixed);
            if (fixed) bodies_[p]->ForceToRest();
            else wakeUp(bodies_[p].get(), 0);
        }
        return;
    }
    bodies_[id]->SetFixed(fixed);
    if (fixed) {
        bodies_[id]->ForceToRest();
    } else {
        wakeUp(bodies_[id].get(), 0);
        resetSleepTimer(*bodies_[id]);  // 固定を外した直後に眠らせない
    }
}

bool PhysicsWorld::bodyFixed(std::size_t id) const {
    if (id >= bodies_.size()) return false;
    // ソフトボディは代表粒子で代表させる（setBodyFixed は全粒子を揃えて
    // 固定するので、代表を見れば足りる）。
    return bodies_[id]->IsFixed();
}

void PhysicsWorld::disableBody(std::size_t id) {
    if (id >= bodies_.size() || !active_[id]) return;
    if (SoftBody* soft = softOfRoot(id)) {
        // 粒子を全部退場させ、ばねも解かない。
        soft->active = false;
        for (const std::size_t p : soft->particles) {
            if (p == id) continue;  // 代表は下の通常経路で
            auto& pb = bodies_[p];
            pb->SetFixed(true);
            if (backend_ != PhysicsBackend::Multicore) pb->EnableCollision(false);
            pb->ForceToRest();
            pb->SetPos(ChVector3d(0, -1000.0, 0));
            active_[p] = false;
        }
    }
    auto& b = bodies_[id];
    b->SetFixed(true);
    // Multicore の衝突系は Remove() が未実装で、Chrono 9 は
    // "ChCollisionSystemMulticore::Remove() not yet implemented." を出して
    // 例外を投げる（物理スレッドで捕まらず、アプリごと落ちる）。そちらでは
    // 当たり判定のフラグを触らず、固定 + 退避だけで無効化する。退場した
    // ボディは全部 fixed なので、同じ場所に重なっても互いに接触しない。
    if (backend_ != PhysicsBackend::Multicore) {
        b->EnableCollision(false);
    }
    b->ForceToRest();
    // 地面のはるか下へ。当たり判定を切っても衝突系がまだ形状を持っている
    // 版があるので、位置でも確実に無関係にしておく。
    b->SetPos(ChVector3d(0, -1000.0, 0));
    active_[id] = false;
    // Core では本当に系から外す（B の 18）。粒子も同じ。
    if (SoftBody* soft = softOfRoot(id)) {
        for (const std::size_t p : soft->particles) detachBody(p);
    } else {
        detachBody(id);
    }
}

// Core（Bullet）は RemoveBody を実装しているので、退場したボディは系から
// 本当に外す: ソルバと衝突系がその変数・形状を見なくなり、エディタで
// 大きさを何度も変えても空のボディが溜まらない。ChBody のオブジェクト自体は
// bodies_ に残す（番号を詰めない・接触コールバックの逆引きが壊れない）。
// Multicore は衝突系の Remove が未実装なので触らない（退避のみ）。
void PhysicsWorld::detachBody(std::size_t id) {
    if (backend_ == PhysicsBackend::Multicore || id >= bodies_.size()) return;
    auto& b = bodies_[id];
    if (!b->GetSystem()) return;  // 既に外れている
    // 定常荷重を先に外す（ボディの変数が系から消えた後に荷重が残ると、
    // 古いオフセットへ書き込む）。
#ifdef WIZ_HAVE_LOADS
    if (bodyLoads_ && !loadsOf_[id].empty()) {
        bool removed = true;
        for (auto& l : loadsOf_[id]) removed = removeLoadFrom(bodyLoads_.get(), l, 0) && removed;
        if (!removed) {
            // Remove の無い版: 全部捨てて、残っている物のぶんを入れ直す。
            clearLoads(bodyLoads_.get(), 0);
            for (std::size_t i = 0; i < loadsOf_.size(); ++i) {
                if (i == id || !active_[i]) continue;
                for (auto& l : loadsOf_[i]) bodyLoads_->Add(l);
            }
        }
        loadsOf_[id].clear();
    }
#endif
    sys_->RemoveBody(b);
}

bool PhysicsWorld::bodyActive(std::size_t id) const {
    return id < active_.size() && active_[id];
}

// ---- ジョイント -----------------------------------------------------------

std::size_t PhysicsWorld::addJoint(JointType type, std::size_t bodyA,
                                   std::size_t bodyB, const ChVector3d& anchor,
                                   const ChVector3d& axis, double distance) {
    JointSpec spec;
    spec.type = type;
    spec.bodyA = bodyA;
    spec.bodyB = bodyB;
    spec.anchor = anchor;
    spec.axis = axis;
    spec.distance = distance;
    return addJoint(spec);
}

std::size_t PhysicsWorld::addJoint(const JointSpec& spec) {
    const std::size_t bodyA = spec.bodyA;
    const std::size_t bodyB = spec.bodyB;
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

    const ChVector3d anchor = spec.anchor;
    const ChVector3d axis = axisUsable(spec.axis) ? spec.axis : ChVector3d(0, 1, 0);
    // リンク座標系。Revolute は Z 軸まわりに回り、Prismatic は Z 軸方向へ
    // スライドするので、指定されたワールド軸を Z に合わせる。
    const ChQuaternion<> frameRot = quatFromZAxis(axis);

    // 2 点間のばね（Spring 種類、または Prismatic に添えるばね）。地面側は
    // アンカー、それ以外はボディの中心を取り付け点にする。
    auto makeSpring = [&](double restLength) -> std::shared_ptr<chrono::ChLinkBase> {
        const ChVector3d p1 = a->IsFixed() ? anchor : a->GetPos();
        const ChVector3d p2 = b->IsFixed() ? anchor : b->GetPos();
        if (a->IsFixed() && b->IsFixed()) return nullptr;
        auto s = chrono_types::make_shared<ChLinkTSDA>();
        s->Initialize(a, b, /*local*/ false, p1, p2);
        double len = restLength;
        if (len <= 0.0) len = (p2 - p1).Length();
        s->SetRestLength(len);
        s->SetSpringCoefficient(spec.stiffness);
        s->SetDampingCoefficient(spec.damping);
        return s;
    };

    JointRec rec;
    rec.type = spec.type;
    rec.breakForce = spec.breakForce;
    std::shared_ptr<chrono::ChLinkBase> link;
    bool wantLimit = spec.limited;
    bool limitDone = false;
    // Multicore は ChLinkTSDA / ChLinkRSDA / トルク（力）モータの力を積分に
    // 取り込まず、速度モータは専用の一覧（RemoveLink で外れない）を持つ。
    // そちらでは手計算の力積（applyManualJoints）と「角度モータ + ランプ
    // 関数」で同じ意味を作る。Core は Chrono のリンクをそのまま使う。
    const bool manualMode = backend_ == PhysicsBackend::Multicore;
    // Multicore は片側拘束（ChLinkLimit）を解けない - 範囲に当たった瞬間に
    // 系全体が NaN になった（実機で確認）。そちらでは可動範囲も手計算の
    // 力積にする（applyManualJoints）。
    auto manualLimit = [&](bool linear) {
        rec.manual.limited = true;
        rec.manual.limitLinear = linear;
        rec.manual.limitLo = spec.limitLo;
        rec.manual.limitHi = spec.limitHi;
        rec.manual.a = bodyA;
        rec.manual.b = bodyB;
        rec.manual.axisLocalA = a->GetRot().RotateBack(axis);
        rec.manual.relRot0 = a->GetRot().GetConjugate() * b->GetRot();
        const ChVector3d axisN = axis * (1.0 / std::max(axis.Length(), 1e-9));
        rec.manual.limitRef = (b->GetPos() - a->GetPos()).Dot(axisN);
    };
    // ローカル座標への変換（手計算のモータ・ばね用）。
    auto localAxis = [&](const std::shared_ptr<ChBody>& body, const ChVector3d& w) {
        return body->GetRot().RotateBack(w);
    };
    auto localPoint = [&](const std::shared_ptr<ChBody>& body, const ChVector3d& p) {
        return body->GetRot().RotateBack(p - body->GetPos());
    };
    // Multicore の速度モータ: 角度 / 位置モータに「今の値 + 速度 × 経過時間」の
    // ランプ関数を渡す。
    auto rampFor = [&](double now, double value0, double speed) {
        return chrono_types::make_shared<WizRampFunction>(value0 - speed * now, speed);
    };

    switch (spec.type) {
        case JointType::Fixed: {
            auto l = chrono_types::make_shared<ChLinkLockLock>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Revolute: {
            const bool manualTorque = manualMode && spec.motor == MotorType::Force;
            if (spec.motor != MotorType::None && !manualTorque) {
                // モータは拘束ごと置き換わる（Z 軸まわり = Revolute と同じ）。
                std::shared_ptr<chrono::ChLinkMotorRotation> m;
                std::shared_ptr<chrono::ChFunction> fn;
                if (spec.motor == MotorType::Speed && manualMode) {
                    // Multicore: 角度モータ + ランプ関数（角速度 = 傾き）。
                    auto am = chrono_types::make_shared<ChLinkMotorRotationAngle>();
                    fn = rampFor(sys_->GetChTime(), 0.0, spec.motorTarget);
                    rec.rampMotor = am;
                    m = am;
                } else {
                    fn = chrono_types::make_shared<WizConstFunction>(spec.motorTarget);
                    rec.motorFn = fn;
                    switch (spec.motor) {
                        case MotorType::Speed:
                            m = chrono_types::make_shared<ChLinkMotorRotationSpeed>();
                            break;
                        case MotorType::Position:
                            m = chrono_types::make_shared<ChLinkMotorRotationAngle>();
                            break;
                        default:
                            m = chrono_types::make_shared<ChLinkMotorRotationTorque>();
                            break;
                    }
                }
                m->Initialize(a, b, ChFrame<>(anchor, frameRot));
                m->SetMotorFunction(fn);
                link = m;
                wantLimit = false;  // モータに可動範囲は付かない
            } else {
                auto l = chrono_types::make_shared<ChLinkLockRevolute>();
                initLink(l.get(), a, b, anchor, frameRot, 0);
                if (spec.limited) {
                    if (manualMode) { manualLimit(false); limitDone = true; }
                    else limitDone = setLimitRz(l.get(), spec.limitLo, spec.limitHi, 0);
                }
                link = l;
                if (manualTorque) {
                    // Multicore のトルクモータ: 普通のちょうつがい + 毎ステップの角力積。
                    rec.manual.kind = JointRec::Manual::Kind::Torque;
                    rec.manual.a = bodyA;
                    rec.manual.b = bodyB;
                    rec.manual.axisLocalA = localAxis(a, axis);
                    rec.manual.target = spec.motorTarget;
                    wantLimit = false;
                }
            }
            // 回転ばね（自然角 = 開始姿勢）。
            if (spec.stiffness > 0.0 || spec.damping > 0.0) {
                if (manualMode) {
                    // トルクモータと同居できないので、その場合はばねを優先しない。
                    if (rec.manual.kind == JointRec::Manual::Kind::None) {
                        rec.manual.kind = JointRec::Manual::Kind::Rsda;
                        rec.manual.a = bodyA;
                        rec.manual.b = bodyB;
                        rec.manual.axisLocalA = localAxis(a, axis);
                        rec.manual.relRot0 = a->GetRot().GetConjugate() * b->GetRot();
                        rec.manual.k = spec.stiffness;
                        rec.manual.c = spec.damping;
                    } else {
                        LOGW("physics", "hinge: a torque motor and a spring on the same "
                                        "joint are not supported on the multicore backend "
                                        "- spring ignored");
                    }
                } else {
                    auto s = chrono_types::make_shared<ChLinkRSDA>();
                    s->Initialize(a, b, ChFrame<>(anchor, frameRot));
                    s->SetRestAngle(0.0);
                    s->SetSpringCoefficient(spec.stiffness);
                    s->SetDampingCoefficient(spec.damping);
                    rec.extra = s;
                }
            }
            break;
        }
        case JointType::Spherical: {
            auto l = chrono_types::make_shared<ChLinkLockSpherical>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Prismatic: {
            const bool manualForce = manualMode && spec.motor == MotorType::Force;
            if (spec.motor != MotorType::None && !manualForce) {
                // 直動モータは X 軸方向に動く（ChLinkLockPrismatic は Z）。
                const ChQuaternion<> xRot = quatFromXAxis(axis);
                std::shared_ptr<chrono::ChLinkMotorLinear> m;
                std::shared_ptr<chrono::ChFunction> fn;
                if (spec.motor == MotorType::Speed && manualMode) {
                    auto pm = chrono_types::make_shared<ChLinkMotorLinearPosition>();
                    fn = rampFor(sys_->GetChTime(), 0.0, spec.motorTarget);
                    rec.rampMotor = pm;
                    rec.rampLinear = true;
                    m = pm;
                } else {
                    fn = chrono_types::make_shared<WizConstFunction>(spec.motorTarget);
                    rec.motorFn = fn;
                    switch (spec.motor) {
                        case MotorType::Speed:
                            m = chrono_types::make_shared<ChLinkMotorLinearSpeed>();
                            break;
                        case MotorType::Position:
                            m = chrono_types::make_shared<ChLinkMotorLinearPosition>();
                            break;
                        default:
                            m = chrono_types::make_shared<ChLinkMotorLinearForce>();
                            break;
                    }
                }
                m->Initialize(a, b, ChFrame<>(anchor, xRot));
                m->SetMotorFunction(fn);
                link = m;
                wantLimit = false;
            } else {
                auto l = chrono_types::make_shared<ChLinkLockPrismatic>();
                initLink(l.get(), a, b, anchor, frameRot, 0);
                if (spec.limited) {
                    if (manualMode) { manualLimit(true); limitDone = true; }
                    else limitDone = setLimitZ(l.get(), spec.limitLo, spec.limitHi, 0);
                }
                link = l;
                if (manualForce) {
                    rec.manual.kind = JointRec::Manual::Kind::Force;
                    rec.manual.a = bodyA;
                    rec.manual.b = bodyB;
                    rec.manual.axisLocalA = localAxis(a, axis);
                    rec.manual.target = spec.motorTarget;
                    wantLimit = false;
                }
            }
            if (spec.stiffness > 0.0 || spec.damping > 0.0) {
                if (manualMode) {
                    if (rec.manual.kind == JointRec::Manual::Kind::None) {
                        rec.manual.kind = JointRec::Manual::Kind::Spring;
                        rec.manual.a = bodyA;
                        rec.manual.b = bodyB;
                        rec.manual.p1Local = localPoint(a, a->IsFixed() ? anchor : a->GetPos());
                        rec.manual.p2Local = localPoint(b, b->IsFixed() ? anchor : b->GetPos());
                        rec.manual.k = spec.stiffness;
                        rec.manual.c = spec.damping;
                        rec.manual.rest = ((b->IsFixed() ? anchor : b->GetPos()) -
                                           (a->IsFixed() ? anchor : a->GetPos())).Length();
                    } else {
                        LOGW("physics", "slide: a force motor and a spring on the same joint "
                                        "are not supported on the multicore backend - "
                                        "spring ignored");
                    }
                } else {
                    rec.extra = makeSpring(0.0);
                }
            }
            break;
        }
        case JointType::Distance: {
            // 2 点は各ボディの現在位置。distance が 0 なら今の間隔を保つ。
            const ChVector3d p1 = a->GetPos();
            const ChVector3d p2 = b->GetPos();
            double len = spec.distance;
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
        case JointType::Universal: {
            // 十字軸はリンク座標系の X と Y = 指定した軸（Z）に直交する 2 軸。
            auto l = chrono_types::make_shared<ChLinkUniversal>();
            l->Initialize(a, b, ChFrame<>(anchor, frameRot));
            link = l;
            if (spec.limited) {
                LOGW("physics", "universal joint: limits are not supported - ignored");
                wantLimit = false;
            }
            break;
        }
        case JointType::Cylindrical: {
            auto l = chrono_types::make_shared<ChLinkLockCylindrical>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            if (spec.limited) {
                if (manualMode) { manualLimit(false); limitDone = true; }
                else limitDone = setLimitRz(l.get(), spec.limitLo, spec.limitHi, 0);
            }
            link = l;
            break;
        }
        case JointType::Planar: {
            // 面 = リンク座標系の XY、法線 = Z = 指定した軸。
            auto l = chrono_types::make_shared<ChLinkLockPlanar>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::PointLine: {
            // 線はリンク座標系の X 軸なので、指定した軸を X に合わせる。
            auto l = chrono_types::make_shared<ChLinkLockPointLine>();
            initLink(l.get(), a, b, anchor, quatFromXAxis(axis), 0);
            link = l;
            break;
        }
        case JointType::PointPlane: {
            // 面の法線 = Z = 指定した軸。
            auto l = chrono_types::make_shared<ChLinkLockPointPlane>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Gear: {
#ifdef WIZ_HAVE_GEAR
            // シャフト 1 = A 上の (anchor, axis)、シャフト 2 = B 上の
            // (anchor2, axis2)。どちらもボディのローカル座標系で渡す。
            const ChVector3d anchor2 = spec.hasAnchor2 ? spec.anchor2 : b->GetPos();
            const ChVector3d axis2 =
                (spec.hasAxis2 && axisUsable(spec.axis2)) ? spec.axis2 : axis;
            auto l = chrono_types::make_shared<ChLinkLockGear>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            const ChFrame<> s1 = worldToBodyLocal(*a, anchor, frameRot);
            const ChFrame<> s2 = worldToBodyLocal(*b, anchor2, quatFromZAxis(axis2));
            if (!setGearParams(l.get(), spec.ratio, s1, s2, 0)) {
                LOGW("physics", "gear joint: this Chrono has a different ChLinkLockGear "
                                "API - skipped");
                return kInvalidJoint;
            }
            link = l;
#else
            LOGW("physics", "gear joint: this Chrono build has no ChLinkLockGear - skipped");
            return kInvalidJoint;
#endif
            break;
        }
        case JointType::Screw: {
#ifdef WIZ_HAVE_SCREW
            auto l = chrono_types::make_shared<ChLinkLockScrew>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            if (!setScrewThread(l.get(), spec.pitch, 0)) {
                LOGW("physics", "screw joint: this Chrono has a different ChLinkLockScrew "
                                "API - skipped");
                return kInvalidJoint;
            }
            link = l;
#else
            LOGW("physics", "screw joint: this Chrono build has no ChLinkLockScrew - skipped");
            return kInvalidJoint;
#endif
            break;
        }
        case JointType::Spring: {
            if (a->IsFixed() && b->IsFixed()) {
                LOGW("physics", "spring: both ends are fixed - skipped");
                return kInvalidJoint;
            }
            if (manualMode) {
                // リンク無し。毎ステップの力積だけ（applyManualJoints）。
                const ChVector3d p1 = a->IsFixed() ? anchor : a->GetPos();
                const ChVector3d p2 = b->IsFixed() ? anchor : b->GetPos();
                rec.manual.kind = JointRec::Manual::Kind::Spring;
                rec.manual.a = bodyA;
                rec.manual.b = bodyB;
                rec.manual.p1Local = localPoint(a, p1);
                rec.manual.p2Local = localPoint(b, p2);
                rec.manual.k = spec.stiffness;
                rec.manual.c = spec.damping;
                rec.manual.rest = spec.distance > 0.0 ? spec.distance : (p2 - p1).Length();
            } else {
                link = makeSpring(spec.distance);
            }
            break;
        }
        case JointType::Bushing: {
            // ゴムブッシュ（B の 19）: 拘束ではなく荷重コンテナの力。並進 3 軸は
            // stiffness / damping、回転 3 軸は rotStiffness / rotDamping。
            // 取り付け座標系はアンカー + 指定軸を Z にした向き。
#ifdef WIZ_HAVE_LOADS
            if (manualMode || !jointLoads_) {
                LOGW("physics", "bushing: needs the core backend (loads are not integrated "
                                "by Chrono::Multicore) - skipped");
                return kInvalidJoint;
            }
            const ChVector3d k(spec.stiffness, spec.stiffness, spec.stiffness);
            const ChVector3d c(spec.damping, spec.damping, spec.damping);
            const ChVector3d kr(spec.rotStiffness, spec.rotStiffness, spec.rotStiffness);
            const ChVector3d cr(spec.rotDamping, spec.rotDamping, spec.rotDamping);
            auto load = makeBushing<ChLoadBodyBodyBushingMate>(
                a, b, ChFrame<>(anchor, frameRot), k, c, kr, cr, 0);
            jointLoads_->Add(load);
            rec.load = load;
            wantLimit = false;
            if (spec.breakForce > 0.0) {
                LOGW("physics", "bushing: breakforce is not supported on a load - ignored");
                rec.breakForce = 0.0;
            }
#else
            LOGW("physics", "bushing: this Chrono build has no ChLoad - skipped");
            return kInvalidJoint;
#endif
            break;
        }
    }

    if (!link && !rec.load && rec.manual.kind == JointRec::Manual::Kind::None) {
        return kInvalidJoint;
    }
    if (wantLimit && !limitDone) {
        LOGW("physics", "joint: limits could not be applied (this joint type or "
                        "Chrono version has no limit API) - ignored");
    }
    if (link) sys_->AddLink(link);
    if (rec.extra) sys_->AddLink(rec.extra);
    rec.link = link;
    LOGI("physics", "joint #%zu: type %d bodies %zu-%zu motor %d limited %d spring %s manual %d",
         joints_.size(), int(spec.type), bodyA, bodyB, int(spec.motor), int(spec.limited),
         rec.extra ? "yes" : "no", int(rec.manual.kind));
    joints_.push_back(std::move(rec));
    return joints_.size() - 1;
}

// ---- Multicore 用の手計算（モータ・ばね）----------------------------------
// Chrono::Multicore は自前の積分で、リンクが IntLoadResidual_F で足す力
// （TSDA / RSDA / トルクモータ）を拾わない。同じ意味の力積を DoStepDynamics の
// 前に速度へ織り込む（車両の「点に掛かる力」と同じ流儀）。

void PhysicsWorld::applyAngularImpulse(std::size_t id, const ChVector3d& impulse) {
    if (id >= bodies_.size() || isSoftBody(id)) return;
    auto& b = bodies_[id];
    if (b->IsFixed()) return;
    if (b->IsSleeping()) wakeUp(b.get(), 0);
    const ChQuaternion<> q = b->GetRot();
    const ChVector3d dwL = b->GetInvInertia() * q.RotateBack(impulse);
    setAngVel(b.get(), getAngVel(b.get(), 0) + q.Rotate(dwL), 0);
}

void PhysicsWorld::applyManualJoints(double dt) {
    using Kind = JointRec::Manual::Kind;
    // 軸まわりの逆慣性（ワールド軸 w について w·(R I⁻¹ Rᵀ w)）。固定は 0。
    auto invInertiaAbout = [&](const std::shared_ptr<ChBody>& body, const ChVector3d& w) {
        if (body->IsFixed()) return 0.0;
        const ChVector3d wl = body->GetRot().RotateBack(w);
        return wl.Dot(body->GetInvInertia() * wl);
    };
    auto invMassOf = [&](const std::shared_ptr<ChBody>& body) {
        return (body->IsFixed() || body->GetMass() <= 0.0) ? 0.0 : 1.0 / body->GetMass();
    };
    for (JointRec& j : joints_) {
        JointRec::Manual& m = j.manual;
        if (j.broken || (m.kind == Kind::None && !m.limited)) continue;
        if (m.a >= bodies_.size() || m.b >= bodies_.size()) continue;
        auto& a = bodies_[m.a];
        auto& b = bodies_[m.b];
        const ChVector3d axisW = a->GetRot().Rotate(m.axisLocalA);

        // ---- 可動範囲（速度レベルの片側拘束。接触の解き方と同じ）----
        if (m.limited && dt > 0.0) {
            constexpr double kBeta = 0.2;  // めり込みの押し戻し（1 ステップで 20%）
            if (m.limitLinear) {
                const double d = (b->GetPos() - a->GetPos()).Dot(axisW) - m.limitRef;
                const double vrel = (getLinVel(b.get(), 0) - getLinVel(a.get(), 0)).Dot(axisW);
                double want = vrel;
                if (d > m.limitHi) want = std::min(vrel, -kBeta * (d - m.limitHi) / dt);
                else if (d < m.limitLo) want = std::max(vrel, kBeta * (m.limitLo - d) / dt);
                const double denom = invMassOf(a) + invMassOf(b);
                if (want != vrel && denom > 0.0) {
                    const double impulse = (want - vrel) / denom;  // B に +、A に -
                    applyForce(m.b, axisW * (impulse / dt), dt);
                    applyForce(m.a, axisW * (-impulse / dt), dt);
                    m.lastForce = std::max(m.lastForce, std::fabs(impulse / dt));
                }
            } else {
                const ChQuaternion<> rel = a->GetRot().GetConjugate() * b->GetRot();
                const ChQuaternion<> delta = m.relRot0.GetConjugate() * rel;
                const double proj = delta.e1() * m.axisLocalA.x() + delta.e2() * m.axisLocalA.y() +
                                    delta.e3() * m.axisLocalA.z();
                const double angle = 2.0 * std::atan2(proj, delta.e0());
                const double wrel = (getAngVel(b.get(), 0) - getAngVel(a.get(), 0)).Dot(axisW);
                double want = wrel;
                if (angle > m.limitHi) want = std::min(wrel, -kBeta * (angle - m.limitHi) / dt);
                else if (angle < m.limitLo) want = std::max(wrel, kBeta * (m.limitLo - angle) / dt);
                const double denom = invInertiaAbout(a, axisW) + invInertiaAbout(b, axisW);
                if (want != wrel && denom > 0.0) {
                    const double impulse = (want - wrel) / denom;  // 角力積。B に +、A に -
                    applyAngularImpulse(m.b, axisW * impulse);
                    applyAngularImpulse(m.a, axisW * (-impulse));
                    m.lastTorque = std::max(m.lastTorque, std::fabs(impulse / dt));
                }
            }
        }

        switch (m.kind) {
            case Kind::Torque: {
                // A に +τ、B に -τ（Chrono のモータと同じく body1 を回す向き）。
                const ChVector3d L = axisW * (m.target * dt);
                applyAngularImpulse(m.a, L);
                applyAngularImpulse(m.b, -L);
                m.lastTorque = std::fabs(m.target);
                break;
            }
            case Kind::Force: {
                const ChVector3d F = axisW * m.target;
                applyForce(m.a, F, dt);
                applyForce(m.b, -F, dt);
                m.lastForce = std::fabs(m.target);
                break;
            }
            case Kind::Spring: {
                const ChVector3d p1 = a->GetPos() + a->GetRot().Rotate(m.p1Local);
                const ChVector3d p2 = b->GetPos() + b->GetRot().Rotate(m.p2Local);
                const ChVector3d d = p2 - p1;
                const double len = d.Length();
                if (len < 1e-9) break;
                const ChVector3d dir = d * (1.0 / len);
                const double vrel = (bodyPointVelocity(m.b, p2) - bodyPointVelocity(m.a, p1)).Dot(dir);
                // 正 = 縮める向き（伸びているか、離れつつあるとき）。
                const double f = m.k * (len - m.rest) + m.c * vrel;
                applyForceAtPoint(m.a, dir * f, p1, dt);
                applyForceAtPoint(m.b, dir * (-f), p2, dt);
                m.lastForce = std::fabs(f);
                break;
            }
            case Kind::Rsda: {
                // B の A に対する回転を軸まわりの角度に落とす（開始姿勢が自然角）。
                const ChQuaternion<> rel = a->GetRot().GetConjugate() * b->GetRot();
                const ChQuaternion<> delta = m.relRot0.GetConjugate() * rel;
                const double proj = delta.e1() * m.axisLocalA.x() + delta.e2() * m.axisLocalA.y() +
                                    delta.e3() * m.axisLocalA.z();
                const double angle = 2.0 * std::atan2(proj, delta.e0());
                const double wrel = (getAngVel(b.get(), 0) - getAngVel(a.get(), 0)).Dot(axisW);
                const double tau = m.k * angle + m.c * wrel;  // B を戻す向きの大きさ
                const ChVector3d L = axisW * (tau * dt);
                applyAngularImpulse(m.b, -L);
                applyAngularImpulse(m.a, L);
                m.lastTorque = std::fabs(tau);
                break;
            }
            case Kind::None:
                break;
        }
    }
}

void PhysicsWorld::removeAllJoints() {
    diagSteps_ = 0;  // 次のシミュレート開始で診断行を出し直す
    diagNanReported_ = false;
    bool anyLoad = false;
    for (auto& j : joints_) {
        // 破断したリンクも無効化されて系に残っている（走行中の RemoveLink は
        // Multicore を壊す）ので、ここでまとめて外す。
        if (j.link) sys_->RemoveLink(j.link);
        if (j.extra) sys_->RemoveLink(j.extra);
        if (j.load) anyLoad = true;
    }
#ifdef WIZ_HAVE_LOADS
    // ブッシュはジョイント用の荷重コンテナにだけ入っているので、まとめて捨てる。
    if (anyLoad && jointLoads_) clearLoads(jointLoads_.get(), 0);
#else
    (void)anyLoad;
#endif
    joints_.clear();
    brokenPending_.clear();
    // 拘束の増減はソルバの構成を変えるので、Setup で作り直させる
    // （wakeAll と同じ理由）。
    sys_->Setup();
}

std::size_t PhysicsWorld::jointCount() const {
    return joints_.size();
}

bool PhysicsWorld::setJointMotorTarget(std::size_t joint, double target) {
    if (joint >= joints_.size()) return false;
    JointRec& j = joints_[joint];
    if (j.broken) return false;
    if (j.manual.kind == JointRec::Manual::Kind::Torque ||
        j.manual.kind == JointRec::Manual::Kind::Force) {
        j.manual.target = target;
        return true;
    }
    if (j.rampMotor) {
        // 今の角度 / 位置から新しい傾きで引き直す（跳びを作らない）。
        const double now = sys_->GetChTime();
        if (j.rampLinear) {
            auto pm = std::dynamic_pointer_cast<ChLinkMotorLinearPosition>(j.rampMotor);
            if (!pm) return false;
            pm->SetMotorFunction(chrono_types::make_shared<WizRampFunction>(
                pm->GetMotorPos() - target * now, target));
        } else {
            auto am = std::dynamic_pointer_cast<ChLinkMotorRotationAngle>(j.rampMotor);
            if (!am) return false;
            am->SetMotorFunction(chrono_types::make_shared<WizRampFunction>(
                am->GetMotorAngle() - target * now, target));
        }
        return true;
    }
    if (!j.motorFn) return false;
    setConstValue(static_cast<WizConstFunction*>(j.motorFn.get()), target, 0);
    return true;
}

bool PhysicsWorld::jointReaction(std::size_t joint, double& force, double& torque) const {
    force = 0.0;
    torque = 0.0;
    if (joint >= joints_.size()) return false;
    const JointRec& j = joints_[joint];
    if (j.broken) return false;
    if (j.manual.kind != JointRec::Manual::Kind::None) {
        // 手計算のばね / モータ: 拘束の反力があればそれに手計算のぶんを足す。
        double lf = 0.0, lt = 0.0;
        if (j.link) linkReaction(j.link.get(), lf, lt, 0);
        force = std::max(lf, j.manual.lastForce);
        torque = std::max(lt, j.manual.lastTorque);
        return true;
    }
#ifdef WIZ_HAVE_LOADS
    if (j.load) {
        auto bushing = std::dynamic_pointer_cast<ChLoadBodyBodyBushingMate>(j.load);
        return bushing && loadReaction(bushing.get(), force, torque, 0);
    }
#endif
    if (!j.link) return false;
    if (j.type == JointType::Spring) {
        // ばねは拘束ではないので反力の口が無い。ばね力を返す。
        if (auto s = std::dynamic_pointer_cast<ChLinkTSDA>(j.link)) {
            force = std::fabs(s->GetForce());
            return true;
        }
        return false;
    }
    return linkReaction(j.link.get(), force, torque, 0);
}

bool PhysicsWorld::jointBroken(std::size_t joint) const {
    return joint < joints_.size() && joints_[joint].broken;
}

std::vector<std::size_t> PhysicsWorld::takeBrokenJoints() {
    std::vector<std::size_t> out;
    out.swap(brokenPending_);
    return out;
}

void PhysicsWorld::checkJointBreaks() {
    // 開始直後はソルバが収束途中で反力が跳ねる（Multicore で静止荷重 59 N の
    // 棒が 1 ステップ目に 325 N を返した）ので、最初の数ステップは見ない。
    constexpr int kWarmupSteps = 10;
    if (diagSteps_ < kWarmupSteps) return;
    for (std::size_t i = 0; i < joints_.size(); ++i) {
        JointRec& j = joints_[i];
        if (j.broken || j.breakForce <= 0.0 || j.load) continue;
        double force = 0.0, torque = 0.0;
        if (!jointReaction(i, force, torque)) continue;
        if (!std::isfinite(force) || force <= j.breakForce) continue;
        // 外すのではなく無効化する。走行中の RemoveLink + Setup は Multicore の
        // データマネージャを壊し、拘束の付いた物が全部 NaN になった。
        // 系からは次の removeAllJoints（停止時）でまとめて外す。
        bool ok = true;
        if (j.link) ok = disableLink(j.link.get(), 0) && ok;
        if (j.extra) ok = disableLink(j.extra.get(), 0) && ok;
        if (!ok) {
            LOGW("physics", "joint #%zu: this Chrono has no SetDisabled - break ignored", i);
            j.breakForce = 0.0;  // 二度と試さない
            continue;
        }
        j.broken = true;
        brokenPending_.push_back(i);
        LOGI("physics", "joint #%zu broke (reaction %.1f N > %.1f N)", i, force,
             j.breakForce);
    }
}

// ---- ケーブル（FEA。B の 15）-----------------------------------------------
// ANCF ケーブル要素を直線に並べ、端を固定点 / ボディ / 自由にする。節点の
// 球で接触する（ChContactSurfaceNodeCloud）。Core 専用: Multicore のデータ
// マネージャは剛体しか持たず、FEA の要素は積分に乗らない。

std::size_t PhysicsWorld::addCable(const CableSpec& spec) {
#ifdef WIZ_HAVE_FEA
    static bool warnedBackend = false;
    if (backend_ == PhysicsBackend::Multicore) {
        if (!warnedBackend) {
            warnedBackend = true;
            LOGW("physics", "cable: needs the core backend (Chrono::Multicore has no FEA) - "
                            "skipped");
        }
        return kInvalidId;
    }
    auto endOk = [&](CableSpec::End end, std::size_t id, const char* which) {
        if (end != CableSpec::End::Body) return true;
        if (id >= bodies_.size() || !active_[id] || softOf_[id] != kNoSoft) {
            LOGW("physics", "cable: end %s refers to a missing or soft body (%zu) - skipped",
                 which, id);
            return false;
        }
        return true;
    };
    if (!endOk(spec.endA, spec.bodyA, "A") || !endOk(spec.endB, spec.bodyB, "B")) {
        return kInvalidId;
    }
    const int n = std::max(2, std::min(64, spec.segments));
    const double length = (spec.b - spec.a).Length();
    if (length < 1e-6) {
        LOGW("physics", "cable: zero length - skipped");
        return kInvalidId;
    }

    auto mesh = chrono_types::make_shared<fea::ChMesh>();
    auto section = chrono_types::make_shared<fea::ChBeamSectionCable>();
    section->SetDiameter(spec.diameter);
    section->SetYoungModulus(spec.young);
    setSectionDensity(section.get(), spec.density, 0);
    setSectionDamping(section.get(), spec.damping, 0);

    fea::ChBuilderCableANCF builder;
    builder.BuildBeam(mesh, section, n, spec.a, spec.b);
    CableRec rec;
    rec.mesh = mesh;
    rec.nodes = builder.GetLastBeamNodes();
    if (rec.nodes.size() < 2) {
        LOGW("physics", "cable: the beam builder produced no nodes - skipped");
        return kInvalidId;
    }

    if (spec.collide) {
        // 節点ごとの球（半径 = ケーブルの半径）。材質は剛体と同じ流儀。
        // 面はメッシュを知っている必要がある: コンストラクタで渡し、
        // AddContactSurface でも結び直される。
        auto mat = materialFor(spec.options);
        auto surf = chrono_types::make_shared<fea::ChContactSurfaceNodeCloud>(mat, mesh.get());
        mesh->AddContactSurface(surf);
        // 留め先のボディの内側にある節点は入れない（端の節点はボディの中心に
        // 留まるので、入れると自分の留め先と深くめり込んだ接触ができて
        // 留め先が暴れる）。
        const double radius = spec.diameter * 0.5;
        std::size_t added = 0;
        for (const auto& node : rec.nodes) {
            const ChVector3d p = node->GetPos();
            if (spec.endA == CableSpec::End::Body && (p - spec.a).Length() < spec.clearA) continue;
            if (spec.endB == CableSpec::End::Body && (p - spec.b).Length() < spec.clearB) continue;
            surf->AddNode(node, radius);
            ++added;
        }
        if (added == 0) {
            LOGW("physics", "cable: every node lies inside an attached body - no contact");
        }
    }
    setMeshGravity(mesh.get(), true, 0);
    sys_->Add(mesh);
    // 衝突系が初期化済みなら登録する（ボディと同じ理由: Chrono 9 は後から
    // 足した物を BindAll で拾わない）。
    if (spec.collide) {
        auto coll = sys_->GetCollisionSystem();
        if (coll && coll->IsInitialized()) coll->BindItem(mesh);
    }

    auto attach = [&](const std::shared_ptr<fea::ChNodeFEAxyzD>& node, CableSpec::End end,
                      std::size_t id) {
        if (end == CableSpec::End::Fixed) {
            node->SetFixed(true);
            return;
        }
        if (end != CableSpec::End::Body) return;
        auto link = chrono_types::make_shared<WizNodeFrameLink>();
        link->Initialize(node, bodies_[id]);
        sys_->AddLink(link);
        rec.links.push_back(link);
        wakeUp(bodies_[id].get(), 0);
    };
    attach(rec.nodes.front(), spec.endA, spec.bodyA);
    attach(rec.nodes.back(), spec.endB, spec.bodyB);
    rec.valid = true;
    cables_.push_back(std::move(rec));
    sys_->Setup();
    LOGI("physics", "cable #%zu: %d elements, %zu nodes, length %.2f m, young %.3g Pa, "
                    "collide %d",
         cables_.size() - 1, n, cables_.back().nodes.size(), length, spec.young,
         int(spec.collide));
    return cables_.size() - 1;
#else
    (void)spec;
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOGW("physics", "this Chrono build has no FEA headers - cables are skipped");
    }
    return kInvalidId;
#endif
}

void PhysicsWorld::removeAllCables() {
#ifdef WIZ_HAVE_FEA
    if (cables_.empty()) return;
    for (auto& c : cables_) {
        for (auto& l : c.links) sys_->RemoveLink(l);
        if (c.mesh) removeMeshFrom(sys_.get(), c.mesh, 0);
    }
    cables_.clear();
    sys_->Setup();
#endif
}

std::size_t PhysicsWorld::cableCount() const {
    return cables_.size();
}

void PhysicsWorld::cableNodePositions(std::size_t cable, std::vector<float>& out) const {
    out.clear();
#ifdef WIZ_HAVE_FEA
    if (cable >= cables_.size() || !cables_[cable].valid) return;
    const CableRec& c = cables_[cable];
    out.reserve(c.nodes.size() * 3);
    for (const auto& node : c.nodes) {
        const ChVector3d p = node->GetPos();
        out.push_back(float(p.x()));
        out.push_back(float(p.y()));
        out.push_back(float(p.z()));
    }
#else
    (void)cable;
#endif
}

double PhysicsWorld::cableTension(std::size_t cable) const {
    if (cable >= cables_.size()) return 0.0;
    double best = 0.0;
    for (const auto& l : cables_[cable].links) {
        double f = 0.0, t = 0.0;
        if (linkReaction(l.get(), f, t, 0) && std::isfinite(f)) best = std::max(best, f);
    }
    return best;
}

// ---- モーダル解析（Chrono::Modal。B の 20）--------------------------------
// 系の質量行列 M・剛性行列 K・拘束ヤコビアン Cq を取り出して、非減衰の
// 一般化固有値問題を Lanczos で解く。接触は入らない（K に無い）。剛体だけの
// 系では拘束のモードしか出ないので、ケーブル（FEA）がある系で意味を持つ。

bool PhysicsWorld::modalFrequencies(int count, std::vector<double>& hz, std::string& why) {
    hz.clear();
#ifdef WIZ_HAVE_MODAL
    if (backend_ == PhysicsBackend::Multicore) {
        why = "modal analysis needs the core backend";
        return false;
    }
    if (count < 1) {
        why = "count must be >= 1";
        return false;
    }
    try {
        sys_->Setup();
        sys_->Update();
        ChSparseMatrix M, K, Cq;
        if (!systemMatrices(sys_.get(), M, K, Cq, 0)) {
            why = "this Chrono has no system matrix accessors";
            return false;
        }
        if (M.rows() == 0) {
            why = "the system is empty";
            return false;
        }
        modal::ChModalSolveUndamped solver(count, 1e-5, 500, 1e-10, false,
                                           modal::ChGeneralizedEigenvalueSolverLanczos());
        ChMatrixDynamic<std::complex<double>> V;
        ChVectorDynamic<std::complex<double>> eig;
        ChVectorDynamic<double> freq;
        solver.Solve(M, K, Cq, V, eig, freq);
        for (int i = 0; i < freq.size(); ++i) {
            if (std::isfinite(freq(i))) hz.push_back(freq(i));
        }
        std::sort(hz.begin(), hz.end());
        if (int(hz.size()) > count) hz.resize(std::size_t(count));
        return true;
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
#else
    (void)count;
    why = "Chrono::Modal is not built in (configure with -DWIZ_WITH_CHRONO_MODAL=ON)";
    return false;
#endif
}
