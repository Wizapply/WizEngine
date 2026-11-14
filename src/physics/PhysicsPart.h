#pragma once

// 物理の担当どうしの連絡の口。名簿（BodyRegistry）などが起きた出来事を伝え、
// 担当は関係するものだけを上書きする（既定は何もしない）。

#include <cstddef>
#include <memory>
#include <vector>

#include "physics/PhysicsTypes.h"

namespace chrono {
class ChContactMaterial;
}

namespace wizengine {
namespace terrain {
struct BodyProxy;
}
namespace physics {

// ボディを名簿に足したときに渡す内容。
struct BodyAddInfo {
    const BodyOptions& options;
    // そのボディが使う材質。共有なら共有のもの。null = 材質なし（ゴースト）。
    std::shared_ptr<chrono::ChContactMaterial> material;
    bool allowSleeping;  // false = 眠らない（ソフトボディの粒子）
    // テレインの接触に使う形の記録（通知の間だけ有効）。null = 記録なし。
    const wizengine::terrain::BodyProxy* proxy = nullptr;
};

class PhysicsPart {
public:
    virtual ~PhysicsPart() = default;

    // 新しい系に基本設定（重力・反復回数・スレッド数）を当てた後、荷重コンテナと
    // ソルバの設定の前（初回の構築でも呼ばれる）。名簿は空。
    virtual void onSystemRecreated() {}
    virtual void onBodyAdded(std::size_t id, const BodyAddInfo& info) {
        (void)id;
        (void)info;
    }
    // 退場の直前（衝突モデルを手放す前）。
    virtual void onBodyRetiring(std::size_t id) { (void)id; }
    virtual void onBodyFixedChanged(std::size_t id) { (void)id; }
    virtual void onBodyPlaced(std::size_t id) { (void)id; }
    virtual void onWakeAll() {}
    virtual void onMaterialsChanged() {}
};

// 出来事の受け手の一覧と、配る処理。受け手を持たない（壊さない）。
class PhysicsEvents {
public:
    void add(PhysicsPart* part) { parts_.push_back(part); }

    void systemRecreated() {
        for (PhysicsPart* part : parts_) part->onSystemRecreated();
    }
    void bodyAdded(std::size_t id, const BodyAddInfo& info) {
        for (PhysicsPart* part : parts_) part->onBodyAdded(id, info);
    }
    void bodyRetiring(std::size_t id) {
        for (PhysicsPart* part : parts_) part->onBodyRetiring(id);
    }
    void bodyFixedChanged(std::size_t id) {
        for (PhysicsPart* part : parts_) part->onBodyFixedChanged(id);
    }
    void bodyPlaced(std::size_t id) {
        for (PhysicsPart* part : parts_) part->onBodyPlaced(id);
    }
    void wakeAll() {
        for (PhysicsPart* part : parts_) part->onWakeAll();
    }
    void materialsChanged() {
        for (PhysicsPart* part : parts_) part->onMaterialsChanged();
    }

private:
    std::vector<PhysicsPart*> parts_;
};

}  // namespace physics
}  // namespace wizengine
