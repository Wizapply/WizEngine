#pragma once

// 固定物どうしの衝突ペアを作らせない（Core = Bullet のみ）。理由と仕組みは
// StaticPairs.cpp の先頭の注記。Bullet の型は .cpp に閉じる。物理スレッド専用。

#include <cstddef>
#include <memory>

#include "physics/PhysicsPart.h"

namespace chrono {
class ChBody;
}

namespace wizengine {
namespace physics {

class BodyRegistry;
class PhysicsSystem;
struct StaticPairFilter;  // 定義は StaticPairs.cpp

// Bullet のペアキャッシュがフィルタを指すので、系より後に壊すこと（PhysicsParts.h）。
class StaticPairs : public PhysicsPart {
public:
    StaticPairs(PhysicsSystem& system, BodyRegistry& bodies) : system_(system), bodies_(bodies) {}
    StaticPairs(const StaticPairs&) = delete;
    StaticPairs& operator=(const StaticPairs&) = delete;

    // 新しい系の Bullet ワールドにフィルタを付ける（Multicore では外したまま）。
    // 呼ぶ時期は PhysicsWorld::Parts::createSystem が決める（系の基本設定より前）。
    void attach();
    // 衝突系が初期化済みで固定物の集合がまだなら作る（積分の前後で呼ぶ）。
    void syncIfPending();

    void onBodyAdded(std::size_t id, const BodyAddInfo& info) override;
    void onBodyRetiring(std::size_t id) override;
    void onBodyFixedChanged(std::size_t id) override;

private:
    // 固定ボディの集合を全部作り直し、既にできた固定どうしのペアを消す
    // （衝突系の初期化 = 最初のステップの直後に 1 回）。
    void sync();
    // 1 個のボディの固定状態が変わった / 登録されたあとに呼ぶ。固定になったら
    // 集合に足してペアを掃除、固定でなくなったら集合から外して衝突モデルを
    // 入れ直す（消したペアを Bullet に作り直させる）。
    void update(chrono::ChBody& body);
    // 衝突モデルを手放す前（EnableCollision(false) / RemoveBody）に呼ぶ:
    // 解放された Bullet 側の実体のアドレスを集合に残さない（退場の通知から）。
    void forget(chrono::ChBody& body);

    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    std::shared_ptr<StaticPairFilter> filter_;
};

}  // namespace physics
}  // namespace wizengine
