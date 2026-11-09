#pragma once

// ボディの定常荷重（<geom force torque>）: 荷重コンテナと、ボディごとの荷重の一覧。
// 物理スレッド専用。

#include <cstddef>
#include <memory>
#include <vector>

#include "physics/PhysicsPart.h"

namespace chrono {
class ChLoadBase;
class ChLoadContainer;
}

namespace wizengine {
namespace physics {

class BodyRegistry;
class PhysicsSystem;

class BodyLoads : public PhysicsPart {
public:
    BodyLoads(PhysicsSystem& system, BodyRegistry& bodies) : system_(system), bodies_(bodies) {}
    BodyLoads(const BodyLoads&) = delete;
    BodyLoads& operator=(const BodyLoads&) = delete;

    // ボディ用の荷重コンテナを作って系へ足す（積分に取り込む系でだけ）。呼ぶ時期は
    // PhysicsWorld::Parts::createSystem が決める: 残差を足す順が変わらないよう、ジョイント
    // 用のコンテナの後に系へ足す。通知（onSystemRecreated）はその前に出るので使わない。
    void createContainer();
    // 荷重コンテナを手放す（系の作り直し。古い系を壊す前に呼ぶ）。ボディごとの
    // 荷重の一覧は onBodiesCleared が捨てる。
    void clear();

    void onBodiesCleared() override;
    void onBodyAdded(std::size_t id, const BodyAddInfo& info) override;
    void onBodyRetiring(std::size_t id) override;

private:
    void attach(std::size_t id, const BodyOptions& options);

    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    std::shared_ptr<chrono::ChLoadContainer> bodyLoads_;
    // 名簿の番号と並ぶ: そのボディの定常荷重（無ければ空）。
    std::vector<std::vector<std::shared_ptr<chrono::ChLoadBase>>> loadsOf_;
};

}  // namespace physics
}  // namespace wizengine
