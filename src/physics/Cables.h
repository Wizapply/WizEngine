#pragma once

// FEA ケーブル: ANCF 梁の生成・端の留め方・接触面と、節点の位置・張力の問い合わせ。
// 物理スレッド専用。公開の意味は PhysicsWorld.h の同じ役目の宣言（addCable ほか）。

#include <cstddef>
#include <memory>
#include <vector>

#include "physics/PhysicsTypes.h"

namespace chrono {
class ChLinkBase;
namespace fea {
class ChMesh;
class ChNodeFEAxyzD;
}
}

namespace wizengine {
namespace physics {

class BodyRegistry;
class ContactMaterials;
class PhysicsSystem;

class Cables {
public:
    Cables(PhysicsSystem& system, BodyRegistry& bodies, ContactMaterials& materials)
        : system_(system), bodies_(bodies), materials_(materials) {}
    Cables(const Cables&) = delete;
    Cables& operator=(const Cables&) = delete;

    // 戻り値はケーブル番号（作れなければ kInvalidId）。
    std::size_t add(const CableSpec& spec);
    // 全部外す（シミュレートの停止）。
    void removeAll();
    std::size_t count() const { return cables_.size(); }
    void nodePositions(std::size_t cable, std::vector<float>& out) const;
    double tension(std::size_t cable) const;

    // 系の作り直し: 一覧だけを捨てる（リンクもメッシュも系から外さない = 古い系ごと
    // 消えるため。古い系を壊す前に呼ぶ）。
    void clear() { cables_.clear(); }

private:
    // ケーブル（FEA メッシュ + 端の拘束）。removeAll でまとめて外す。
    struct CableRec {
        std::shared_ptr<chrono::fea::ChMesh> mesh;
        std::vector<std::shared_ptr<chrono::fea::ChNodeFEAxyzD>> nodes;
        std::vector<std::shared_ptr<chrono::ChLinkBase>> links;  // 端の拘束
        bool valid = false;
    };

    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    ContactMaterials& materials_;
    std::vector<CableRec> cables_;
};

}  // namespace physics
}  // namespace wizengine
