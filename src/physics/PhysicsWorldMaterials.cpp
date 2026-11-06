// 接触の材質: 共有の材質とボディ専用の材質。

#include "physics/PhysicsParts.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactMaterialNSC.h>
#include <chrono/physics/ChContactMaterialSMC.h>

using namespace chrono;
using namespace wizengine::physics::compat;

// ---- 接触の物性 -------------------------------------------------------------
// 材質は接触の種類（PhysicsSystem::contact）で NSC / SMC のどちらか。共通の欄（摩擦・反発）は基底に、
// 転がり・スピン・粘着は NSC に、ヤング率・ポアソン比・付着は SMC にある。

void PhysicsWorld::applySurface(ChContactMaterial& m, const BodyOptions& o) const {
    m.SetFriction(o.friction >= 0.0f ? o.friction : friction_);
    m.SetRestitution(o.restitution >= 0.0f ? o.restitution : restitution_);
    if (auto* n = dynamic_cast<ChContactMaterialNSC*>(&m)) {
        n->SetRollingFriction(o.rolling >= 0.0f ? o.rolling : rolling_);
        n->SetSpinningFriction(o.rolling >= 0.0f ? o.rolling : spinning_);
        n->SetCohesion(o.cohesion);
    } else if (auto* s = dynamic_cast<ChContactMaterialSMC*>(&m)) {
        s->SetYoungModulus(float(o.young >= 0.0f ? o.young : smcYoung_));
        s->SetPoissonRatio(float(o.poisson >= 0.0f ? o.poisson : smcPoisson_));
        s->SetAdhesion(o.cohesion);  // 粘着 = SMC の付着力（同じ意味の欄）
    }
}

std::shared_ptr<ChContactMaterial> PhysicsWorld::makeMaterial() const {
    if (p_->system.contact() == ContactMethod::SMC) {
        return chrono_types::make_shared<ChContactMaterialSMC>();
    }
    return chrono_types::make_shared<ChContactMaterialNSC>();
}

std::shared_ptr<ChContactMaterial> PhysicsWorld::materialFor(const BodyOptions& o) {
    const bool custom = o.friction >= 0.0f || o.restitution >= 0.0f ||
                        o.rolling >= 0.0f || o.cohesion != 0.0f || o.young >= 0.0f ||
                        o.poisson >= 0.0f;
    if (!custom) return mat_;
    auto m = makeMaterial();
    applySurface(*m, o);
    return m;
}

void PhysicsWorld::setSmcDefaults(double young, double poisson) {
    smcYoung_ = young;
    smcPoisson_ = poisson;
    if (p_->system.contact() != ContactMethod::SMC) return;
    applySurface(*mat_, BodyOptions{});
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        if (mats_[i]) applySurface(*mats_[i], options_[i]);
    }
    refreshTerrainMaterials();
}

bool PhysicsWorld::setBodySurface(std::size_t id, const BodyOptions& options) {
    if (id >= p_->bodies.count()) return false;
    const bool custom = options.friction >= 0.0f || options.restitution >= 0.0f ||
                        options.rolling >= 0.0f || options.cohesion != 0.0f;
    // 物性以外（レイヤ・重力）は options_ に写すだけ。レイヤは衝突モデルの
    // 作り直しが要るので呼び出し側が判断する。
    BodyOptions& cur = options_[id];
    cur.friction = options.friction;
    cur.restitution = options.restitution;
    cur.rolling = options.rolling;
    cur.cohesion = options.cohesion;
    if (SoftBody* soft = softOfRoot(id)) {
        bool ok = true;
        for (const std::size_t p : p_->bodies.members(soft->root)) {
            if (mats_[p]) applySurface(*mats_[p], cur);
            else if (custom) ok = false;
            options_[p].friction = cur.friction;
            options_[p].restitution = cur.restitution;
            options_[p].rolling = cur.rolling;
            options_[p].cohesion = cur.cohesion;
        }
        return ok;
    }
    if (mats_[id]) {
        applySurface(*mats_[id], cur);
        return true;
    }
    // 共有材質のまま「シーン任せ」に戻すだけなら、何も変えずに済む。
    return !custom;
}

void PhysicsWorld::setSurfaceMaterial(float friction, float restitution) {
    friction_ = friction;
    restitution_ = restitution;
    applySurface(*mat_, BodyOptions{});
    // 専用材質のボディも「シーン任せ」の欄はこの値に追従する。
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        if (mats_[i]) applySurface(*mats_[i], options_[i]);
    }
    refreshTerrainMaterials();
}

void PhysicsWorld::setRollingFriction(float rolling, float spinning) {
    // NSC materials expose these directly; they are ignored by solvers that do
    // not model rolling resistance, which is harmless. SMC の材質には無い
    // （applySurface が種類を見て入れる）。
    rolling_ = rolling;
    spinning_ = spinning;
    applySurface(*mat_, BodyOptions{});
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        if (mats_[i]) applySurface(*mats_[i], options_[i]);
    }
    refreshTerrainMaterials();
}
