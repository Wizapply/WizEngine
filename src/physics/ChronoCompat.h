#pragma once

// Chrono の版やビルド構成の違いを吸収する部品（ヘッダの有無の判定と SFINAE の関数）を集めたヘッダ。
// src/physics/ の .cpp 専用。他の層から include しない。
// ヘッダの有無の判定はここ 1 か所に置き、機能の本体のヘッダは使う .cpp が
// #ifdef WIZ_HAVE_X で読む（ここに残すのは下の関数と別名の宣言に要るものだけ）。

// 下の関数の引数と戻り値の型。
#include <chrono/core/ChFrame.h>
#include <chrono/core/ChQuaternion.h>
#include <chrono/core/ChVector3.h>

// 複合形状（プレハブの collide 部品）。Chrono 9 の形状クラス。ヘッダが無い
// 版では複合形状を諦めて警告だけ出す（ビルドは止めない）。
#if __has_include(<chrono/collision/ChCollisionShapeBox.h>)
#include <chrono/collision/ChCollisionShape.h>  // addShapeImpl の引数
#define WIZ_HAVE_COLLISION_SHAPES 1
#endif
// 円柱の当たり形状（プレハブの円柱部品）と三角メッシュ（glTF の凹形状）。
// どちらも Chrono 9 の形状クラスで、無い版ではその形だけ諦めて警告する。
#if __has_include(<chrono/collision/ChCollisionShapeCylinder.h>)
#define WIZ_HAVE_CYLINDER_SHAPE 1
#endif
#if __has_include(<chrono/collision/ChCollisionShapeTriangleMesh.h>)
#define WIZ_HAVE_TRIMESH_SHAPE 1
#endif
// 固定物どうしのペアを作らせる前に止める Bullet の重なりフィルタ
// （StaticPairFilter）。無い版ではフィルタ無し = 従来どおり。
#if __has_include(<chrono/collision/bullet/ChCollisionSystemBullet.h>) && \
    __has_include(<chrono/collision/bullet/ChCollisionModelBullet.h>) && \
    __has_include(<chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtCollisionWorld.h>)
#define WIZ_HAVE_BULLET_PAIR_FILTER 1
#endif
// ---- B の導入で足したヘッダ。どれも __has_include で見て、無い版・無い
// ビルドではその機能だけ「無い」と答える（physicsFeatures）。
// 直接法（Eigen の SparseLU / SparseQR は Core 組み込み）。
#if __has_include(<chrono/solver/ChDirectSolverLS.h>)
#define WIZ_HAVE_DIRECT_SOLVERS 1
#endif
// ADMM（FEA の剛性行列と NSC の接触を同時に解ける VI ソルバ。内側は直接法）。
#if __has_include(<chrono/solver/ChSolverADMM.h>)
#define WIZ_HAVE_ADMM 1
#endif
#if __has_include(<chrono_pardisomkl/ChSolverPardisoMKL.h>) && defined(WIZ_WITH_PARDISO)
#define WIZ_HAVE_PARDISO 1
#endif
#if __has_include(<chrono_mumps/ChSolverMumps.h>) && defined(WIZ_WITH_MUMPS)
#define WIZ_HAVE_MUMPS 1
#endif
// 荷重（ChLoad）: ブッシュと定常荷重。
#if __has_include(<chrono/physics/ChLoadContainer.h>) && __has_include(<chrono/physics/ChLoadsBody.h>)
#include <chrono/core/ChTypes.h>             // makeBushing の make_shared
#include <chrono/physics/ChBody.h>           // makeBushing の引数
#include <chrono/physics/ChLoadContainer.h>  // removeLoadFrom の ChLoadBase
#define WIZ_HAVE_LOADS 1
#endif
// FEA のケーブル（ANCF）。Chrono 9 は ChLinkNodeFrame、旧版は ChLinkPointFrame。
#if __has_include(<chrono/fea/ChBuilderBeam.h>) && __has_include(<chrono/fea/ChMesh.h>)
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
#include <chrono/core/ChMatrix.h>  // systemMatrices の ChSparseMatrix
#define WIZ_HAVE_MODAL 1
#endif
// 歯車とねじは版で置き場が変わったので __has_include
// で見て、無ければその種類だけ「作らない」で済ませる（Distance と同じ扱い）。
#if __has_include(<chrono/physics/ChLinkLockGear.h>)
#define WIZ_HAVE_GEAR 1
#endif
#if __has_include(<chrono/physics/ChLinkLockScrew.h>)
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

#include <algorithm>
#include <memory>
#include <type_traits>

namespace wizengine {
namespace physics {
namespace compat {

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

// 眠りの計時のやり直しに使う 2 つ（PhysicsWorld::resetSleepTimer）。項目ごとの時計
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
// リンクの一覧。Chrono 9 は GetLinks、旧版は Get_linklist。
template <typename S>
auto linksOf(const S* s, int) -> decltype(s->GetLinks()) {
    return s->GetLinks();
}
template <typename S>
auto linksOf(const S* s, long) -> decltype(s->Get_linklist()) {
    return s->Get_linklist();
}
template <typename S>
auto linkCountOf(S* s, int) -> decltype(linksOf(s, 0).size(), int()) {
    return int(linksOf(s, 0).size());
}
template <typename S>
int linkCountOf(S*, ...) {
    return -1;
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

}  // namespace compat
}  // namespace physics
}  // namespace wizengine
