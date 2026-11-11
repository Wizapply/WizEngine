// 接触の報告: 接触コンテナの走査と、番号の持ち主への寄せ。

#include "physics/PhysicsParts.h"
#include "physics/ChronoCompat.h"

#include <chrono/core/ChMatrix33.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactContainer.h>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace {

// 接触コンテナの走査で、ボディ同士の接触だけを physId のペア + 力・法線・
// めり込みで集める。ChContactable* から ChBody* へは dynamic_cast（Chrono
// 自身が SumAllContactForces で使っている手）。同じペアに複数の接触点が
// あるので、ペアだけ欲しい側（activeContactPairs）が 1 本化する。
// react_forces は接触座標系（X = 法線）の力 (N)。NSC でも Chrono が
// 力積 / dt に直して渡してくる。
class ContactCollector : public chrono::ChContactContainer::ReportContactCallback {
public:
    ContactCollector(const wizengine::physics::BodyRegistry& bodies,
                     std::vector<ContactInfo>& out)
        : bodies_(bodies), out_(out) {}

    bool OnReportContact(const ChVector3d&, const ChVector3d&,
                         const ChMatrix33<>& plane, const double& distance,
                         const double&, const ChVector3d& reactForce,
                         const ChVector3d&, ChContactable* objA,
                         ChContactable* objB) override {
        const auto* bodyA = dynamic_cast<const ChBody*>(objA);
        const auto* bodyB = dynamic_cast<const ChBody*>(objB);
        if (!bodyA || !bodyB) return true;  // ボディ以外（FEA 等）は対象外
        const std::size_t ia = bodies_.indexOf(bodyA);
        const std::size_t ib = bodies_.indexOf(bodyB);
        if (ia == PhysicsWorld::kInvalidId || ib == PhysicsWorld::kInvalidId) return true;
        ContactInfo c;
        c.a = ia;
        c.b = ib;
        if (c.a == c.b) return true;
        const ChVector3d n = matrixXAxis(plane, 0);  // A から B へ向く法線
        if (c.a > c.b) {
            std::swap(c.a, c.b);
            c.nx = -n.x(); c.ny = -n.y(); c.nz = -n.z();
        } else {
            c.nx = n.x(); c.ny = n.y(); c.nz = n.z();
        }
        c.force = reactForce.Length();
        c.normalForce = reactForce.x();
        c.depth = -distance;  // Chrono は「重なり = 負の距離」
        out_.push_back(c);
        return true;  // 続けて最後まで走査する
    }

private:
    const wizengine::physics::BodyRegistry& bodies_;
    std::vector<ContactInfo>& out_;
};

}  // namespace

std::vector<ContactInfo> PhysicsWorld::activeContacts() const {
    std::vector<ContactInfo> contacts;
    const auto container = p_->system.sys()->GetContactContainer();
    if (!container) return contacts;
    auto collector = chrono_types::make_shared<ContactCollector>(p_->bodies, contacts);
    container->ReportAllContacts(collector);
    // ソフトボディの粒子は代表番号に、子ボディ（setAlias）は持ち主に寄せる
    // （Scene はその番号しか知らない）。同じ物どうしは一致するので落ちる。
    std::size_t kept = 0;
    for (auto& c : contacts) {
        const std::size_t a = p_->bodies.owner(c.a);
        const std::size_t b = p_->bodies.owner(c.b);
        if (a == b) continue;
        ContactInfo out = c;
        out.a = a;
        out.b = b;
        if (a > b) {  // 番号順に揃え、法線も a → b のまま保つ
            std::swap(out.a, out.b);
            out.nx = -out.nx; out.ny = -out.ny; out.nz = -out.nz;
        }
        contacts[kept++] = out;
    }
    contacts.resize(kept);
    return contacts;
}

std::vector<std::pair<std::size_t, std::size_t>>
PhysicsWorld::activeContactPairs() const {
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (const ContactInfo& c : activeContacts()) pairs.push_back({c.a, c.b});
    // 1 ペアに接触点は複数あるのが普通（箱同士は最大 4 点）。ここで 1 本化。
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}
