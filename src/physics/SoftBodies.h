#pragma once

// ソフトボディ: 質点ばねの粒子群の一覧と、組み立て・ばねの解法・姿勢の当てはめ・
// 置き直し。粒子の列は名簿のグループ（代表 = 最初の粒子の physId）。物理スレッド専用。
// 公開の意味は PhysicsWorld.h の同じ役目の宣言（addSoftBody ほか）。

#include <cstddef>
#include <vector>

#include "physics/PhysicsPart.h"
#include "physics/PhysicsTypes.h"

namespace wizengine {
namespace physics {

class BodyFactory;
class BodyRegistry;
class ContactMaterials;

class SoftBodies : public PhysicsPart {
public:
    SoftBodies(BodyRegistry& bodies, BodyFactory& factory, ContactMaterials& materials)
        : bodies_(bodies), factory_(factory), materials_(materials) {}
    SoftBodies(const SoftBodies&) = delete;
    SoftBodies& operator=(const SoftBodies&) = delete;

    // 粒子を作って 1 つのソフトボディにする。戻り値 = 代表番号（rest が空なら kInvalidId）。
    std::size_t add(const SoftBodySpec& spec, const chrono::ChVector3d& pos,
                    const chrono::ChQuaternion<>& rot);
    // id がソフトボディの代表番号か。
    bool isRoot(std::size_t id) const;
    std::size_t particleCount(std::size_t id) const;
    // 粒子のワールド位置を 3 個ずつ out へ（代表番号でなければ空）。
    void particlePositions(std::size_t id, std::vector<float>& out) const;
    // 粒子群に当てはめた剛体姿勢（重心 + 最小二乗の回転）。代表番号で呼ぶこと
    // （それ以外は単位姿勢）。
    BodyTransform transform(std::size_t id) const;
    // 静止形状のまま pos / rot へ置き直す（全粒子を止める）。代表番号で呼ぶこと。
    void place(std::size_t id, const chrono::ChVector3d& pos, const chrono::ChQuaternion<>& rot);
    // 全部のソフトボディのばねを解いて粒子の速度を書き換える（step の先頭）。
    void solveSprings(double dt);

    // 系の作り直し: 一覧を捨てる（番号が全部無効になるため）。
    void clear();

    // 代表が退場したら、そのソフトボディのばねを解かない。
    void onBodyRetiring(std::size_t id) override;

private:
    struct SoftBody {
        std::size_t root = 0;                   // 代表 = 最初の粒子の physId（粒子の列は名簿のグループ）
        std::vector<chrono::ChVector3d> rest;   // 静止位置（ローカル）
        chrono::ChVector3d restCentroid;        // rest の重心
        std::vector<SoftBodySpec::Spring> springs;
        int iterations = 2;
        bool active = true;
    };

    // その番号が代表するソフトボディ（代表番号でなければ nullptr）。
    const SoftBody* find(std::size_t id) const;
    SoftBody* find(std::size_t id);
    // 1 つぶんのばねを解く。
    void solve(SoftBody& soft, double dt);

    BodyRegistry& bodies_;
    BodyFactory& factory_;
    ContactMaterials& materials_;
    std::vector<SoftBody> soft_;  // root の昇順（find が二分探索で引く）
};

}  // namespace physics
}  // namespace wizengine
