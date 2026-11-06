// 接触の材質: 共有の材質とボディ専用の材質。
// 材質は接触の種類（PhysicsSystem::contact）で NSC / SMC のどちらか。共通の欄（摩擦・反発）は基底に、
// 転がり・スピン・粘着は NSC に、ヤング率・ポアソン比・付着は SMC にある。

#include "physics/ContactMaterials.h"
#include "physics/BodyRegistry.h"
#include "physics/PhysicsSystem.h"

#include <chrono/physics/ChContactMaterialNSC.h>
#include <chrono/physics/ChContactMaterialSMC.h>

using namespace chrono;

namespace {

// 接触の物性の 4 欄のどれかに指定があるか（無ければシーン設定のまま）。
bool hasCustomContact(const BodyOptions& o) {
    return o.friction >= 0.0f || o.restitution >= 0.0f || o.rolling >= 0.0f || o.cohesion != 0.0f;
}

}  // namespace

namespace wizengine {
namespace physics {

void ContactMaterials::applySurface(ChContactMaterial& m, const BodyOptions& o) const {
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

std::shared_ptr<ChContactMaterial> ContactMaterials::make() const {
    if (system_.contact() == ContactMethod::SMC) {
        return chrono_types::make_shared<ChContactMaterialSMC>();
    }
    return chrono_types::make_shared<ChContactMaterialNSC>();
}

std::shared_ptr<ChContactMaterial> ContactMaterials::materialFor(const BodyOptions& o) {
    const bool custom = hasCustomContact(o) || o.young >= 0.0f || o.poisson >= 0.0f;
    if (!custom) return mat_;
    auto m = make();
    applySurface(*m, o);
    return m;
}

void ContactMaterials::reapplyAll() {
    applySurface(*mat_, BodyOptions{});
    // 専用材質のボディも「シーン任せ」の欄はこの値に追従する。
    for (std::size_t i = 0; i < mats_.size(); ++i) {
        if (mats_[i]) applySurface(*mats_[i], options_[i]);
    }
    bodies_.events().materialsChanged();
}

void ContactMaterials::setSmcDefaults(double young, double poisson) {
    smcYoung_ = young;
    smcPoisson_ = poisson;
    if (system_.contact() != ContactMethod::SMC) return;
    reapplyAll();
}

bool ContactMaterials::setBodySurface(std::size_t id, const BodyOptions& options) {
    if (id >= bodies_.count()) return false;
    // materialFor と違って young / poisson を見ない。今の動きを保つためで、見るように
    // するのは別の修正の候補。
    const bool custom = hasCustomContact(options);
    // options_ に写すのは下の 4 欄だけ。レイヤ・重力・young / poisson などは写さない
    // （レイヤは衝突モデルの作り直しが要るので呼び出し側が判断する）。
    BodyOptions& cur = options_[id];
    cur.friction = options.friction;
    cur.restitution = options.restitution;
    cur.rolling = options.rolling;
    cur.cohesion = options.cohesion;
    bool ok = true;
    for (const std::size_t p : bodies_.members(id)) {
        if (mats_[p]) {
            applySurface(*mats_[p], cur);
        } else if (custom) {
            ok = false;  // 共有材質のまま「シーン任せ」に戻すだけなら変えずに済む
        }
        options_[p].friction = cur.friction;
        options_[p].restitution = cur.restitution;
        options_[p].rolling = cur.rolling;
        options_[p].cohesion = cur.cohesion;
    }
    return ok;
}

void ContactMaterials::setSurfaceMaterial(float friction, float restitution) {
    friction_ = friction;
    restitution_ = restitution;
    reapplyAll();
}

void ContactMaterials::setRollingFriction(float rolling, float spinning) {
    // NSC materials expose these directly; they are ignored by solvers that do
    // not model rolling resistance, which is harmless. SMC の材質には無い
    // （applySurface が種類を見て入れる）。
    rolling_ = rolling;
    spinning_ = spinning;
    reapplyAll();
}

void ContactMaterials::onSystemRecreated() {
    mats_.clear();
    options_.clear();
    mat_ = make();
    // No bounce: even a little restitution keeps settled boxes micro-bouncing.
    applySurface(*mat_, BodyOptions{});
}

void ContactMaterials::onBodyAdded(std::size_t id, const BodyAddInfo& info) {
    (void)id;
    mats_.push_back(info.material == mat_ ? nullptr : info.material);
    options_.push_back(info.options);
}

}  // namespace physics
}  // namespace wizengine
