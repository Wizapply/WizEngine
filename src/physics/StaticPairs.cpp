// 固定物どうしの衝突ペアを作らせない。Bullet のヘッダを読むのはこのファイルだけ。

#include "physics/StaticPairs.h"
#include "core/Log.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/PhysicsSystem.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionSystem.h>
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
#include <chrono/collision/bullet/BulletCollision/CollisionDispatch/cbtCollisionWorld.h>
#include <chrono/collision/bullet/ChCollisionSystemBullet.h>
#include <chrono/collision/bullet/ChCollisionModelBullet.h>
#endif
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystem.h>

#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

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

namespace wizengine {
namespace physics {

// フィルタが無いビルドでは定義しない（StaticPairs は生成しないので完全型が要らない）。
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
struct StaticPairFilter : public cbtOverlapFilterCallback {
    std::unordered_set<const void*> statics;  // 固定ボディの ChCollisionModelBullet*
    cbtCollisionWorld* world = nullptr;       // 付けている衝突ワールド（null = 無効）
    bool ready = false;  // 衝突系の初期化後に集合を作った（それまでは実体が無い）

    bool isStatic(const cbtBroadphaseProxy* p) const {
        const auto* o = static_cast<const cbtCollisionObject*>(p->m_clientObject);
        return o && statics.count(o->getUserPointer()) != 0;
    }
    bool needBroadphaseCollision(cbtBroadphaseProxy* p0, cbtBroadphaseProxy* p1) const override {
        // 既定の判定（衝突ファミリの group / mask）はそのまま残す。
        bool collides = (p0->m_collisionFilterGroup & p1->m_collisionFilterMask) != 0;
        collides = collides && (p1->m_collisionFilterGroup & p0->m_collisionFilterMask);
        if (!collides) return false;
        return !(isStatic(p0) && isStatic(p1));
    }
    // 既にある固定どうしのペアを消す。消した数を返す。
    std::size_t purge() {
        if (!world) return 0;
        cbtOverlappingPairCache* cache = world->getPairCache();
        cbtBroadphasePairArray& pairs = cache->getOverlappingPairArray();
        std::vector<std::pair<cbtBroadphaseProxy*, cbtBroadphaseProxy*>> doomed;
        for (int i = 0; i < pairs.size(); ++i) {
            if (isStatic(pairs[i].m_pProxy0) && isStatic(pairs[i].m_pProxy1)) {
                doomed.emplace_back(pairs[i].m_pProxy0, pairs[i].m_pProxy1);
            }
        }
        // 走査中に消すと配列が詰まるので、集めてから消す。
        for (const auto& d : doomed) cache->removeOverlappingPair(d.first, d.second, world->getDispatcher());
        return doomed.size();
    }
};
#endif

void StaticPairs::attach() {
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    if (!filter_) filter_ = std::make_shared<StaticPairFilter>();
    // 古い系のワールドは系と一緒に壊れている（recreate が先に reset する）。
    filter_->statics.clear();
    filter_->world = nullptr;
    filter_->ready = false;
    if (system_.backend() == PhysicsBackend::Multicore) return;
    auto bt = std::dynamic_pointer_cast<ChCollisionSystemBullet>(system_.sys()->GetCollisionSystem());
    if (!bt || !bt->GetBulletCollisionWorld()) return;
    filter_->world = bt->GetBulletCollisionWorld();
    filter_->world->getPairCache()->setOverlapFilterCallback(filter_.get());
#endif
}

void StaticPairs::syncIfPending() {
    // 固定物どうしのペアの掃除: 衝突系の初期化（BindAll）は最初の
    // DoStepDynamics の中で起きるので、その前後で「初期化済みなのに集合が
    // まだ」を見て 1 回だけ作る（初期化を別の呼び出しが先に済ませていても
    // 拾えるよう、前でも見る）。最初の 1 ステップだけは固定どうしの判定が
    // 走る。
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    if (!filter_ || !filter_->world || filter_->ready) return;
    const auto coll = system_.sys()->GetCollisionSystem();
    if (coll && coll->IsInitialized()) sync();
#endif
}

void StaticPairs::sync() {
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    if (!filter_ || !filter_->world) return;
    StaticPairFilter& f = *filter_;
    f.statics.clear();
    for (std::size_t i = 0; i < bodies_.count(); ++i) {
        if (!bodies_.active(i) || !bodies_.body(i)->IsFixed()) continue;
        if (const void* key = bulletKeyOf(*bodies_.body(i))) f.statics.insert(key);
    }
    f.ready = true;
    const std::size_t removed = f.purge();
    LOGI("physics", "static pair filter: %zu fixed bodies, removed %zu fixed-fixed pairs",
         f.statics.size(), removed);
#endif
}

void StaticPairs::update(ChBody& body) {
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    // 初期化前は何もしない（実体がまだ無い。初期化直後の sync が拾う）。
    if (!filter_ || !filter_->world || !filter_->ready) return;
    StaticPairFilter& f = *filter_;
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

void StaticPairs::forget(ChBody& body) {
#ifdef WIZ_HAVE_BULLET_PAIR_FILTER
    if (!filter_ || !filter_->world) return;
    if (const void* key = bulletKeyOf(body)) filter_->statics.erase(key);
#else
    (void)body;
#endif
}

void StaticPairs::onBodyAdded(std::size_t id, const BodyAddInfo& info) {
    (void)info;
    // 走行中に足した固定ボディ（テレインの作り直し・プレハブの固定部品など）:
    // 登録の時点でできた固定どうしのペアを消す。
    ChBody& body = *bodies_.body(id);
    if (body.IsFixed()) update(body);
}

void StaticPairs::onBodyRetiring(std::size_t id) {
    // 固定物の集合から先に外す（retire の EnableCollision(false) で Bullet 側の
    // 実体が解放され、そのアドレスが別のボディに使い回されうるため）。
    forget(*bodies_.body(id));
}

void StaticPairs::onBodyFixedChanged(std::size_t id) {
    // 固定物どうしのペア: 固定にしたら掃除、外したら作り直させる。
    if (bodies_.active(id)) update(*bodies_.body(id));
}

}  // namespace physics
}  // namespace wizengine
