// ボディの名簿: 番号の登録・別名・衝突系への登録・退場。

#include "physics/PhysicsWorldInternal.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/physics/ChBody.h>
#ifdef WIZ_HAVE_LOADS
#include <chrono/physics/ChLoadContainer.h>
#endif

#include <memory>

using namespace chrono;
using namespace wizengine::physics::compat;

void PhysicsWorld::registerBody(const std::shared_ptr<chrono::ChBody>& body) {
    bodyIndex_[body.get()] = bodies_.size();
    bodies_.push_back(body);
    active_.push_back(true);
    softOf_.push_back(kNoSoft);  // addSoftBody が粒子ぶんを後から書き換える
    alias_.push_back(kNoSoft);
    mats_.push_back(nullptr);  // finishBody が専用材質なら書く
    options_.push_back(BodyOptions{});
    loadsOf_.emplace_back();
    terrainProxies_.emplace_back();  // 形の記録は add* が後から書く（無ければ対象外）
    bindCollision(body);
    // 走行中に足した固定ボディ（テレインの作り直し・プレハブの固定部品など）:
    // 登録の時点でできた固定どうしのペアを消す。
    if (body->IsFixed()) updateStaticPairs(*body);
    resetSleepTimer(*body);  // 新規ボディの計時は 0 = 最初のステップで眠る
}

void PhysicsWorld::setAlias(std::size_t id, std::size_t owner) {
    if (id >= alias_.size() || owner >= bodies_.size() || id == owner) return;
    alias_[id] = owner;
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

std::size_t PhysicsWorld::bodyCount() const {
    return bodies_.size();
}

void PhysicsWorld::disableBody(std::size_t id) {
    if (id >= bodies_.size() || !active_[id]) return;
    // テレインのゴーストは PhysicsWorld の持ち物: 系から外すと、直前の
    // ステップの接触が外れたボディを指したまま残るので、外さずに接触だけ止める。
    if (terrain_ && id == terrain_->ghost) {
        clearTerrain();
        return;
    }
    if (SoftBody* soft = softOfRoot(id)) {
        // 粒子を全部退場させ、ばねも解かない。
        soft->active = false;
        for (const std::size_t p : soft->particles) {
            if (p == id) continue;  // 代表は下の通常経路で
            auto& pb = bodies_[p];
            pb->SetFixed(true);
            forgetStaticPairs(*pb);  // 衝突モデルを手放す前に
            if (backend_ != PhysicsBackend::Multicore) pb->EnableCollision(false);
            pb->ForceToRest();
            pb->SetPos(ChVector3d(0, -1000.0, 0));
            active_[p] = false;
        }
    }
    auto& b = bodies_[id];
    b->SetFixed(true);
    // 固定物の集合から先に外す（下の EnableCollision(false) で Bullet 側の
    // 実体が解放され、そのアドレスが別のボディに使い回されうるため）。
    forgetStaticPairs(*b);
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
    forgetStaticPairs(*b);  // RemoveBody が衝突モデルを手放す前に
    sys_->RemoveBody(b);
}

bool PhysicsWorld::bodyActive(std::size_t id) const {
    return id < active_.size() && active_[id];
}
