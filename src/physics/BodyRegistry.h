#pragma once

// ボディの名簿: 番号の登録・退場・逆引き・別名・グループ（代表番号）・出来事の通知。
// 番号は詰めない（退場しても残る）。物理スレッド専用。

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "physics/PhysicsPart.h"
#include "physics/PhysicsTypes.h"

namespace chrono {
class ChBody;
}

namespace wizengine {
namespace physics {

class PhysicsSystem;

// グループの番号の列。グループでないボディは自分 1 個の列になる。
class BodyRange {
public:
    explicit BodyRange(std::size_t single) : single_(single) {}
    explicit BodyRange(const std::vector<std::size_t>& list) : list_(&list) {}

    const std::size_t* begin() const { return list_ ? list_->data() : &single_; }
    const std::size_t* end() const { return begin() + size(); }
    std::size_t size() const { return list_ ? list_->size() : 1; }
    std::size_t operator[](std::size_t i) const { return begin()[i]; }

private:
    const std::vector<std::size_t>* list_ = nullptr;
    std::size_t single_ = 0;
};

class BodyRegistry {
public:
    explicit BodyRegistry(PhysicsSystem& system) : system_(system) {}
    BodyRegistry(const BodyRegistry&) = delete;
    BodyRegistry& operator=(const BodyRegistry&) = delete;

    // 系に AddBody 済みのボディを登録し、衝突系へ渡してから受け手へ通知する。
    std::size_t add(const std::shared_ptr<chrono::ChBody>& body, const BodyAddInfo& info);
    // 退場させる: 通知 → 固定 → 当たり判定を切る → 地面の下へ退避。番号は残る。
    void retire(std::size_t id);
    // 全部忘れる（系の作り直し）。受け手の一覧は残す。空にした後で受け手へ
    // onBodiesCleared を配る。
    void clear();
    // 出来事の受け手の一覧（ここ 1 つ）。窓口と担当が出す出来事もここから配る。
    PhysicsEvents& events() { return events_; }

    const std::shared_ptr<chrono::ChBody>& body(std::size_t id) const { return bodies_[id]; }
    const std::vector<std::shared_ptr<chrono::ChBody>>& all() const { return bodies_; }
    std::size_t count() const { return bodies_.size(); }
    bool active(std::size_t id) const { return id < active_.size() && active_[id]; }
    // ChBody* から番号への逆引き（知らなければ kInvalidId）。
    std::size_t indexOf(const chrono::ChBody* body) const {
        const auto it = bodyIndex_.find(body);
        return it == bodyIndex_.end() ? kInvalidId : it->second;
    }

    // id のボディを owner の子にする（接触の報告で owner の番号に寄せる）。
    void setAlias(std::size_t id, std::size_t owner);

    // members を 1 つのグループにする（root が代表番号）。
    void defineGroup(std::size_t root, std::vector<std::size_t> members);
    bool isGroupMember(std::size_t id) const { return groupIndex(id) != kInvalidId; }
    // id がグループの代表番号か。
    bool isGroupRoot(std::size_t id) const {
        const std::size_t g = groupIndex(id);
        return g != kInvalidId && groups_[g].root == id;
    }
    // グループの員ならその代表番号、それ以外はそのまま。
    std::size_t representative(std::size_t id) const {
        const std::size_t g = groupIndex(id);
        return g == kInvalidId ? id : groups_[g].root;
    }
    // 接触の報告に出す番号: 代表番号に寄せてから、別名があれば持ち主へ。
    std::size_t owner(std::size_t id) const {
        id = representative(id);
        return (id < alias_.size() && alias_[id] != kInvalidId) ? alias_[id] : id;
    }
    // 代表番号ならグループの全員（代表が先頭）、それ以外は id だけ。
    BodyRange members(std::size_t id) const {
        return isGroupRoot(id) ? BodyRange(groups_[groupIndex(id)].members) : BodyRange(id);
    }

private:
    struct Group {
        std::size_t root = 0;
        std::vector<std::size_t> members;
    };

    // id の属するグループの位置（無ければ kInvalidId）。名簿の外へ見せない。
    std::size_t groupIndex(std::size_t id) const {
        return id < groupOf_.size() ? groupOf_[id] : kInvalidId;
    }
    void bindCollision(const std::shared_ptr<chrono::ChBody>& body);

    PhysicsSystem& system_;
    std::vector<std::shared_ptr<chrono::ChBody>> bodies_;
    // 接触コールバックが返す ChBody* から番号への逆引き。ボディは名簿から
    // 消えない（退場しても残る）ので、追加時に足すだけ。
    std::unordered_map<const chrono::ChBody*, std::size_t> bodyIndex_;
    // retire で退場させたボディは false。
    std::vector<bool> active_;
    // bodies_ と並ぶ: 属するグループの位置（無ければ kInvalidId）。
    std::vector<std::size_t> groupOf_;
    // 子ボディ → 持ち主（setAlias）。kInvalidId = 無し。接触の報告にだけ効く。
    std::vector<std::size_t> alias_;
    std::vector<Group> groups_;
    PhysicsEvents events_;
};

}  // namespace physics
}  // namespace wizengine
