// Multicore 用の手計算のジョイント（モータ・ばね・可動範囲）。

#include "physics/PhysicsParts.h"
#include "physics/ChronoCompat.h"

#include <chrono/physics/ChBody.h>

#include <algorithm>
#include <cmath>

using namespace chrono;
using namespace wizengine::physics::compat;

// ---- Multicore 用の手計算（モータ・ばね）----------------------------------
// Chrono::Multicore は自前の積分で、リンクが IntLoadResidual_F で足す力
// （TSDA / RSDA / トルクモータ）を拾わない。同じ意味の力積を DoStepDynamics の
// 前に速度へ織り込む（車両の「点に掛かる力」と同じ流儀）。

void PhysicsWorld::applyManualJoints(double dt) {
    using Kind = JointRec::Manual::Kind;
    // 軸まわりの逆慣性（ワールド軸 w について w·(R I⁻¹ Rᵀ w)）。固定は 0。
    auto invInertiaAbout = [&](const std::shared_ptr<ChBody>& body, const ChVector3d& w) {
        if (body->IsFixed()) return 0.0;
        const ChVector3d wl = body->GetRot().RotateBack(w);
        return wl.Dot(body->GetInvInertia() * wl);
    };
    auto invMassOf = [&](const std::shared_ptr<ChBody>& body) {
        return (body->IsFixed() || body->GetMass() <= 0.0) ? 0.0 : 1.0 / body->GetMass();
    };
    for (JointRec& j : joints_) {
        JointRec::Manual& m = j.manual;
        if (j.broken || (m.kind == Kind::None && !m.limited)) continue;
        if (m.a >= p_->bodies.count() || m.b >= p_->bodies.count()) continue;
        auto& a = p_->bodies.body(m.a);
        auto& b = p_->bodies.body(m.b);
        const ChVector3d axisW = a->GetRot().Rotate(m.axisLocalA);

        // ---- 可動範囲（速度レベルの片側拘束。接触の解き方と同じ）----
        if (m.limited && dt > 0.0) {
            constexpr double kBeta = 0.2;  // めり込みの押し戻し（1 ステップで 20%）
            if (m.limitLinear) {
                const double d = (b->GetPos() - a->GetPos()).Dot(axisW) - m.limitRef;
                const double vrel = (getLinVel(b.get(), 0) - getLinVel(a.get(), 0)).Dot(axisW);
                double want = vrel;
                if (d > m.limitHi) want = std::min(vrel, -kBeta * (d - m.limitHi) / dt);
                else if (d < m.limitLo) want = std::max(vrel, kBeta * (m.limitLo - d) / dt);
                const double denom = invMassOf(a) + invMassOf(b);
                if (want != vrel && denom > 0.0) {
                    const double impulse = (want - vrel) / denom;  // B に +、A に -
                    applyForce(m.b, axisW * (impulse / dt), dt);
                    applyForce(m.a, axisW * (-impulse / dt), dt);
                    m.lastForce = std::max(m.lastForce, std::fabs(impulse / dt));
                }
            } else {
                const ChQuaternion<> rel = a->GetRot().GetConjugate() * b->GetRot();
                const ChQuaternion<> delta = m.relRot0.GetConjugate() * rel;
                const double proj = delta.e1() * m.axisLocalA.x() + delta.e2() * m.axisLocalA.y() +
                                    delta.e3() * m.axisLocalA.z();
                const double angle = 2.0 * std::atan2(proj, delta.e0());
                const double wrel = (getAngVel(b.get(), 0) - getAngVel(a.get(), 0)).Dot(axisW);
                double want = wrel;
                if (angle > m.limitHi) want = std::min(wrel, -kBeta * (angle - m.limitHi) / dt);
                else if (angle < m.limitLo) want = std::max(wrel, kBeta * (m.limitLo - angle) / dt);
                const double denom = invInertiaAbout(a, axisW) + invInertiaAbout(b, axisW);
                if (want != wrel && denom > 0.0) {
                    const double impulse = (want - wrel) / denom;  // 角力積。B に +、A に -
                    applyAngularImpulse(m.b, axisW * impulse);
                    applyAngularImpulse(m.a, axisW * (-impulse));
                    m.lastTorque = std::max(m.lastTorque, std::fabs(impulse / dt));
                }
            }
        }

        switch (m.kind) {
            case Kind::Torque: {
                // A に +τ、B に -τ（Chrono のモータと同じく body1 を回す向き）。
                const ChVector3d L = axisW * (m.target * dt);
                applyAngularImpulse(m.a, L);
                applyAngularImpulse(m.b, -L);
                m.lastTorque = std::fabs(m.target);
                break;
            }
            case Kind::Force: {
                const ChVector3d F = axisW * m.target;
                applyForce(m.a, F, dt);
                applyForce(m.b, -F, dt);
                m.lastForce = std::fabs(m.target);
                break;
            }
            case Kind::Spring: {
                const ChVector3d p1 = a->GetPos() + a->GetRot().Rotate(m.p1Local);
                const ChVector3d p2 = b->GetPos() + b->GetRot().Rotate(m.p2Local);
                const ChVector3d d = p2 - p1;
                const double len = d.Length();
                if (len < 1e-9) break;
                const ChVector3d dir = d * (1.0 / len);
                const double vrel = (bodyPointVelocity(m.b, p2) - bodyPointVelocity(m.a, p1)).Dot(dir);
                // 正 = 縮める向き（伸びているか、離れつつあるとき）。
                const double f = m.k * (len - m.rest) + m.c * vrel;
                applyForceAtPoint(m.a, dir * f, p1, dt);
                applyForceAtPoint(m.b, dir * (-f), p2, dt);
                m.lastForce = std::fabs(f);
                break;
            }
            case Kind::Rsda: {
                // B の A に対する回転を軸まわりの角度に落とす（開始姿勢が自然角）。
                const ChQuaternion<> rel = a->GetRot().GetConjugate() * b->GetRot();
                const ChQuaternion<> delta = m.relRot0.GetConjugate() * rel;
                const double proj = delta.e1() * m.axisLocalA.x() + delta.e2() * m.axisLocalA.y() +
                                    delta.e3() * m.axisLocalA.z();
                const double angle = 2.0 * std::atan2(proj, delta.e0());
                const double wrel = (getAngVel(b.get(), 0) - getAngVel(a.get(), 0)).Dot(axisW);
                const double tau = m.k * angle + m.c * wrel;  // B を戻す向きの大きさ
                const ChVector3d L = axisW * (tau * dt);
                applyAngularImpulse(m.b, -L);
                applyAngularImpulse(m.a, L);
                m.lastTorque = std::fabs(tau);
                break;
            }
            case Kind::None:
                break;
        }
    }
}
