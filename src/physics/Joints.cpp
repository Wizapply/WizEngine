// ジョイント: 拘束・モータ・ばねの生成、反力の計測、破断。

#include "physics/Joints.h"
#include "core/Log.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/ChronoMath.h"
#include "physics/PhysicsSystem.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChLinkBase.h>
#include <chrono/physics/ChLinkDistance.h>
#include <chrono/physics/ChLinkLock.h>
#include <chrono/physics/ChSystem.h>
// ジョイントの拡張（A の導入）: モータ・自在継手・歯車・ねじ・ばね。
#include <chrono/physics/ChLinkMotorRotationSpeed.h>
#include <chrono/physics/ChLinkMotorRotationAngle.h>
#include <chrono/physics/ChLinkMotorRotationTorque.h>
#include <chrono/physics/ChLinkMotorLinearSpeed.h>
#include <chrono/physics/ChLinkMotorLinearPosition.h>
#include <chrono/physics/ChLinkMotorLinearForce.h>
#include <chrono/physics/ChLinkUniversal.h>
#include <chrono/physics/ChLinkTSDA.h>
#include <chrono/physics/ChLinkRSDA.h>
#ifdef WIZ_HAVE_GEAR
#include <chrono/physics/ChLinkLockGear.h>
#endif
#ifdef WIZ_HAVE_SCREW
#include <chrono/physics/ChLinkLockScrew.h>
#endif
#ifdef WIZ_HAVE_LOADS
#include <chrono/physics/ChLoadContainer.h>
#include <chrono/physics/ChLoadsBody.h>
#endif
#ifdef WIZ_HAVE_FUNCTIONS_DIR
#include <chrono/functions/ChFunctionConst.h>
#include <chrono/functions/ChFunctionRamp.h>
#else
#include <chrono/motion_functions/ChFunction_Const.h>
#include <chrono/motion_functions/ChFunction_Ramp.h>
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wizengine {
namespace physics {

namespace {

// モータの目標値の関数（定数とランプ）。Chrono 9 は ChFunctionConst、旧版は ChFunction_Const。
#ifdef WIZ_HAVE_FUNCTIONS_DIR
using WizConstFunction = chrono::ChFunctionConst;
using WizRampFunction = chrono::ChFunctionRamp;
#else
using WizConstFunction = chrono::ChFunction_Const;
using WizRampFunction = chrono::ChFunction_Ramp;
#endif

// ワールド座標の座標系をボディのローカルへ（歯車のシャフト座標系用）。
// Chrono の TransformParentToLocal は版で引数が違うので四元数で手計算。
chrono::ChFrame<> worldToBodyLocal(const chrono::ChBody& body, const chrono::ChVector3d& pos,
                                   const chrono::ChQuaternion<>& rot) {
    const chrono::ChQuaternion<> q = body.GetRot();
    const chrono::ChVector3d lp = q.RotateBack(pos - body.GetPos());
    const chrono::ChQuaternion<> lr = q.GetConjugate() * rot;
    return chrono::ChFrame<>(lp, lr);
}

// +X をこの向きに合わせる回転。ChLinkMotorLinear* は X 軸方向に動くので、
// 直動モータだけはこちら（ChLinkLockPrismatic は Z）。
chrono::ChQuaternion<> quatFromXAxis(const chrono::ChVector3d& axis) {
    double ax = axis.x(), ay = axis.y(), az = axis.z();
    const double n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n < 1e-9) return chrono::ChQuaternion<>(1, 0, 0, 0);
    ax /= n;
    ay /= n;
    az /= n;
    const double c = ax;  // dot((1,0,0), axis)
    if (c < -0.999999) {  // 真後ろ: Y 軸まわりに 180 度
        return chrono::ChQuaternion<>(0, 0, 1, 0);
    }
    // 最短回転。cross((1,0,0), axis) = (0, -az, ay)
    chrono::ChQuaternion<> q(1.0 + c, 0.0, -az, ay);
    q.Normalize();
    return q;
}

// 零ベクトルでない軸か。
bool axisUsable(const chrono::ChVector3d& a) {
    return a.x() * a.x() + a.y() * a.y() + a.z() * a.z() > 1e-12;
}

}  // namespace

std::size_t Joints::add(const JointSpec& spec) {
    const std::size_t bodyA = spec.bodyA;
    const std::size_t bodyB = spec.bodyB;
    if (bodyA >= bodies_.count() || bodyB >= bodies_.count()) {
        LOGW("physics", "joint: body id out of range (%zu, %zu)", bodyA, bodyB);
        return kInvalidJoint;
    }
    if (bodyA == bodyB) {
        LOGW("physics", "joint: both ends are the same body (%zu)", bodyA);
        return kInvalidJoint;
    }
    if (!bodies_.active(bodyA) || !bodies_.active(bodyB)) {
        LOGW("physics", "joint: body has been removed (%zu, %zu)", bodyA, bodyB);
        return kInvalidJoint;
    }

    auto a = bodies_.body(bodyA);
    auto b = bodies_.body(bodyB);
    // 眠ったままだと拘束が効かないので、両端とも起こしておく。
    wakeUp(a.get(), 0);
    wakeUp(b.get(), 0);

    const ChVector3d anchor = spec.anchor;
    const ChVector3d axis = axisUsable(spec.axis) ? spec.axis : ChVector3d(0, 1, 0);
    // リンク座標系。Revolute は Z 軸まわりに回り、Prismatic は Z 軸方向へ
    // スライドするので、指定されたワールド軸を Z に合わせる。
    const ChQuaternion<> frameRot = quatFromZAxis(axis);

    // 2 点間のばね（Spring 種類、または Prismatic に添えるばね）。地面側は
    // アンカー、それ以外はボディの中心を取り付け点にする。
    auto makeSpring = [&](double restLength) -> std::shared_ptr<chrono::ChLinkBase> {
        const ChVector3d p1 = a->IsFixed() ? anchor : a->GetPos();
        const ChVector3d p2 = b->IsFixed() ? anchor : b->GetPos();
        if (a->IsFixed() && b->IsFixed()) return nullptr;
        auto s = chrono_types::make_shared<ChLinkTSDA>();
        s->Initialize(a, b, /*local*/ false, p1, p2);
        double len = restLength;
        if (len <= 0.0) len = (p2 - p1).Length();
        s->SetRestLength(len);
        s->SetSpringCoefficient(spec.stiffness);
        s->SetDampingCoefficient(spec.damping);
        return s;
    };

    JointRec rec;
    rec.type = spec.type;
    rec.breakForce = spec.breakForce;
    std::shared_ptr<chrono::ChLinkBase> link;
    bool wantLimit = spec.limited;
    bool limitDone = false;
    // Multicore は ChLinkTSDA / ChLinkRSDA / トルク（力）モータの力を積分に
    // 取り込まず、速度モータは専用の一覧（RemoveLink で外れない）を持つ。
    // そちらでは手計算の力積（applyManual）と「角度モータ + ランプ
    // 関数」で同じ意味を作る。Core は Chrono のリンクをそのまま使う。
    const bool manualMode = system_.backend() == PhysicsBackend::Multicore;
    // Multicore は片側拘束（ChLinkLimit）を解けない - 範囲に当たった瞬間に
    // 系全体が NaN になった（実機で確認）。そちらでは可動範囲も手計算の
    // 力積にする（applyManual）。
    auto manualLimit = [&](bool linear) {
        rec.manual.limited = true;
        rec.manual.limitLinear = linear;
        rec.manual.limitLo = spec.limitLo;
        rec.manual.limitHi = spec.limitHi;
        rec.manual.a = bodyA;
        rec.manual.b = bodyB;
        rec.manual.axisLocalA = a->GetRot().RotateBack(axis);
        rec.manual.relRot0 = a->GetRot().GetConjugate() * b->GetRot();
        const ChVector3d axisN = axis * (1.0 / std::max(axis.Length(), 1e-9));
        rec.manual.limitRef = (b->GetPos() - a->GetPos()).Dot(axisN);
    };
    // ローカル座標への変換（手計算のモータ・ばね用）。
    auto localAxis = [&](const std::shared_ptr<ChBody>& body, const ChVector3d& w) {
        return body->GetRot().RotateBack(w);
    };
    auto localPoint = [&](const std::shared_ptr<ChBody>& body, const ChVector3d& p) {
        return body->GetRot().RotateBack(p - body->GetPos());
    };
    // Multicore の速度モータ: 角度 / 位置モータに「今の値 + 速度 × 経過時間」の
    // ランプ関数を渡す。
    auto rampFor = [&](double now, double value0, double speed) {
        return chrono_types::make_shared<WizRampFunction>(value0 - speed * now, speed);
    };

    switch (spec.type) {
        case JointType::Fixed: {
            auto l = chrono_types::make_shared<ChLinkLockLock>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Revolute: {
            const bool manualTorque = manualMode && spec.motor == MotorType::Force;
            if (spec.motor != MotorType::None && !manualTorque) {
                // モータは拘束ごと置き換わる（Z 軸まわり = Revolute と同じ）。
                std::shared_ptr<chrono::ChLinkMotorRotation> m;
                std::shared_ptr<chrono::ChFunction> fn;
                if (spec.motor == MotorType::Speed && manualMode) {
                    // Multicore: 角度モータ + ランプ関数（角速度 = 傾き）。
                    auto am = chrono_types::make_shared<ChLinkMotorRotationAngle>();
                    fn = rampFor(system_.sys()->GetChTime(), 0.0, spec.motorTarget);
                    rec.rampMotor = am;
                    m = am;
                } else {
                    fn = chrono_types::make_shared<WizConstFunction>(spec.motorTarget);
                    rec.motorFn = fn;
                    switch (spec.motor) {
                        case MotorType::Speed:
                            m = chrono_types::make_shared<ChLinkMotorRotationSpeed>();
                            break;
                        case MotorType::Position:
                            m = chrono_types::make_shared<ChLinkMotorRotationAngle>();
                            break;
                        default:
                            m = chrono_types::make_shared<ChLinkMotorRotationTorque>();
                            break;
                    }
                }
                m->Initialize(a, b, ChFrame<>(anchor, frameRot));
                m->SetMotorFunction(fn);
                link = m;
                wantLimit = false;  // モータに可動範囲は付かない
            } else {
                auto l = chrono_types::make_shared<ChLinkLockRevolute>();
                initLink(l.get(), a, b, anchor, frameRot, 0);
                if (spec.limited) {
                    if (manualMode) { manualLimit(false); limitDone = true; }
                    else limitDone = setLimitRz(l.get(), spec.limitLo, spec.limitHi, 0);
                }
                link = l;
                if (manualTorque) {
                    // Multicore のトルクモータ: 普通のちょうつがい + 毎ステップの角力積。
                    rec.manual.kind = JointRec::Manual::Kind::Torque;
                    rec.manual.a = bodyA;
                    rec.manual.b = bodyB;
                    rec.manual.axisLocalA = localAxis(a, axis);
                    rec.manual.target = spec.motorTarget;
                    wantLimit = false;
                }
            }
            // 回転ばね（自然角 = 開始姿勢）。
            if (spec.stiffness > 0.0 || spec.damping > 0.0) {
                if (manualMode) {
                    // トルクモータと同居できないので、その場合はばねを優先しない。
                    if (rec.manual.kind == JointRec::Manual::Kind::None) {
                        rec.manual.kind = JointRec::Manual::Kind::Rsda;
                        rec.manual.a = bodyA;
                        rec.manual.b = bodyB;
                        rec.manual.axisLocalA = localAxis(a, axis);
                        rec.manual.relRot0 = a->GetRot().GetConjugate() * b->GetRot();
                        rec.manual.k = spec.stiffness;
                        rec.manual.c = spec.damping;
                    } else {
                        LOGW("physics", "hinge: a torque motor and a spring on the same "
                                        "joint are not supported on the multicore backend "
                                        "- spring ignored");
                    }
                } else {
                    auto s = chrono_types::make_shared<ChLinkRSDA>();
                    s->Initialize(a, b, ChFrame<>(anchor, frameRot));
                    s->SetRestAngle(0.0);
                    s->SetSpringCoefficient(spec.stiffness);
                    s->SetDampingCoefficient(spec.damping);
                    rec.extra = s;
                }
            }
            break;
        }
        case JointType::Spherical: {
            auto l = chrono_types::make_shared<ChLinkLockSpherical>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Prismatic: {
            const bool manualForce = manualMode && spec.motor == MotorType::Force;
            if (spec.motor != MotorType::None && !manualForce) {
                // 直動モータは X 軸方向に動く（ChLinkLockPrismatic は Z）。
                const ChQuaternion<> xRot = quatFromXAxis(axis);
                std::shared_ptr<chrono::ChLinkMotorLinear> m;
                std::shared_ptr<chrono::ChFunction> fn;
                if (spec.motor == MotorType::Speed && manualMode) {
                    auto pm = chrono_types::make_shared<ChLinkMotorLinearPosition>();
                    fn = rampFor(system_.sys()->GetChTime(), 0.0, spec.motorTarget);
                    rec.rampMotor = pm;
                    rec.rampLinear = true;
                    m = pm;
                } else {
                    fn = chrono_types::make_shared<WizConstFunction>(spec.motorTarget);
                    rec.motorFn = fn;
                    switch (spec.motor) {
                        case MotorType::Speed:
                            m = chrono_types::make_shared<ChLinkMotorLinearSpeed>();
                            break;
                        case MotorType::Position:
                            m = chrono_types::make_shared<ChLinkMotorLinearPosition>();
                            break;
                        default:
                            m = chrono_types::make_shared<ChLinkMotorLinearForce>();
                            break;
                    }
                }
                m->Initialize(a, b, ChFrame<>(anchor, xRot));
                m->SetMotorFunction(fn);
                link = m;
                wantLimit = false;
            } else {
                auto l = chrono_types::make_shared<ChLinkLockPrismatic>();
                initLink(l.get(), a, b, anchor, frameRot, 0);
                if (spec.limited) {
                    if (manualMode) { manualLimit(true); limitDone = true; }
                    else limitDone = setLimitZ(l.get(), spec.limitLo, spec.limitHi, 0);
                }
                link = l;
                if (manualForce) {
                    rec.manual.kind = JointRec::Manual::Kind::Force;
                    rec.manual.a = bodyA;
                    rec.manual.b = bodyB;
                    rec.manual.axisLocalA = localAxis(a, axis);
                    rec.manual.target = spec.motorTarget;
                    wantLimit = false;
                }
            }
            if (spec.stiffness > 0.0 || spec.damping > 0.0) {
                if (manualMode) {
                    if (rec.manual.kind == JointRec::Manual::Kind::None) {
                        rec.manual.kind = JointRec::Manual::Kind::Spring;
                        rec.manual.a = bodyA;
                        rec.manual.b = bodyB;
                        rec.manual.p1Local = localPoint(a, a->IsFixed() ? anchor : a->GetPos());
                        rec.manual.p2Local = localPoint(b, b->IsFixed() ? anchor : b->GetPos());
                        rec.manual.k = spec.stiffness;
                        rec.manual.c = spec.damping;
                        rec.manual.rest = ((b->IsFixed() ? anchor : b->GetPos()) -
                                           (a->IsFixed() ? anchor : a->GetPos())).Length();
                    } else {
                        LOGW("physics", "slide: a force motor and a spring on the same joint "
                                        "are not supported on the multicore backend - "
                                        "spring ignored");
                    }
                } else {
                    rec.extra = makeSpring(0.0);
                }
            }
            break;
        }
        case JointType::Distance: {
            // 2 点は各ボディの現在位置。distance が 0 なら今の間隔を保つ。
            const ChVector3d p1 = a->GetPos();
            const ChVector3d p2 = b->GetPos();
            double len = spec.distance;
            if (len <= 0.0) {
                const ChVector3d d = p2 - p1;
                len = std::sqrt(d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
            }
            auto l = chrono_types::make_shared<ChLinkDistance>();
            if (!initDistance(l.get(), a, b, p1, p2, len, 0)) {
                LOGW("physics",
                     "distance joint: this Chrono version has a different "
                     "ChLinkDistance::Initialize - skipped");
                return kInvalidJoint;
            }
            link = l;
            break;
        }
        case JointType::Universal: {
            // 十字軸はリンク座標系の X と Y = 指定した軸（Z）に直交する 2 軸。
            auto l = chrono_types::make_shared<ChLinkUniversal>();
            l->Initialize(a, b, ChFrame<>(anchor, frameRot));
            link = l;
            if (spec.limited) {
                LOGW("physics", "universal joint: limits are not supported - ignored");
                wantLimit = false;
            }
            break;
        }
        case JointType::Cylindrical: {
            auto l = chrono_types::make_shared<ChLinkLockCylindrical>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            if (spec.limited) {
                if (manualMode) { manualLimit(false); limitDone = true; }
                else limitDone = setLimitRz(l.get(), spec.limitLo, spec.limitHi, 0);
            }
            link = l;
            break;
        }
        case JointType::Planar: {
            // 面 = リンク座標系の XY、法線 = Z = 指定した軸。
            auto l = chrono_types::make_shared<ChLinkLockPlanar>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::PointLine: {
            // 線はリンク座標系の X 軸なので、指定した軸を X に合わせる。
            auto l = chrono_types::make_shared<ChLinkLockPointLine>();
            initLink(l.get(), a, b, anchor, quatFromXAxis(axis), 0);
            link = l;
            break;
        }
        case JointType::PointPlane: {
            // 面の法線 = Z = 指定した軸。
            auto l = chrono_types::make_shared<ChLinkLockPointPlane>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            link = l;
            break;
        }
        case JointType::Gear: {
#ifdef WIZ_HAVE_GEAR
            // シャフト 1 = A 上の (anchor, axis)、シャフト 2 = B 上の
            // (anchor2, axis2)。どちらもボディのローカル座標系で渡す。
            const ChVector3d anchor2 = spec.hasAnchor2 ? spec.anchor2 : b->GetPos();
            const ChVector3d axis2 =
                (spec.hasAxis2 && axisUsable(spec.axis2)) ? spec.axis2 : axis;
            auto l = chrono_types::make_shared<ChLinkLockGear>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            const ChFrame<> s1 = worldToBodyLocal(*a, anchor, frameRot);
            const ChFrame<> s2 = worldToBodyLocal(*b, anchor2, quatFromZAxis(axis2));
            if (!setGearParams(l.get(), spec.ratio, s1, s2, 0)) {
                LOGW("physics", "gear joint: this Chrono has a different ChLinkLockGear "
                                "API - skipped");
                return kInvalidJoint;
            }
            link = l;
#else
            LOGW("physics", "gear joint: this Chrono build has no ChLinkLockGear - skipped");
            return kInvalidJoint;
#endif
            break;
        }
        case JointType::Screw: {
#ifdef WIZ_HAVE_SCREW
            auto l = chrono_types::make_shared<ChLinkLockScrew>();
            initLink(l.get(), a, b, anchor, frameRot, 0);
            if (!setScrewThread(l.get(), spec.pitch, 0)) {
                LOGW("physics", "screw joint: this Chrono has a different ChLinkLockScrew "
                                "API - skipped");
                return kInvalidJoint;
            }
            link = l;
#else
            LOGW("physics", "screw joint: this Chrono build has no ChLinkLockScrew - skipped");
            return kInvalidJoint;
#endif
            break;
        }
        case JointType::Spring: {
            if (a->IsFixed() && b->IsFixed()) {
                LOGW("physics", "spring: both ends are fixed - skipped");
                return kInvalidJoint;
            }
            if (manualMode) {
                // リンク無し。毎ステップの力積だけ（applyManual）。
                const ChVector3d p1 = a->IsFixed() ? anchor : a->GetPos();
                const ChVector3d p2 = b->IsFixed() ? anchor : b->GetPos();
                rec.manual.kind = JointRec::Manual::Kind::Spring;
                rec.manual.a = bodyA;
                rec.manual.b = bodyB;
                rec.manual.p1Local = localPoint(a, p1);
                rec.manual.p2Local = localPoint(b, p2);
                rec.manual.k = spec.stiffness;
                rec.manual.c = spec.damping;
                rec.manual.rest = spec.distance > 0.0 ? spec.distance : (p2 - p1).Length();
            } else {
                link = makeSpring(spec.distance);
            }
            break;
        }
        case JointType::Bushing: {
            // ゴムブッシュ（B の 19）: 拘束ではなく荷重コンテナの力。並進 3 軸は
            // stiffness / damping、回転 3 軸は rotStiffness / rotDamping。
            // 取り付け座標系はアンカー + 指定軸を Z にした向き。
#ifdef WIZ_HAVE_LOADS
            if (!jointLoads_) {
                LOGW("physics", "bushing: needs the core backend (loads are not integrated "
                                "by Chrono::Multicore) - skipped");
                return kInvalidJoint;
            }
            const ChVector3d k(spec.stiffness, spec.stiffness, spec.stiffness);
            const ChVector3d c(spec.damping, spec.damping, spec.damping);
            const ChVector3d kr(spec.rotStiffness, spec.rotStiffness, spec.rotStiffness);
            const ChVector3d cr(spec.rotDamping, spec.rotDamping, spec.rotDamping);
            auto load = makeBushing<ChLoadBodyBodyBushingMate>(
                a, b, ChFrame<>(anchor, frameRot), k, c, kr, cr, 0);
            jointLoads_->Add(load);
            rec.load = load;
            wantLimit = false;
            if (spec.breakForce > 0.0) {
                LOGW("physics", "bushing: breakforce is not supported on a load - ignored");
                rec.breakForce = 0.0;
            }
#else
            LOGW("physics", "bushing: this Chrono build has no ChLoad - skipped");
            return kInvalidJoint;
#endif
            break;
        }
    }

    if (!link && !rec.load && rec.manual.kind == JointRec::Manual::Kind::None) {
        return kInvalidJoint;
    }
    if (wantLimit && !limitDone) {
        LOGW("physics", "joint: limits could not be applied (this joint type or "
                        "Chrono version has no limit API) - ignored");
    }
    if (link) system_.sys()->AddLink(link);
    if (rec.extra) system_.sys()->AddLink(rec.extra);
    rec.link = link;
    LOGI("physics", "joint #%zu: type %d bodies %zu-%zu motor %d limited %d spring %s manual %d",
         joints_.size(), int(spec.type), bodyA, bodyB, int(spec.motor), int(spec.limited),
         rec.extra ? "yes" : "no", int(rec.manual.kind));
    joints_.push_back(std::move(rec));
    return joints_.size() - 1;
}

void Joints::removeAll() {
    // 暖機は先にやり直しにする（途中の RemoveLink が例外を投げても、次の走行は
    // 暖機から始まる）。
    breakSteps_ = 0;
    brokenPending_.clear();
    bool anyLoad = false;
    for (auto& j : joints_) {
        // 破断したリンクも無効化されて系に残っている（走行中の RemoveLink は
        // Multicore を壊す）ので、ここでまとめて外す。
        system_.removeLink(j.link);
        system_.removeLink(j.extra);
        if (j.load) anyLoad = true;
    }
#ifdef WIZ_HAVE_LOADS
    // ブッシュはジョイント用の荷重コンテナにだけ入っているので、まとめて捨てる。
    if (anyLoad && jointLoads_) clearLoads(jointLoads_.get(), 0);
#else
    (void)anyLoad;
#endif
    joints_.clear();
    // 拘束が減ったのでソルバの配置を組み直す（理由は PhysicsSystem::setup）。
    system_.setup();
}

bool Joints::setMotorTarget(std::size_t joint, double target) {
    if (joint >= joints_.size()) return false;
    JointRec& j = joints_[joint];
    if (j.broken) return false;
    if (j.manual.kind == JointRec::Manual::Kind::Torque ||
        j.manual.kind == JointRec::Manual::Kind::Force) {
        j.manual.target = target;
        return true;
    }
    if (j.rampMotor) {
        // 今の角度 / 位置から新しい傾きで引き直す（跳びを作らない）。
        const double now = system_.sys()->GetChTime();
        if (j.rampLinear) {
            auto pm = std::dynamic_pointer_cast<ChLinkMotorLinearPosition>(j.rampMotor);
            if (!pm) return false;
            pm->SetMotorFunction(chrono_types::make_shared<WizRampFunction>(
                pm->GetMotorPos() - target * now, target));
        } else {
            auto am = std::dynamic_pointer_cast<ChLinkMotorRotationAngle>(j.rampMotor);
            if (!am) return false;
            am->SetMotorFunction(chrono_types::make_shared<WizRampFunction>(
                am->GetMotorAngle() - target * now, target));
        }
        return true;
    }
    if (!j.motorFn) return false;
    setConstValue(static_cast<WizConstFunction*>(j.motorFn.get()), target, 0);
    return true;
}

bool Joints::reaction(std::size_t joint, double& force, double& torque) const {
    force = 0.0;
    torque = 0.0;
    if (joint >= joints_.size()) return false;
    const JointRec& j = joints_[joint];
    if (j.broken) return false;
    if (j.manual.kind != JointRec::Manual::Kind::None) {
        // 手計算のばね / モータ: 拘束の反力があればそれに手計算のぶんを足す。
        double lf = 0.0, lt = 0.0;
        if (j.link) linkReaction(j.link.get(), lf, lt, 0);
        force = std::max(lf, j.manual.lastForce);
        torque = std::max(lt, j.manual.lastTorque);
        return true;
    }
#ifdef WIZ_HAVE_LOADS
    if (j.load) {
        auto bushing = std::dynamic_pointer_cast<ChLoadBodyBodyBushingMate>(j.load);
        return bushing && loadReaction(bushing.get(), force, torque, 0);
    }
#endif
    if (!j.link) return false;
    if (j.type == JointType::Spring) {
        // ばねは拘束ではないので反力の口が無い。ばね力を返す。
        if (auto s = std::dynamic_pointer_cast<ChLinkTSDA>(j.link)) {
            force = std::fabs(s->GetForce());
            return true;
        }
        return false;
    }
    return linkReaction(j.link.get(), force, torque, 0);
}

bool Joints::broken(std::size_t joint) const {
    return joint < joints_.size() && joints_[joint].broken;
}

std::vector<std::size_t> Joints::takeBroken() {
    std::vector<std::size_t> out;
    out.swap(brokenPending_);
    return out;
}

void Joints::logReactions() const {
    for (std::size_t j = 0; j < joints_.size(); ++j) {
        double f = 0.0, t = 0.0;
        const bool ok = reaction(j, f, t);
        LOGE("physics", "  joint #%zu type %d: reaction %s (force %.3g torque %.3g)%s",
             j, int(joints_[j].type),
             ok ? (std::isfinite(f + t) ? "finite" : "NaN") : "n/a", f, t,
             joints_[j].manual.kind != JointRec::Manual::Kind::None ? " [manual]" : "");
    }
}

void Joints::checkBreaks() {
    // 開始直後はソルバが収束途中で反力が跳ねる（Multicore で静止荷重 59 N の
    // 棒が 1 ステップ目に 325 N を返した）ので、最初の数ステップは見ない。
    constexpr int kWarmupSteps = 10;
    // 数えるのは暖機の間だけ（kWarmupSteps で止める = 長く走らせてもあふれない）。
    // N ステップ目の呼び出しで N になり、kWarmupSteps ステップ目から判定する。
    if (breakSteps_ < kWarmupSteps) ++breakSteps_;
    if (breakSteps_ < kWarmupSteps) return;
    for (std::size_t i = 0; i < joints_.size(); ++i) {
        JointRec& j = joints_[i];
        if (j.broken || j.breakForce <= 0.0 || j.load) continue;
        double force = 0.0, torque = 0.0;
        if (!reaction(i, force, torque)) continue;
        if (!std::isfinite(force) || force <= j.breakForce) continue;
        // 外すのではなく無効化する。走行中の RemoveLink + Setup は Multicore の
        // データマネージャを壊し、拘束の付いた物が全部 NaN になった。
        // 系からは次の removeAll（停止時）でまとめて外す。
        bool ok = true;
        if (j.link) ok = disableLink(j.link.get(), 0) && ok;
        if (j.extra) ok = disableLink(j.extra.get(), 0) && ok;
        if (!ok) {
            LOGW("physics", "joint #%zu: this Chrono has no SetDisabled - break ignored", i);
            j.breakForce = 0.0;  // 二度と試さない
            continue;
        }
        j.broken = true;
        brokenPending_.push_back(i);
        LOGI("physics", "joint #%zu broke (reaction %.1f N > %.1f N)", i, force,
             j.breakForce);
    }
}

void Joints::createContainer() {
#ifdef WIZ_HAVE_LOADS
    if (!system_.integratesLoads()) return;
    jointLoads_ = chrono_types::make_shared<ChLoadContainer>();
    system_.sys()->Add(jointLoads_);
#endif
}

void Joints::clear() {
    joints_.clear();
    brokenPending_.clear();
    jointLoads_.reset();
    breakSteps_ = 0;
}

}  // namespace physics
}  // namespace wizengine
