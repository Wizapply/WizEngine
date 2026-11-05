// 固定物どうしの衝突ペアを作らせない（フィルタの定義は PhysicsWorldInternal.h）。

#include "physics/PhysicsWorldInternal.h"
#include "core/Log.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionSystem.h>
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
#include <chrono/collision/bullet/ChCollisionSystemBullet.h>
#include <chrono/collision/bullet/ChCollisionModelBullet.h>
#endif
#include <chrono/physics/ChBody.h>

using namespace chrono;
using namespace wizengine::physics::compat;

// ---- 固定物どうしの衝突ペアを作らせない（Core = Bullet のみ）----------------
// Chrono は固定ボディ（SetFixed）を Bullet に「静的」と伝えない: Bullet 側の
// オブジェクトには CF_STATIC_OBJECT も非アクティブ状態も付かず、衝突ファミリが
// 同じなら固定物どうしも普通のペアになる（Chrono 9.0.0 の
// cbtOverlappingPairCache.h の needsBroadphaseCollision と
// cbtCollisionDispatcher.cpp の needsCollision）。動かない物どうしの接触は
// ソルバでは何の役にも立たないのに、狭域判定は毎ステップ走る。固定物どうし
// 一般（階段の部品と床、固定の三角メッシュと床など）の対策で、重なった固定物の
// 組ごとに毎ステップ判定が走るのを止める（以前のテレインの三角メッシュの
// チャンクでは、隣どうしのチャンクと床の判定が 44 組でき、物が 1 つも無くても
// 22 ms/step 掛かった。Chrono 単体のベンチ。除外すると 0.4〜0.6 ms）。
//
// そこで Bullet の広域判定の重なりフィルタ（cbtOverlapFilterCallback。ヘッダ
// だけの仮想インターフェース = DLL の書き出しに依存しない）を付け、既定の
// 判定（ファミリの group / mask）に「両方が固定なら作らない」を足す。固定物の
// 見分けは Bullet オブジェクトの userPointer（= Chrono が入れる
// ChCollisionModelBullet*）の集合で行う。集合は物理スレッドだけが触る
// （フィルタが呼ばれるのも衝突系への登録と DoStepDynamics の中 = 同じスレッド）。
//
// フィルタはペアが「できるとき」にしか効かないので、
//  - 既にできた固定どうしのペアは自分で消す（初期化直後・固定にしたとき）。
//  - 固定を外したときは、消したペアを Bullet が自分から作り直さない（DBVT は
//    AABB が太らせた箱をはみ出して動くまで相手を探し直さない）ので、衝突モデルを
//    入れ直して（EnableCollision の切り替え = 衝突系の Remove / Add）探させる。
// Multicore は自前の衝突系なので何もしない。

#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
namespace {
// ボディの Bullet 側の実体（= その Bullet オブジェクトの userPointer）。
// 衝突系に登録されていなければ null。
const void* bulletKeyOf(const ChBody& body) {
    const auto model = body.GetCollisionModel();
    if (!model || !model->HasImplementation()) return nullptr;
    return static_cast<const void*>(
        static_cast<ChCollisionModelBullet*>(model->GetImplementation()));
}
}  // namespace
#endif

void PhysicsWorld::attachStaticPairFilter() {
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    if (!staticPairs_) staticPairs_ = std::make_shared<StaticPairFilter>();
    // 古い系のワールドは sys_ と一緒に壊れている（recreate が先に reset する）。
    staticPairs_->statics.clear();
    staticPairs_->world = nullptr;
    staticPairs_->ready = false;
    if (backend_ == PhysicsBackend::Multicore) return;
    auto bt = std::dynamic_pointer_cast<ChCollisionSystemBullet>(sys_->GetCollisionSystem());
    if (!bt || !bt->GetBulletCollisionWorld()) return;
    staticPairs_->world = bt->GetBulletCollisionWorld();
    staticPairs_->world->getPairCache()->setOverlapFilterCallback(staticPairs_.get());
#endif
}

void PhysicsWorld::syncStaticPairs() {
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    if (!staticPairs_ || !staticPairs_->world) return;
    StaticPairFilter& f = *staticPairs_;
    f.statics.clear();
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        if (!active_[i] || !bodies_[i]->IsFixed()) continue;
        if (const void* key = bulletKeyOf(*bodies_[i])) f.statics.insert(key);
    }
    f.ready = true;
    const std::size_t removed = f.purge();
    LOGI("physics", "static pair filter: %zu fixed bodies, removed %zu fixed-fixed pairs",
         f.statics.size(), removed);
#endif
}

void PhysicsWorld::updateStaticPairs(ChBody& body) {
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    // 初期化前は何もしない（実体がまだ無い。初期化直後の syncStaticPairs が拾う）。
    if (!staticPairs_ || !staticPairs_->world || !staticPairs_->ready) return;
    StaticPairFilter& f = *staticPairs_;
    const void* key = bulletKeyOf(body);
    if (!key) return;
    if (body.IsFixed()) {
        if (f.statics.insert(key).second) f.purge();
        return;
    }
    if (f.statics.erase(key) == 0) return;  // もともと固定物として扱っていない
    // 固定だった間に消した / 作らせなかったペアを作り直させる。Remove で
    // このボディのペアと接触多様体が全部消え、Add（createProxy）で今の
    // 重なりが改めてフィルタに掛かる（もう固定ではないので通る）。
    if (body.IsCollisionEnabled()) {
        body.EnableCollision(false);
        body.EnableCollision(true);
    }
#else
    (void)body;
#endif
}

void PhysicsWorld::forgetStaticPairs(ChBody& body) {
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    if (!staticPairs_ || !staticPairs_->world) return;
    if (const void* key = bulletKeyOf(body)) staticPairs_->statics.erase(key);
#else
    (void)body;
#endif
}
