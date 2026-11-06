#pragma once

// 物理の担当どうしの連絡の口。名簿（BodyRegistry）などが起きた出来事を伝え、
// 担当は関係するものだけを上書きする（既定は何もしない）。

#include <cstddef>
#include <memory>

#include "physics/PhysicsTypes.h"

namespace chrono {
class ChContactMaterial;
}

namespace wizengine {
namespace physics {

// ボディを名簿に足したときに渡す内容。
struct BodyAddInfo {
    const BodyOptions& options;
    // そのボディが使う材質。共有なら共有のもの。null = 材質なし（ゴースト）。
    std::shared_ptr<chrono::ChContactMaterial> material;
};

class PhysicsPart {
public:
    virtual ~PhysicsPart() = default;

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

}  // namespace physics
}  // namespace wizengine
