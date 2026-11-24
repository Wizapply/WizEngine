// 1D のシャフト網（Chrono の ChShaft 系）。

#include "physics/ShaftNetworks.h"
#include "core/Log.h"
#include "physics/BodyRegistry.h"
#include "physics/PhysicsSystem.h"

#include <chrono/functions/ChFunctionConst.h>
#include <chrono/functions/ChFunctionInterp.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChShaft.h>
#include <chrono/physics/ChShaftBodyConstraint.h>
#include <chrono/physics/ChShaftsClutch.h>
#include <chrono/physics/ChShaftsGear.h>
#include <chrono/physics/ChShaftsMotorSpeed.h>
#include <chrono/physics/ChShaftsPlanetary.h>
#include <chrono/physics/ChShaftsTorqueConverter.h>
#include <chrono/physics/ChShaftsTorsionSpring.h>
#include <chrono/physics/ChSystem.h>

#include <algorithm>
#include <utility>

using namespace chrono;

namespace wizengine {
namespace physics {

// ---- 組む ---------------------------------------------------------------------
// Core 専用: Chrono::Multicore のデータマネージャは剛体しか持たず、ChShaft を積分しない
// （Scene がシャフト網のあるシーンを Core にする = requiredBackend）。
std::size_t ShaftNetworks::add(const ShaftNetworkSpec& spec) {
    if (system_.backend() == PhysicsBackend::Multicore) {
        LOGW("physics", "shaft network: needs the core backend (Chrono::Multicore has no "
                        "1D shafts) - skipped");
        return kInvalidId;
    }
    const int n = int(spec.shafts.size());
    auto okShaft = [&](int i, bool housingOk) { return (housingOk && i == -1) || (i >= 0 && i < n); };
    for (const auto& p : spec.planetaries) {
        if (!okShaft(p.carrier, false) || !okShaft(p.sun, false) || !okShaft(p.ring, false)) {
            LOGW("physics", "shaft network: planetary refers to a missing shaft - skipped");
            return kInvalidId;
        }
    }
    for (const auto& c : spec.clutches) {
        if (!okShaft(c.a, false) || !okShaft(c.b, true)) {
            LOGW("physics", "shaft network: clutch refers to a missing shaft - skipped");
            return kInvalidId;
        }
    }
    for (const auto& s : spec.springs) {
        if (!okShaft(s.a, true) || !okShaft(s.b, true)) {
            LOGW("physics", "shaft network: spring refers to a missing shaft - skipped");
            return kInvalidId;
        }
    }
    for (const auto& g : spec.gears) {
        if (!okShaft(g.a, true) || !okShaft(g.b, true)) {
            LOGW("physics", "shaft network: gear refers to a missing shaft - skipped");
            return kInvalidId;
        }
    }
    for (const auto& c : spec.converters) {
        if (!okShaft(c.pump, false) || !okShaft(c.turbine, false) || c.sr.size() < 2 ||
            c.sr.size() != c.k.size() || c.sr.size() != c.tr.size()) {
            LOGW("physics", "shaft network: torque converter is incomplete - skipped");
            return kInvalidId;
        }
    }
    for (const auto& m : spec.motors) {
        if (!okShaft(m.shaft, false)) {
            LOGW("physics", "shaft network: motor refers to a missing shaft - skipped");
            return kInvalidId;
        }
    }
    for (const auto& b : spec.bodyLinks) {
        if (!okShaft(b.shaft, false) || b.body >= bodies_.count() || !bodies_.active(b.body) ||
            bodies_.isGroupMember(b.body)) {
            LOGW("physics", "shaft network: body link refers to a missing or soft body (%zu) - "
                            "skipped",
                 b.body);
            return kInvalidId;
        }
    }

    ChSystem& sys = *system_.sys();
    Net net;
    auto addItem = [&](const std::shared_ptr<ChPhysicsItem>& item) {
        sys.Add(item);
        net.items.push_back(item);
    };
    auto makeShaft = [&](double inertia, double w0, bool fixed) {
        auto s = chrono_types::make_shared<ChShaft>();
        s->SetInertia(std::max(1e-6, inertia));
        s->SetPosDt(w0);
        s->SetFixed(fixed);
        // 眠らせない（一部だけ眠ると拘束の相手が動いても起きない）。
        s->SetSleepingAllowed(false);
        addItem(s);
        return s;
    };
    net.housing = makeShaft(1.0, 0.0, true);
    for (const auto& s : spec.shafts) net.shafts.push_back(makeShaft(s.inertia, s.w0, false));
    auto shaft = [&](int i) { return i < 0 ? net.housing : net.shafts[std::size_t(i)]; };

    for (const auto& p : spec.planetaries) {
        auto pl = chrono_types::make_shared<ChShaftsPlanetary>();
        pl->Initialize(shaft(p.carrier), shaft(p.sun), shaft(p.ring));
        // r1 ωc + r2 ωs + r3 ωr = 0 = Willis の −(1 + k) ωc + ωs + k ωr。
        pl->SetTransmissionRatios(-(1.0 + p.k), 1.0, p.k);
        addItem(pl);
        net.planetaries.push_back(pl);
    }
    for (const auto& c : spec.clutches) {
        auto cl = chrono_types::make_shared<ChShaftsClutch>();
        cl->Initialize(shaft(c.a), shaft(c.b));
        cl->SetTorqueLimit(0.0);
        cl->SetModulation(1.0);
        addItem(cl);
        net.clutches.push_back(cl);
    }
    for (const auto& s : spec.springs) {
        auto sp = chrono_types::make_shared<ChShaftsTorsionSpring>();
        sp->Initialize(shaft(s.a), shaft(s.b));
        sp->SetTorsionalStiffness(s.stiffness);
        sp->SetTorsionalDamping(s.damping);
        addItem(sp);
        net.springs.push_back(sp);
    }
    for (const auto& g : spec.gears) {
        auto ge = chrono_types::make_shared<ChShaftsGear>();
        ge->Initialize(shaft(g.a), shaft(g.b));
        ge->SetTransmissionRatio(g.ratio);
        addItem(ge);
        net.gears.push_back(ge);
    }
    for (const auto& c : spec.converters) {
        auto tc = chrono_types::make_shared<ChShaftsTorqueConverter>();
        tc->Initialize(shaft(c.pump), shaft(c.turbine), net.housing);
        auto k = chrono_types::make_shared<ChFunctionInterp>();
        auto tr = chrono_types::make_shared<ChFunctionInterp>();
        for (std::size_t i = 0; i < c.sr.size(); ++i) {
            k->AddPoint(c.sr[i], c.k[i]);
            tr->AddPoint(c.sr[i], c.tr[i]);
        }
        tc->SetCurveCapacityFactor(k);
        tc->SetCurveTorqueRatio(tr);
        addItem(tc);
        net.converters.push_back(tc);
    }
    for (const auto& m : spec.motors) {
        auto mo = chrono_types::make_shared<ChShaftsMotorSpeed>();
        mo->Initialize(shaft(m.shaft), net.housing);
        auto f = chrono_types::make_shared<ChFunctionConst>(shaft(m.shaft)->GetPosDt());
        mo->SetSpeedFunction(f);
        addItem(mo);
        net.motors.push_back(mo);
        net.motorSpeeds.push_back(f);
    }
    for (const auto& b : spec.bodyLinks) {
        auto link = chrono_types::make_shared<ChShaftBodyRotation>();
        link->Initialize(shaft(b.shaft), bodies_.body(b.body),
                         ChVector3d(b.dir[0], b.dir[1], b.dir[2]));
        addItem(link);
        net.bodyLinks.push_back(link);
    }
    system_.setup();
    nets_.push_back(std::move(net));
    return nets_.size() - 1;
}

void ShaftNetworks::removeAll() {
    if (nets_.empty()) return;
    ChSystem& sys = *system_.sys();
    for (auto& net : nets_) {
        // 拘束を先に（足したのと逆の順）。1 つずつ手元に取ってから外す。
        for (auto it = net.items.rbegin(); it != net.items.rend(); ++it) {
            if (auto item = std::exchange(*it, nullptr)) sys.Remove(item);
        }
    }
    nets_.clear();
    system_.setup();
}

// ---- 毎ステップ ---------------------------------------------------------------
void ShaftNetworks::setLoads(std::size_t id, const std::vector<double>& torque) {
    if (id >= nets_.size()) return;
    auto& s = nets_[id].shafts;
    for (std::size_t i = 0; i < s.size() && i < torque.size(); ++i) s[i]->SetAppliedLoad(torque[i]);
}

void ShaftNetworks::setClutchLimits(std::size_t id, const std::vector<double>& limit) {
    if (id >= nets_.size()) return;
    auto& c = nets_[id].clutches;
    for (std::size_t i = 0; i < c.size() && i < limit.size(); ++i) {
        c[i]->SetTorqueLimit(std::max(0.0, limit[i]));
    }
}

void ShaftNetworks::setMotorSpeeds(std::size_t id, const std::vector<double>& w) {
    if (id >= nets_.size()) return;
    auto& f = nets_[id].motorSpeeds;
    for (std::size_t i = 0; i < f.size() && i < w.size(); ++i) f[i]->SetConstant(w[i]);
}

void ShaftNetworks::read(std::size_t id, ShaftNetworkState& out) const {
    if (id >= nets_.size()) return;
    const Net& n = nets_[id];
    out.w.resize(n.shafts.size());
    for (std::size_t i = 0; i < n.shafts.size(); ++i) out.w[i] = n.shafts[i]->GetPosDt();
    out.clutchTorque.resize(n.clutches.size());
    for (std::size_t i = 0; i < n.clutches.size(); ++i) out.clutchTorque[i] = n.clutches[i]->GetReaction1();
    out.sunTorque.resize(n.planetaries.size());
    for (std::size_t i = 0; i < n.planetaries.size(); ++i) {
        out.sunTorque[i] = n.planetaries[i]->GetReaction2();
    }
    out.springTorque.resize(n.springs.size());
    for (std::size_t i = 0; i < n.springs.size(); ++i) out.springTorque[i] = n.springs[i]->GetReaction2();
    out.gearTorque.resize(n.gears.size());
    for (std::size_t i = 0; i < n.gears.size(); ++i) out.gearTorque[i] = n.gears[i]->GetReaction2();
    out.motorTorque.resize(n.motors.size());
    for (std::size_t i = 0; i < n.motors.size(); ++i) out.motorTorque[i] = n.motors[i]->GetReaction1();
    out.bodyTorque.resize(n.bodyLinks.size());
    for (std::size_t i = 0; i < n.bodyLinks.size(); ++i) {
        out.bodyTorque[i] = n.bodyLinks[i]->GetTorqueReactionOnShaft();
    }
    out.converters.resize(n.converters.size());
    for (std::size_t i = 0; i < n.converters.size(); ++i) {
        out.converters[i].pumpTorque = n.converters[i]->GetTorqueReactionOnInput();
        out.converters[i].turbineTorque = n.converters[i]->GetTorqueReactionOnOutput();
        out.converters[i].speedRatio = n.converters[i]->GetSpeedRatio();
    }
}

}  // namespace physics
}  // namespace wizengine
