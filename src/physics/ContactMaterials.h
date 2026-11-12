#pragma once

// 接触の材質: シーンで共有の材質と、ボディ専用の材質。物理スレッド専用。

#include <cstddef>
#include <memory>
#include <vector>

#include "physics/PhysicsPart.h"
#include "physics/PhysicsTypes.h"

namespace chrono {
class ChContactMaterial;
}

namespace wizengine {
namespace physics {

class BodyRegistry;
class PhysicsSystem;

class ContactMaterials : public PhysicsPart {
public:
    ContactMaterials(PhysicsSystem& system, BodyRegistry& bodies)
        : system_(system), bodies_(bodies) {}
    ContactMaterials(const ContactMaterials&) = delete;
    ContactMaterials& operator=(const ContactMaterials&) = delete;

    // ボディの物性を決める材質（指定が無ければ共有の材質）。
    std::shared_ptr<chrono::ChContactMaterial> materialFor(const BodyOptions& o);
    // 接触の種類（NSC / SMC）に合う空の材質。
    std::shared_ptr<chrono::ChContactMaterial> make() const;
    // シーン設定と合成した値を材質へ書く（負 = シーン設定）。
    void applySurface(chrono::ChContactMaterial& m, const BodyOptions& o) const;
    // そのボディが使う材質（専用が無ければ共有の材質）。
    const std::shared_ptr<chrono::ChContactMaterial>& materialOf(std::size_t id) const {
        return mats_[id] ? mats_[id] : mat_;
    }

    void setSurfaceMaterial(float friction, float restitution);
    void setRollingFriction(float rolling, float spinning);
    // 材質を書き直したら true（NSC では覚えるだけで false）。
    bool setSmcDefaults(double young, double poisson);
    // 物性を実行中に書き換える（意味は PhysicsWorld::setBodySurface の宣言）。
    bool setBodySurface(std::size_t id, const BodyOptions& options);

    void onSystemRecreated() override;
    void onBodyAdded(std::size_t id, const BodyAddInfo& info) override;

private:
    // 共有の材質と専用の材質を今のシーン設定で書き直す。
    void reapplyAll();

    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    std::shared_ptr<chrono::ChContactMaterial> mat_;  // NSC か SMC（PhysicsSystem::contact）
    double smcYoung_ = 2.0e7;
    double smcPoisson_ = 0.3;
    // シーン設定の値（専用材質を持つボディの「シーン任せ」の欄を解くため）。
    float friction_ = 0.6f;
    float restitution_ = 0.0f;
    float rolling_ = 0.0f;
    float spinning_ = 0.0f;
    // 名簿の番号と並ぶ: 専用の材質（共有なら nullptr）とオプション。オプションで
    // 読むのは物性の欄だけ（他の欄はそれぞれの担当が onBodyAdded で受け取る）。
    std::vector<std::shared_ptr<chrono::ChContactMaterial>> mats_;
    std::vector<BodyOptions> options_;
};

}  // namespace physics
}  // namespace wizengine
