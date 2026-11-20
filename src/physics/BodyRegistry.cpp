// ボディの名簿: 番号の登録・衝突系への登録・退場・別名・グループ。

#include "physics/BodyRegistry.h"
#include "physics/PhysicsSystem.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystem.h>

#include <utility>

using namespace chrono;

namespace wizengine {
namespace physics {

std::size_t BodyRegistry::add(const std::shared_ptr<ChBody>& body, const BodyAddInfo& info) {
    const std::size_t id = bodies_.size();
    bodyIndex_[body.get()] = id;
    bodies_.push_back(body);
    active_.push_back(true);
    groupOf_.push_back(kInvalidId);  // defineGroup が員のぶんを後から書き換える
    alias_.push_back(kInvalidId);
    bindCollision(body);
    events_.bodyAdded(id, info);
    return id;
}

void BodyRegistry::retire(std::size_t id) {
    events_.bodyRetiring(id);
    const auto& b = bodies_[id];
    b->SetFixed(true);
    // Multicore の衝突系は Remove() が未実装で、Chrono 9 は
    // "ChCollisionSystemMulticore::Remove() not yet implemented." を出して
    // 例外を投げる（物理スレッドで捕まらず、アプリごと落ちる）。そちらでは
    // 当たり判定のフラグを触らず、固定 + 退避だけで無効化する。退場した
    // ボディは全部 fixed なので、同じ場所に重なっても互いに接触しない。
    if (system_.backend() != PhysicsBackend::Multicore) {
        b->EnableCollision(false);
    }
    b->ForceToRest();
    // 地面のはるか下へ。当たり判定を切っても衝突系がまだ形状を持っている
    // 版があるので、位置でも確実に無関係にしておく。
    b->SetPos(ChVector3d(0, -1000.0, 0));
    active_[id] = false;
}

void BodyRegistry::clear() {
    bodies_.clear();
    bodyIndex_.clear();
    active_.clear();
    groupOf_.clear();
    alias_.clear();
    groups_.clear();
    // 名簿が空になってから配る（受け手は番号と並ぶ配列を捨てる）。
    events_.bodiesCleared();
}

void BodyRegistry::setAlias(std::size_t id, std::size_t owner) {
    if (id >= alias_.size() || owner >= bodies_.size() || id == owner) return;
    alias_[id] = owner;
}

void BodyRegistry::defineGroup(std::size_t root, std::vector<std::size_t> members) {
    const std::size_t index = groups_.size();
    for (const std::size_t m : members) groupOf_[m] = index;
    Group g;
    g.root = root;
    g.members = std::move(members);
    groups_.push_back(std::move(g));
}

// Chrono 9 は衝突モデルを「衝突系の初期化（最初の DoStepDynamics）で
// まとめて登録（BindAll）」する。それ以降に AddBody したボディは
// ChSystem::AddBody も ChSystemMulticore::AddBody も衝突系へ渡さないので、
// 自分で BindItem を呼ばないと**形はあるのに当たり判定が無い**ボディに
// なる（エディタで足した箱が床を抜ける・シーン読込で作り直した床に何も
// 乗らない、の正体。Core / Multicore どちらも同じ）。初期化前に足した
// ぶんは BindAll が拾うので触らない（Multicore の Add は二重登録を
// 想定していない）。
void BodyRegistry::bindCollision(const std::shared_ptr<ChBody>& body) {
    auto coll = system_.sys()->GetCollisionSystem();
    if (!coll || !coll->IsInitialized()) return;
    const auto model = body->GetCollisionModel();
    if (!model || model->HasImplementation() || !body->IsCollisionEnabled()) {
        return;
    }
    coll->BindItem(body);
}

}  // namespace physics
}  // namespace wizengine
