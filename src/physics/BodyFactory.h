#pragma once

// ボディの組み立て: 形ごとの追加・追加の当たり形状・形の記録の作成。状態を持たない。
// 物理スレッド専用。引数の意味は PhysicsWorld.h の同じ名前の宣言。

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "physics/PhysicsPart.h"
#include "physics/PhysicsTypes.h"

namespace chrono {
class ChBody;
}

namespace wizengine {
namespace physics {

class BodyRegistry;
class ContactMaterials;
class PhysicsSystem;
class Sleeping;

class BodyFactory {
public:
    BodyFactory(PhysicsSystem& system, BodyRegistry& bodies, Sleeping& sleeping,
                ContactMaterials& materials)
        : system_(system), bodies_(bodies), sleeping_(sleeping), materials_(materials) {}
    BodyFactory(const BodyFactory&) = delete;
    BodyFactory& operator=(const BodyFactory&) = delete;

    std::size_t addSphere(double radius, double density, const chrono::ChVector3d& pos,
                          const chrono::ChQuaternion<>& rot, bool fixed,
                          const std::vector<ExtraShape>& extras, const BodyOptions& options);
    std::size_t addBox(double sx, double sy, double sz, double density,
                       const chrono::ChVector3d& pos, const chrono::ChQuaternion<>& rot,
                       bool fixed, const std::vector<ExtraShape>& extras,
                       const BodyOptions& options);
    std::size_t addTriangleMesh(const std::vector<chrono::ChVector3d>& vertices,
                                const std::vector<std::array<int, 3>>& triangles, double mass,
                                const chrono::ChVector3d& pos, const chrono::ChQuaternion<>& rot,
                                bool fixed, const std::vector<ExtraShape>& extras,
                                const BodyOptions& options);
    std::size_t addFrame(double mass, const chrono::ChVector3d& pos,
                         const chrono::ChQuaternion<>& rot, bool fixed,
                         const std::vector<ExtraShape>& extras, const BodyOptions& options,
                         bool terrainContact);
    std::size_t addConvexHull(const std::vector<chrono::ChVector3d>& points, double mass,
                              const chrono::ChVector3d& pos, const chrono::ChQuaternion<>& rot,
                              const std::vector<ExtraShape>& extras, chrono::ChVector3d* hullCenter,
                              const BodyOptions& options);

    // 形まで組んだボディを仕上げる: 眠りの設定 → 衝突レイヤと重力オフ → 系へ追加 →
    // 名簿へ登録（受け手へ info が届く）。戻り値は physId。
    std::size_t finish(const std::shared_ptr<chrono::ChBody>& body, const BodyAddInfo& info);

private:
    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    Sleeping& sleeping_;
    ContactMaterials& materials_;
};

}  // namespace physics
}  // namespace wizengine
