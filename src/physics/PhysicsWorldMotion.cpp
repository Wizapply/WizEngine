// ボディの運動の窓口: 中継と、ソフトボディの静止形状の分岐・固定の順番。

#include "physics/PhysicsParts.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>

using namespace chrono;
using namespace wizengine::physics::compat;

void PhysicsWorld::setBodyVelocity(std::size_t id, const ChVector3d& linear,
                                   const ChVector3d& angular) {
    p_->motion.setVelocity(id, linear, angular);
}

void PhysicsWorld::applyForce(std::size_t id, const ChVector3d& force, double dt) {
    p_->motion.applyForce(id, force, dt);
}

chrono::ChVector3d PhysicsWorld::bodyVelocity(std::size_t id) const {
    return p_->motion.velocity(id);
}

void PhysicsWorld::applyForceAtPoint(std::size_t id, const ChVector3d& force,
                                     const ChVector3d& point, double dt) {
    p_->motion.applyForceAtPoint(id, force, point, dt);
}

chrono::ChVector3d PhysicsWorld::bodyAngularVelocity(std::size_t id) const {
    return p_->motion.angularVelocity(id);
}

chrono::ChVector3d PhysicsWorld::bodyPointVelocity(
    std::size_t id, const chrono::ChVector3d& point) const {
    return p_->motion.pointVelocity(id, point);
}

double PhysicsWorld::bodyMass(std::size_t id) const { return p_->motion.mass(id); }

BodyTransform PhysicsWorld::bodyTransform(std::size_t id) const {
    // 番号は系の作り直し（recreate）で全部無効になる。古い番号で呼ばれても
    // 落ちないよう、範囲外は単位姿勢を返す（他の bodyXxx と同じ守り）。
    if (id >= p_->bodies.count()) return BodyTransform{0, 0, 0, 1, 0, 0, 0};
    if (const SoftBody* soft = softOfRoot(id)) return softTransform(*soft);
    return p_->motion.rigidTransform(id);
}

void PhysicsWorld::setBodyPose(std::size_t id, const ChVector3d& pos,
                               const ChQuaternion<>& rot) {
    if (SoftBody* soft = softOfRoot(id)) {
        // 静止形状のまま置き直す。粒子は眠らないので落下速度は要らない。
        placeSoftBody(*soft, pos, rot);
        return;
    }
    p_->motion.drop(id, pos, rot);
    forgetTerrainContacts(id);  // 置き直した = 前の場所での反力は使わない
}

void PhysicsWorld::placeBody(std::size_t id, const ChVector3d& pos,
                             const ChQuaternion<>& rot) {
    if (id >= p_->bodies.count()) return;
    if (SoftBody* soft = softOfRoot(id)) {
        placeSoftBody(*soft, pos, rot);
        return;
    }
    p_->motion.place(id, pos, rot);
    forgetTerrainContacts(id);  // 置き直した = 前の場所での反力は使わない
}

void PhysicsWorld::setBodyFixed(std::size_t id, bool fixed) {
    if (id >= p_->bodies.count()) return;
    // 固定するときは速度も捨てる（ForceToRest）。走行中に固定された物が
    // 速度を持ったままだと、Multicore の接触拘束がそれを「動く床」として
    // 上の物へ伝え続ける（applyForce の注記と同じ理屈）。
    if (SoftBody* soft = softOfRoot(id)) {
        // 全粒子を固定 = 形を保ったまま動かない土台になる。
        for (const std::size_t p : p_->bodies.members(soft->root)) {
            p_->bodies.body(p)->SetFixed(fixed);
            if (fixed) p_->bodies.body(p)->ForceToRest();
            else wakeUp(p_->bodies.body(p).get(), 0);
            if (p_->bodies.active(p)) updateStaticPairs(*p_->bodies.body(p));  // 固定物どうしのペア
        }
        return;
    }
    p_->bodies.body(id)->SetFixed(fixed);
    if (fixed) {
        p_->bodies.body(id)->ForceToRest();
    } else {
        wakeUp(p_->bodies.body(id).get(), 0);
        p_->sleeping.resetTimer(id);  // 固定を外した直後に眠らせない
    }
    // 固定物どうしのペア: 固定にしたら掃除、外したら作り直させる。
    if (p_->bodies.active(id)) updateStaticPairs(*p_->bodies.body(id));
}

bool PhysicsWorld::bodyFixed(std::size_t id) const {
    if (id >= p_->bodies.count()) return false;
    // ソフトボディは代表粒子で代表させる（setBodyFixed は全粒子を揃えて
    // 固定するので、代表を見れば足りる）。
    return p_->bodies.body(id)->IsFixed();
}
