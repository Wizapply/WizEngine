// ボディの運動: 力・速度・質量・姿勢・固定。

#include "physics/PhysicsWorld.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>

#include <algorithm>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace {

// Minimum downward speed given to a re-dropped body (m/s). The actual value
// also scales with the configured sleep threshold, so the two cannot drift out
// of sync if the Scene tightens the limits.
constexpr double kWakeSpeed = 0.1;

}  // namespace

void PhysicsWorld::setBodyVelocity(std::size_t id, const ChVector3d& linear,
                                   const ChVector3d& angular) {
    if (id >= bodies_.size()) return;
    if (SoftBody* soft = softOfRoot(id)) {
        // 粒子群には並進だけ（回転は粒子の配置が決める）。
        for (const std::size_t p : soft->particles) {
            if (bodies_[p]->IsFixed()) continue;
            setLinVel(bodies_[p].get(), linear, 0);
        }
        return;
    }
    auto& b = bodies_[id];
    if (b->IsFixed()) return;  // 固定ボディの速度は「動く床」になるので触らない
    wakeUp(b.get(), 0);
    setLinVel(b.get(), linear, 0);
    setAngVel(b.get(), angular, 0);
}

void PhysicsWorld::applyForce(std::size_t id, const ChVector3d& force,
                              double dt) {
    if (id >= bodies_.size()) return;
    if (SoftBody* soft = softOfRoot(id)) {
        // 全体の質量で速度変化を出し、全粒子へ同じだけ足す（剛体に力を
        // 掛けたときと同じ並進になる）。
        const double total = bodyMass(id);
        if (total <= 0.0) return;
        const ChVector3d dv = force * (dt / total);
        for (const std::size_t p : soft->particles) {
            auto& pb = bodies_[p];
            if (pb->IsFixed()) continue;
            setLinVel(pb.get(), getLinVel(pb.get(), 0) + dv, 0);
        }
        return;
    }
    auto& b = bodies_[id];
    const double mass = b->GetMass();
    // 固定ボディには掛けない（applyForceAtPoint と同じ）。固定の物は位置を
    // 積分しないので速度を足しても動かないが、**足した速度は残る**。Multicore
    // の接触拘束は固定ボディの速度も右辺に入れる（= 動く床として扱う）ので、
    // 固定の台を掴んで毎ステップ速度を積むと、台は動かないまま上の物が
    // 全部その速度で押し出されて発散する。Core は非アクティブな変数を
    // 無視するので出ないが、速度が溜まるのは同じ。
    if (mass <= 0.0 || b->IsFixed()) return;

    // A sleeping body ignores everything until something touches it, so wake it
    // first - otherwise pushing a settled box does nothing at all.
    if (b->IsSleeping()) wakeUp(b.get(), 0);

    const ChVector3d v = getLinVel(b.get(), 0);
    setLinVel(b.get(), v + force * (dt / mass), 0);
}

chrono::ChVector3d PhysicsWorld::bodyVelocity(std::size_t id) const {
    if (id >= bodies_.size()) return chrono::ChVector3d(0, 0, 0);
    if (const SoftBody* soft = softOfRoot(id)) {
        ChVector3d v(0, 0, 0);
        for (const std::size_t p : soft->particles) v += getLinVel(bodies_[p].get(), 0);
        return v * (1.0 / double(soft->particles.size()));
    }
    return getLinVel(bodies_[id].get(), 0);
}

void PhysicsWorld::applyForceAtPoint(std::size_t id, const ChVector3d& force,
                                     const ChVector3d& point, double dt) {
    if (id >= bodies_.size()) return;
    if (isSoftBody(id)) {
        // ソフトボディに「点」の力は無い（回転は粒子の配置が決める）。
        // 並進だけ全体へ。
        applyForce(id, force, dt);
        return;
    }
    auto& b = bodies_[id];
    const double mass = b->GetMass();
    if (mass <= 0.0 || b->IsFixed()) return;
    if (b->IsSleeping()) wakeUp(b.get(), 0);

    const ChVector3d impulse = force * dt;
    setLinVel(b.get(), getLinVel(b.get(), 0) + impulse / mass, 0);

    // 角運動量の変化 L = r × J をワールド → ローカルへ回し、ローカルの逆慣性
    // テンソルを掛けてからワールドへ戻す（ChBody の慣性はローカル表現）。
    const ChVector3d r = point - b->GetPos();
    const ChVector3d angImpulseW = r.Cross(impulse);
    const ChQuaternion<> q = b->GetRot();
    const ChVector3d angImpulseL = q.RotateBack(angImpulseW);
    const ChVector3d dwL = b->GetInvInertia() * angImpulseL;
    const ChVector3d dwW = q.Rotate(dwL);
    setAngVel(b.get(), getAngVel(b.get(), 0) + dwW, 0);
}

chrono::ChVector3d PhysicsWorld::bodyAngularVelocity(std::size_t id) const {
    if (id >= bodies_.size()) return chrono::ChVector3d(0, 0, 0);
    if (isSoftBody(id)) return chrono::ChVector3d(0, 0, 0);
    return getAngVel(bodies_[id].get(), 0);
}

chrono::ChVector3d PhysicsWorld::bodyPointVelocity(
    std::size_t id, const chrono::ChVector3d& point) const {
    if (id >= bodies_.size()) return chrono::ChVector3d(0, 0, 0);
    if (isSoftBody(id)) return bodyVelocity(id);
    const auto& b = bodies_[id];
    const ChVector3d r = point - b->GetPos();
    return getLinVel(b.get(), 0) + getAngVel(b.get(), 0).Cross(r);
}

double PhysicsWorld::bodyMass(std::size_t id) const {
    if (id >= bodies_.size()) return 0.0;
    if (const SoftBody* soft = softOfRoot(id)) {
        double total = 0.0;
        for (const std::size_t p : soft->particles) total += bodies_[p]->GetMass();
        return total;
    }
    return bodies_[id]->GetMass();
}

BodyTransform PhysicsWorld::bodyTransform(std::size_t id) const {
    // 番号は系の作り直し（recreate）で全部無効になる。古い番号で呼ばれても
    // 落ちないよう、範囲外は単位姿勢を返す（他の bodyXxx と同じ守り）。
    if (id >= bodies_.size()) return BodyTransform{0, 0, 0, 1, 0, 0, 0};
    if (const SoftBody* soft = softOfRoot(id)) return softTransform(*soft);
    const auto& b = bodies_[id];
    const ChVector3d p = b->GetPos();
    const ChQuaternion<> q = b->GetRot();
    return {p.x(), p.y(), p.z(), q.e0(), q.e1(), q.e2(), q.e3()};
}

void PhysicsWorld::setBodyPose(std::size_t id, const ChVector3d& pos,
                               const ChQuaternion<>& rot) {
    if (SoftBody* soft = softOfRoot(id)) {
        // 静止形状のまま置き直す。粒子は眠らないので落下速度は要らない。
        placeSoftBody(*soft, pos, rot);
        return;
    }
    bodies_[id]->SetPos(pos);
    bodies_[id]->SetRot(rot);
    bodies_[id]->ForceToRest();  // zero linear + angular velocity and accel
    wakeUp(bodies_[id].get(), 0);  // a re-dropped body must not stay asleep
    forgetTerrainContacts(id);     // 置き直した = 前の場所での反力は使わない

    // ...and it must not fall straight back asleep. Chrono only refreshes a
    // body's "last moving" timestamp while it is above the sleep speed, so a
    // body we just stopped still looks like it has been still for ages and
    // would sleep again on the very next step - before gravity gets a chance
    // to speed it up. A small initial downward velocity resets that timer.
    const double wakeSpeed =
        std::max(kWakeSpeed, 3.0 * static_cast<double>(sleepMinLinVel_));
    setLinVel(bodies_[id].get(), ChVector3d(0, -wakeSpeed, 0), 0);
}

// ---- 置き直し・固定・角力積 -------------------------------------------------

void PhysicsWorld::placeBody(std::size_t id, const ChVector3d& pos,
                             const ChQuaternion<>& rot) {
    if (id >= bodies_.size()) return;
    if (SoftBody* soft = softOfRoot(id)) {
        placeSoftBody(*soft, pos, rot);
        return;
    }
    auto& b = bodies_[id];
    b->SetPos(pos);
    b->SetRot(rot);
    b->ForceToRest();
    wakeUp(b.get(), 0);
    resetSleepTimer(*b);  // 静止させた直後に眠らせない
    forgetTerrainContacts(id);  // 置き直した = 前の場所での反力は使わない
    // setBodyPose と違って落下速度は与えない。エディタで置いた物は、
    // シミュレートを始めるまでその場に止まっていてほしい。
}

void PhysicsWorld::setBodyFixed(std::size_t id, bool fixed) {
    if (id >= bodies_.size()) return;
    // 固定するときは速度も捨てる（ForceToRest）。走行中に固定された物が
    // 速度を持ったままだと、Multicore の接触拘束がそれを「動く床」として
    // 上の物へ伝え続ける（applyForce の注記と同じ理屈）。
    if (SoftBody* soft = softOfRoot(id)) {
        // 全粒子を固定 = 形を保ったまま動かない土台になる。
        for (const std::size_t p : soft->particles) {
            bodies_[p]->SetFixed(fixed);
            if (fixed) bodies_[p]->ForceToRest();
            else wakeUp(bodies_[p].get(), 0);
            if (active_[p]) updateStaticPairs(*bodies_[p]);  // 固定物どうしのペア
        }
        return;
    }
    bodies_[id]->SetFixed(fixed);
    if (fixed) {
        bodies_[id]->ForceToRest();
    } else {
        wakeUp(bodies_[id].get(), 0);
        resetSleepTimer(*bodies_[id]);  // 固定を外した直後に眠らせない
    }
    // 固定物どうしのペア: 固定にしたら掃除、外したら作り直させる。
    if (active_[id]) updateStaticPairs(*bodies_[id]);
}

bool PhysicsWorld::bodyFixed(std::size_t id) const {
    if (id >= bodies_.size()) return false;
    // ソフトボディは代表粒子で代表させる（setBodyFixed は全粒子を揃えて
    // 固定するので、代表を見れば足りる）。
    return bodies_[id]->IsFixed();
}

void PhysicsWorld::applyAngularImpulse(std::size_t id, const ChVector3d& impulse) {
    if (id >= bodies_.size() || isSoftBody(id)) return;
    auto& b = bodies_[id];
    if (b->IsFixed()) return;
    if (b->IsSleeping()) wakeUp(b.get(), 0);
    const ChQuaternion<> q = b->GetRot();
    const ChVector3d dwL = b->GetInvInertia() * q.RotateBack(impulse);
    setAngVel(b.get(), getAngVel(b.get(), 0) + q.Rotate(dwL), 0);
}
