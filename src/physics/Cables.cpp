// FEA ケーブル: ANCF 梁の生成・端の留め方・接触面と、節点の位置・張力の問い合わせ。

#include "physics/Cables.h"
#include "core/Log.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/ContactMaterials.h"
#include "physics/PhysicsSystem.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactMaterial.h>
#include <chrono/physics/ChLinkBase.h>
#include <chrono/physics/ChSystem.h>
#ifdef WIZ_HAVE_FEA
#include <chrono/collision/ChCollisionSystem.h>
#include <chrono/fea/ChBeamSectionCable.h>
#include <chrono/fea/ChBuilderBeam.h>
#include <chrono/fea/ChContactSurfaceNodeCloud.h>
#include <chrono/fea/ChElementCableANCF.h>
#include <chrono/fea/ChMesh.h>
#include <chrono/fea/ChNodeFEAxyzD.h>
#ifdef WIZ_HAVE_FEA_LINK_NODE_FRAME
#include <chrono/fea/ChLinkNodeFrame.h>
#else
#include <chrono/fea/ChLinkPointFrame.h>
#endif
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

#ifdef WIZ_HAVE_FEA
namespace {

// 端の拘束（節点 ⇄ ボディ）。Chrono 9 は ChLinkNodeFrame、旧版は ChLinkPointFrame。
#ifdef WIZ_HAVE_FEA_LINK_NODE_FRAME
using WizNodeFrameLink = chrono::fea::ChLinkNodeFrame;
#else
using WizNodeFrameLink = chrono::fea::ChLinkPointFrame;
#endif

}  // namespace
#endif

// ---- ケーブル（FEA の ANCF 梁）---------------------------------------------
// ANCF ケーブル要素を直線に並べ、端を固定点 / ボディ / 自由にする。節点の
// 球で接触する（ChContactSurfaceNodeCloud）。Core 専用: Multicore のデータ
// マネージャは剛体しか持たず、FEA の要素は積分に乗らない。

std::size_t Cables::add(const CableSpec& spec) {
#ifdef WIZ_HAVE_FEA
    static bool warnedBackend = false;
    if (system_.backend() == PhysicsBackend::Multicore) {
        if (!warnedBackend) {
            warnedBackend = true;
            LOGW("physics", "cable: needs the core backend (Chrono::Multicore has no FEA) - "
                            "skipped");
        }
        return kInvalidId;
    }
    auto endOk = [&](CableSpec::End end, std::size_t id, const char* which) {
        if (end != CableSpec::End::Body) return true;
        if (id >= bodies_.count() || !bodies_.active(id) || bodies_.isGroupMember(id)) {
            LOGW("physics", "cable: end %s refers to a missing or soft body (%zu) - skipped",
                 which, id);
            return false;
        }
        return true;
    };
    if (!endOk(spec.endA, spec.bodyA, "A") || !endOk(spec.endB, spec.bodyB, "B")) {
        return kInvalidId;
    }
    const int n = std::max(2, std::min(64, spec.segments));
    const double length = (spec.b - spec.a).Length();
    if (length < 1e-6) {
        LOGW("physics", "cable: zero length - skipped");
        return kInvalidId;
    }

    auto mesh = chrono_types::make_shared<fea::ChMesh>();
    auto section = chrono_types::make_shared<fea::ChBeamSectionCable>();
    section->SetDiameter(spec.diameter);
    section->SetYoungModulus(spec.young);
    setSectionDensity(section.get(), spec.density, 0);
    setSectionDamping(section.get(), spec.damping, 0);

    fea::ChBuilderCableANCF builder;
    builder.BuildBeam(mesh, section, n, spec.a, spec.b);
    CableRec rec;
    rec.mesh = mesh;
    rec.nodes = builder.GetLastBeamNodes();
    if (rec.nodes.size() < 2) {
        LOGW("physics", "cable: the beam builder produced no nodes - skipped");
        return kInvalidId;
    }

    if (spec.collide) {
        // 節点ごとの球（半径 = ケーブルの半径）。材質は剛体と同じ流儀。
        // 面はメッシュを知っている必要がある: コンストラクタで渡し、
        // AddContactSurface でも結び直される。
        auto mat = materials_.materialFor(spec.options);
        auto surf = chrono_types::make_shared<fea::ChContactSurfaceNodeCloud>(mat, mesh.get());
        mesh->AddContactSurface(surf);
        // 留め先のボディの内側にある節点は入れない（端の節点はボディの中心に
        // 留まるので、入れると自分の留め先と深くめり込んだ接触ができて
        // 留め先が暴れる）。
        const double radius = spec.diameter * 0.5;
        std::size_t added = 0;
        for (const auto& node : rec.nodes) {
            const ChVector3d p = node->GetPos();
            if (spec.endA == CableSpec::End::Body && (p - spec.a).Length() < spec.clearA) continue;
            if (spec.endB == CableSpec::End::Body && (p - spec.b).Length() < spec.clearB) continue;
            surf->AddNode(node, radius);
            ++added;
        }
        if (added == 0) {
            LOGW("physics", "cable: every node lies inside an attached body - no contact");
        }
    }
    setMeshGravity(mesh.get(), true, 0);
    system_.sys()->Add(mesh);
    // 衝突系が初期化済みなら登録する（ボディと同じ理由: Chrono 9 は後から
    // 足した物を BindAll で拾わない）。
    if (spec.collide) {
        auto coll = system_.sys()->GetCollisionSystem();
        if (coll && coll->IsInitialized()) coll->BindItem(mesh);
    }

    auto attach = [&](const std::shared_ptr<fea::ChNodeFEAxyzD>& node, CableSpec::End end,
                      std::size_t id) {
        if (end == CableSpec::End::Fixed) {
            node->SetFixed(true);
            return;
        }
        if (end != CableSpec::End::Body) return;
        auto link = chrono_types::make_shared<WizNodeFrameLink>();
        link->Initialize(node, bodies_.body(id));
        system_.sys()->AddLink(link);
        rec.links.push_back(link);
        wakeUp(bodies_.body(id).get(), 0);
    };
    attach(rec.nodes.front(), spec.endA, spec.bodyA);
    attach(rec.nodes.back(), spec.endB, spec.bodyB);
    cables_.push_back(std::move(rec));
    system_.setup();
    LOGI("physics", "cable #%zu: %d elements, %zu nodes, length %.2f m, young %.3g Pa, "
                    "collide %d",
         cables_.size() - 1, n, cables_.back().nodes.size(), length, spec.young,
         int(spec.collide));
    return cables_.size() - 1;
#else
    (void)spec;
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOGW("physics", "this Chrono build has no FEA headers - cables are skipped");
    }
    return kInvalidId;
#endif
}

void Cables::removeAll() {
#ifdef WIZ_HAVE_FEA
    if (cables_.empty()) return;
    for (auto& c : cables_) {
        // リンクもメッシュも 1 つずつ手元に取ってから外す（理由は PhysicsSystem::removeLink）。
        for (auto& l : c.links) system_.removeLink(l);
        if (auto mesh = std::exchange(c.mesh, nullptr)) {
            removeMeshFrom(system_.sys().get(), mesh, 0);
        }
    }
    cables_.clear();
    system_.setup();
#endif
}

void Cables::nodePositions(std::size_t cable, std::vector<float>& out) const {
    out.clear();
#ifdef WIZ_HAVE_FEA
    if (cable >= cables_.size()) return;
    const CableRec& c = cables_[cable];
    out.reserve(c.nodes.size() * 3);
    for (const auto& node : c.nodes) {
        const ChVector3d p = node->GetPos();
        out.push_back(float(p.x()));
        out.push_back(float(p.y()));
        out.push_back(float(p.z()));
    }
#else
    (void)cable;
#endif
}

double Cables::tension(std::size_t cable) const {
    if (cable >= cables_.size()) return 0.0;
    double best = 0.0;
    for (const auto& l : cables_[cable].links) {
        if (!l) continue;  // removeAll が途中で投げたとき、外し終えたリンクは null
        double f = 0.0, t = 0.0;
        if (linkReaction(l.get(), f, t, 0) && std::isfinite(f)) best = std::max(best, f);
    }
    return best;
}

}  // namespace physics
}  // namespace wizengine
