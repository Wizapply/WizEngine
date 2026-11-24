#pragma once

// モーダル解析: 系の非減衰固有振動数を求める。状態は持たない（行列を取り出すために
// 系の Setup / Update を呼ぶ）。
// 意味は PhysicsWorld::modalFrequencies の宣言。

#include <string>
#include <vector>

namespace wizengine {
namespace physics {

class PhysicsSystem;

bool modalFrequencies(PhysicsSystem& system, int count, std::vector<double>& hz,
                      std::string& why);

}  // namespace physics
}  // namespace wizengine
