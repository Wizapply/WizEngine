#pragma once

// 接触の報告: 接触コンテナの走査と、番号の持ち主への寄せ。状態は持たない。
// 意味は PhysicsWorld::activeContacts / activeContactPairs の宣言。

#include <cstddef>
#include <utility>
#include <vector>

#include "physics/PhysicsTypes.h"

namespace wizengine {
namespace physics {

class BodyRegistry;
class PhysicsSystem;

std::vector<ContactInfo> activeContacts(const PhysicsSystem& system, const BodyRegistry& bodies);
std::vector<std::pair<std::size_t, std::size_t>> activeContactPairs(const PhysicsSystem& system,
                                                                    const BodyRegistry& bodies);

}  // namespace physics
}  // namespace wizengine
