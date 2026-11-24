// ボディの運動: 力・速度・質量・姿勢。

#include "physics/BodyMotion.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/Sleeping.h"

#include <chrono/physics/ChBody.h>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wizengine {
namespace physics {

void BodyMotion::setVelocity(std::size_t id, const ChVector3d& linear, const ChVector3d& angular) {
    if (id >= bodies_.count()) return;
    if (bodies_.isGroupRoot(id)) {
        // 粒子群には並進だけ（回転は粒子の配置が決める）。
        for (const std::size_t p : bodies_.members(id)) {
            if (bodies_.body(p)->IsFixed()) continue;
            setLinVel(bodies_.body(p).get(), linear, 0);
        }
        return;
    }
    auto& b = bodies_.body(id);
    if (b->IsFixed()) return;  // 固定ボディの速度は「動く床」になるので触らない
    wakeUp(b.get(), 0);
    setLinVel(b.get(), linear, 0);
    setAngVel(b.get(), angular, 0);
}

void BodyMotion::applyForce(std::size_t id, const ChVector3d& force, double dt) {
    if (id >= bodies_.count()) return;
    // 全体の質量で速度変化を出し、全員へ同じだけ足す（グループでも剛体に力を
    // 掛けたときと同じ並進になる）。
    const double total = mass(id);
    if (total <= 0.0) return;
    const ChVector3d dv = force * (dt / total);
    for (const std::size_t p : bodies_.members(id)) {
        auto& b = bodies_.body(p);
        // 固定ボディには掛けない（applyForceAtPoint と同じ）。固定の物は位置を
        // 積分しないので速度を足しても動かないが、**足した速度は残る**。Multicore
        // の接触拘束は固定ボディの速度も右辺に入れる（= 動く床として扱う）ので、
        // 固定の台を掴んで毎ステップ速度を積むと、台は動かないまま上の物が
        // 全部その速度で押し出されて発散する。Core は非アクティブな変数を
        // 無視するので出ないが、速度が溜まるのは同じ。
        if (b->IsFixed()) continue;

        // 眠ったボディは何かが触れるまで何も受け付けないので、先に起こす -
        // そうしないと、落ち着いた箱を押しても何も起きない。
        if (b->IsSleeping()) wakeUp(b.get(), 0);

        setLinVel(b.get(), getLinVel(b.get(), 0) + dv, 0);
    }
}

ChVector3d BodyMotion::velocity(std::size_t id) const {
    if (id >= bodies_.count()) return ChVector3d(0, 0, 0);
    if (bodies_.isGroupRoot(id)) {
        const auto members = bodies_.members(id);
        ChVector3d v(0, 0, 0);
        for (const std::size_t p : members) v += getLinVel(bodies_.body(p).get(), 0);
        return v * (1.0 / double(members.size()));
    }
    return getLinVel(bodies_.body(id).get(), 0);
}

void BodyMotion::applyForceAtPoint(std::size_t id, const ChVector3d& force,
                                   const ChVector3d& point, double dt) {
    if (id >= bodies_.count()) return;
    if (bodies_.isGroupRoot(id)) {
        // ソフトボディに「点」の力は無い（回転は粒子の配置が決める）。
        // 並進だけ全体へ。
        applyForce(id, force, dt);
        return;
    }
    auto& b = bodies_.body(id);
    const double m = b->GetMass();
    if (m <= 0.0 || b->IsFixed()) return;
    if (b->IsSleeping()) wakeUp(b.get(), 0);

    const ChVector3d impulse = force * dt;
    setLinVel(b.get(), getLinVel(b.get(), 0) + impulse / m, 0);

    // 角運動量の変化 L = r × J。
    const ChVector3d r = point - b->GetPos();
    addAngularImpulse(*b, r.Cross(impulse));
}

ChVector3d BodyMotion::angularVelocity(std::size_t id) const {
    if (id >= bodies_.count()) return ChVector3d(0, 0, 0);
    if (bodies_.isGroupRoot(id)) return ChVector3d(0, 0, 0);
    return getAngVel(bodies_.body(id).get(), 0);
}

ChVector3d BodyMotion::pointVelocity(std::size_t id, const ChVector3d& point) const {
    if (id >= bodies_.count()) return ChVector3d(0, 0, 0);
    if (bodies_.isGroupRoot(id)) return velocity(id);
    const auto& b = bodies_.body(id);
    const ChVector3d r = point - b->GetPos();
    return getLinVel(b.get(), 0) + getAngVel(b.get(), 0).Cross(r);
}

double BodyMotion::mass(std::size_t id) const {
    if (id >= bodies_.count()) return 0.0;
    double total = 0.0;
    for (const std::size_t p : bodies_.members(id)) total += bodies_.body(p)->GetMass();
    return total;
}

BodyTransform BodyMotion::rigidTransform(std::size_t id) const {
    const auto& b = bodies_.body(id);
    const ChVector3d p = b->GetPos();
    const ChQuaternion<> q = b->GetRot();
    return {p.x(), p.y(), p.z(), q.e0(), q.e1(), q.e2(), q.e3()};
}

void BodyMotion::place(std::size_t id, const ChVector3d& pos, const ChQuaternion<>& rot) {
    auto& b = bodies_.body(id);
    b->SetPos(pos);
    b->SetRot(rot);
    b->ForceToRest();
    wakeUp(b.get(), 0);
    sleeping_.resetTimer(id);  // 静止させた直後に眠らせない
    // 落下速度は与えない。エディタで置いた物は、
    // シミュレートを始めるまでその場に止まっていてほしい。
}

void BodyMotion::applyAngularImpulse(std::size_t id, const ChVector3d& impulse) {
    if (id >= bodies_.count() || bodies_.isGroupRoot(id)) return;
    auto& b = bodies_.body(id);
    if (b->IsFixed()) return;
    if (b->IsSleeping()) wakeUp(b.get(), 0);
    addAngularImpulse(*b, impulse);
}

// 角力積（ワールド）をワールド → ローカルへ回し、ローカルの逆慣性テンソルを
// 掛けてからワールドへ戻して角速度に足す（ChBody の慣性はローカル表現）。
void BodyMotion::addAngularImpulse(ChBody& b, const ChVector3d& impulseW) {
    const ChQuaternion<> q = b.GetRot();
    const ChVector3d dwL = b.GetInvInertia() * q.RotateBack(impulseW);
    setAngVel(&b, getAngVel(&b, 0) + q.Rotate(dwL), 0);
}

}  // namespace physics
}  // namespace wizengine
