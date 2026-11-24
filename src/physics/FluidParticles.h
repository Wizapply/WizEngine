#pragma once

// 流体粒子（Chrono::Multicore の ChFluidContainer）の生成と、粒子の位置の問い合わせ。
// 物理スレッド専用。公開の意味は PhysicsWorld.h の同じ役目の宣言（addFluid ほか）。
//
// 外す口は持たない: Multicore のデータマネージャは 3 自由度の粒子の配列を
// 「足す」口しか持たず（ChFluidContainer::AddBodies）、途中で空にすると
// 並べ替え・接触・ソルバの配列の大きさとずれる恐れがある。作り直したいとき
// （次のシミュレート開始）は系ごと作り直す（Scene::rebuildPhysicsWorld）。
// 系の作り直しでは clear() で手元の参照だけを捨てる（コンテナは古い系と一緒に消える）。

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "physics/PhysicsTypes.h"

namespace chrono {
class ChFluidContainer;
}

namespace wizengine {
namespace physics {

class PhysicsSystem;

class FluidParticles {
public:
    explicit FluidParticles(PhysicsSystem& system) : system_(system) {}
    FluidParticles(const FluidParticles&) = delete;
    FluidParticles& operator=(const FluidParticles&) = delete;

    // 系に流体を足す。作れなければ false と理由（英語）: Multicore のビルドでない・
    // 系が Core / SMC・既に足してある（系を作り直してから）・粒子が 0 個。
    bool add(const FluidSpec& spec, std::string& why);
    bool active() const { return count_ > 0; }
    std::size_t count() const { return count_; }
    // 粒子のワールド位置を 3 個ずつ out へ詰める（描画スレッドへのスナップショット用）。
    void positions(std::vector<float>& out) const;
    // 粒子の速さの最大値（m/s。診断と /scene の表示用）。
    double maxSpeed() const;

    // 粒子を足したときの位置・速度へその場で書き戻す（Reset。数は変わらないので
    // データマネージャの配列はそのまま = 外す・足すのではない）。無ければ false。
    bool resetToInitial();

    // 系の作り直し: 参照だけを捨てる（古い系を壊す前に呼ぶ）。
    void clear();

private:
    PhysicsSystem& system_;
    std::shared_ptr<chrono::ChFluidContainer> container_;
    std::size_t count_ = 0;
    // 足したときの位置・速度（resetToInitial が書き戻す。3 個で 1 粒子）。
    std::vector<float> initialPos_, initialVel_;
};

}  // namespace physics
}  // namespace wizengine
