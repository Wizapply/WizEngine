#pragma once

// 1D のシャフト網（Chrono の ChShaft 系）: 軸・遊星・クラッチ・ばね・歯車・トルコン・
// 速度モータ・3D の剛体との直結を作り、毎ステップの荷重とクラッチの容量を受け、結果を
// 返す。物理スレッド専用。公開の意味は PhysicsWorld.h の同じ役目の宣言（addShaftNetwork ほか）。

#include <cstddef>
#include <memory>
#include <vector>

#include "physics/ShaftTypes.h"

namespace chrono {
class ChFunctionConst;
class ChPhysicsItem;
class ChShaft;
class ChShaftBodyRotation;
class ChShaftsClutch;
class ChShaftsGear;
class ChShaftsMotorSpeed;
class ChShaftsPlanetary;
class ChShaftsTorqueConverter;
class ChShaftsTorsionSpring;
}

namespace wizengine {
namespace physics {

class BodyRegistry;
class PhysicsSystem;

class ShaftNetworks {
public:
    ShaftNetworks(PhysicsSystem& system, BodyRegistry& bodies) : system_(system), bodies_(bodies) {}
    ShaftNetworks(const ShaftNetworks&) = delete;
    ShaftNetworks& operator=(const ShaftNetworks&) = delete;

    // 戻り値は網の番号（作れなければ kInvalidId: Multicore・番号の範囲外・剛体が無い）。
    std::size_t add(const ShaftNetworkSpec& spec);
    // 全部外す（シミュレートの停止）。
    void removeAll();
    std::size_t count() const { return nets_.size(); }

    // 毎ステップ（step の前）。並びは spec の shafts / clutches / motors。
    void setLoads(std::size_t net, const std::vector<double>& torque);
    void setClutchLimits(std::size_t net, const std::vector<double>& limit);
    void setMotorSpeeds(std::size_t net, const std::vector<double>& w);
    // 直前のステップの結果。
    void read(std::size_t net, ShaftNetworkState& out) const;

    // 系の作り直し: 一覧だけを捨てる（古い系ごと消えるため。古い系を壊す前に呼ぶ）。
    void clear() { nets_.clear(); }

private:
    struct Net {
        std::shared_ptr<chrono::ChShaft> housing;
        std::vector<std::shared_ptr<chrono::ChShaft>> shafts;
        std::vector<std::shared_ptr<chrono::ChShaftsPlanetary>> planetaries;
        std::vector<std::shared_ptr<chrono::ChShaftsClutch>> clutches;
        std::vector<std::shared_ptr<chrono::ChShaftsTorsionSpring>> springs;
        std::vector<std::shared_ptr<chrono::ChShaftsGear>> gears;
        std::vector<std::shared_ptr<chrono::ChShaftsTorqueConverter>> converters;
        std::vector<std::shared_ptr<chrono::ChShaftsMotorSpeed>> motors;
        std::vector<std::shared_ptr<chrono::ChFunctionConst>> motorSpeeds;
        std::vector<std::shared_ptr<chrono::ChShaftBodyRotation>> bodyLinks;
        // 系から外すための一覧（足した順）。
        std::vector<std::shared_ptr<chrono::ChPhysicsItem>> items;
    };

    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    std::vector<Net> nets_;
};

}  // namespace physics
}  // namespace wizengine
