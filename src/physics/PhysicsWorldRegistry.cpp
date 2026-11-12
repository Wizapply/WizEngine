// ボディの登録と退場の順番（名簿と、まだ窓口が持つ配列を揃える）。

#include "physics/PhysicsWorldInternal.h"

#include <chrono/physics/ChBody.h>

#include <memory>

void PhysicsWorld::registerBody(const std::shared_ptr<chrono::ChBody>& body,
                                const std::shared_ptr<chrono::ChContactMaterial>& mat,
                                const BodyOptions& options, bool allowSleeping) {
    terrainProxies_.emplace_back();  // 形の記録は add* が後から書く（無ければ対象外）
    const std::size_t id =
        p_->bodies.add(body, wizengine::physics::BodyAddInfo{options, mat, allowSleeping});
    // 固定ボディは resetTimer が何もしないので、通知の中の固定物のペアの更新
    // （固定ボディだけ）との前後には依存しない。
    p_->sleeping.resetTimer(id);  // 新規ボディの計時は 0 = 最初のステップで眠る
}

void PhysicsWorld::setAlias(std::size_t id, std::size_t owner) {
    p_->bodies.setAlias(id, owner);
}

std::size_t PhysicsWorld::bodyCount() const {
    return p_->bodies.count();
}

void PhysicsWorld::disableBody(std::size_t id) {
    if (id >= p_->bodies.count() || !p_->bodies.active(id)) return;
    // テレインのゴーストは PhysicsWorld の持ち物: 系から外すと、直前の
    // ステップの接触が外れたボディを指したまま残るので、外さずに接触だけ止める。
    if (terrain_ && id == terrain_->ghost) {
        clearTerrain();
        return;
    }
    if (SoftBody* soft = softOfRoot(id)) {
        // 粒子を全部退場させ、ばねも解かない。
        soft->active = false;
        for (const std::size_t p : p_->bodies.members(soft->root)) {
            if (p == id) continue;  // 代表は下の通常経路で
            p_->bodies.retire(p);
        }
    }
    p_->bodies.retire(id);
    // Core では本当に系から外す（B の 18）。粒子も同じ。
    if (SoftBody* soft = softOfRoot(id)) {
        for (const std::size_t p : p_->bodies.members(soft->root)) detachBody(p);
    } else {
        detachBody(id);
    }
}

// Core（Bullet）は RemoveBody を実装しているので、退場したボディは系から
// 本当に外す: ソルバと衝突系がその変数・形状を見なくなり、エディタで
// 大きさを何度も変えても空のボディが溜まらない。ChBody のオブジェクト自体は
// 名簿に残す（番号を詰めない・接触コールバックの逆引きが壊れない）。
// Multicore は衝突系の Remove が未実装なので触らない（退避のみ）。
void PhysicsWorld::detachBody(std::size_t id) {
    if (p_->system.backend() == PhysicsBackend::Multicore || id >= p_->bodies.count()) return;
    auto& b = p_->bodies.body(id);
    if (!b->GetSystem()) return;  // 既に外れている
    p_->staticPairs.forget(*b);  // RemoveBody が衝突モデルを手放す前に
    p_->system.sys()->RemoveBody(b);
}

bool PhysicsWorld::bodyActive(std::size_t id) const {
    return p_->bodies.active(id);
}
