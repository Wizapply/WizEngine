// テレイン: 高さ格子との自前の接触を Chrono の接触へ詰め替える。

#include "physics/PhysicsWorldInternal.h"
#include "core/Log.h"

#include <chrono/collision/ChCollisionInfo.h>
#include <chrono/collision/ChCollisionModel.h>
#include <chrono/core/ChMatrix33.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactContainer.h>
#include <chrono/physics/ChContactMaterialNSC.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace {

ChVector3d fromTerrainVec(const wterrain::Vec3& v) {
    return ChVector3d(v.x(), v.y(), v.z());
}

}  // namespace

// ---- テレイン（高さ格子との自前の接触）---------------------------------------
// 検出は terrain::collectContacts（Chrono 非依存、tests/terrain で検査）。ここは
// その結果を Chrono の接触へ詰め替えるだけで、詰め方はベンチ
// （tests/chrono_bench の mode=hf hfimpl=lib、B1 / B2 で検証済み）と同じ:
//   A = 物（vpA = onBody）、B = ゴースト（vpB = onTerrain）、vN = −normal
//   （A → B）、distance = gap（負 = めり込み）、reaction_cache = 点ごとの永続
//   float[6]（前のステップに接触しなかった点は 0 に戻してから渡す）、
//   材質 = AddContact(物の材質, round ? 転がりあり : 転がり・スピン 0)。
// ゴーストは空の衝突モデルを持つ固定ボディで、EnableCollision を呼ばない =
// Bullet に登録されない（StaticPairFilter の集合にも入らない: bulletKeyOf が
// null）。AddBody と registerBody は通す（接触の相手の変数・逆引きに要る）。

void PhysicsWorld::setTerrainProxy(std::size_t id, const wterrain::BodyProxy& proxy) {
    if (id >= terrainProxies_.size()) return;
    auto rec = std::make_shared<TerrainProxyRec>();
    rec->proxy = proxy;
    rec->proxy.boundRadius = wterrain::computeBoundRadius(rec->proxy);
    const int points = wterrain::proxyPointCount(rec->proxy, terrainContactParams());
    if (points <= 0) {
        terrainProxies_[id].reset();
        return;
    }
    // ここで 1 回だけ確保する（以後は大きさを変えない = アドレスが動かない）。
    rec->cache.assign(std::size_t(points), std::array<float, 6>{0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
    rec->was.assign(std::size_t(points), 0);
    rec->now.assign(std::size_t(points), 0);
    terrainProxies_[id] = std::move(rec);
}

void PhysicsWorld::forgetTerrainContacts(std::size_t id) {
    if (id >= terrainProxies_.size() || !terrainProxies_[id]) return;
    TerrainProxyRec& rec = *terrainProxies_[id];
    std::fill(rec.was.begin(), rec.was.end(), char(0));
    for (auto& c : rec.cache) c.fill(0.0f);
}

bool PhysicsWorld::hasTerrainProxy(std::size_t id) const {
    return id < terrainProxies_.size() && terrainProxies_[id] && !terrainProxies_[id]->fallback;
}

// テレインと接触しうるボディか（眠りは見ない = 一時的なので）。
// terrainContactShape と collectTerrainContacts の共通の条件はここ 1 か所:
// 範囲内・ゴーストでない・退場していない・系に居る・固定でない・当たり判定が
// 有効・衝突モデルがあって形が 1 つ以上ある（Chrono 側に形が無い物 = 例えば
// 円柱の口の無い版で円柱の部品だけのフレーム・退化した凸包は、他の何とも
// 当たらないのでテレインにも当てない = 安全網も作らない）。
bool PhysicsWorld::terrainEligible(std::size_t id) const {
    if (id >= bodies_.size() || id >= active_.size()) return false;
    if (terrain_ && id == terrain_->ghost) return false;
    const ChBody* b = bodies_[id].get();
    if (!active_[id] || !b->GetSystem() || b->IsFixed() || !b->IsCollisionEnabled())
        return false;
    const auto model = b->GetCollisionModel();
    return model && !model->GetShapeInstances().empty();
}

PhysicsWorld::TerrainContactShape PhysicsWorld::terrainContactShape(std::size_t id) const {
    if (!terrainEligible(id)) return TerrainContactShape::None;
    return hasTerrainProxy(id) ? TerrainContactShape::Exact : TerrainContactShape::BoundingBox;
}

// 安全網: 形の記録の無い動くボディに、衝突モデルの AABB の 8 角で当てる記録を
// 作る（中身の点は毎ステップ collectTerrainContacts が入れ直す）。
PhysicsWorld::TerrainProxyRec* PhysicsWorld::terrainFallbackRec(std::size_t id) {
    if (id >= terrainProxies_.size()) return nullptr;
    if (!terrainProxies_[id]) {
        auto rec = std::make_shared<TerrainProxyRec>();
        rec->fallback = true;
        wterrain::ShapeProxy sp;
        sp.kind = wterrain::ShapeProxy::Kind::Points;
        sp.points.assign(8, wterrain::Vec3::Zero());
        rec->proxy.shapes.push_back(sp);
        rec->cache.assign(8, std::array<float, 6>{0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
        rec->was.assign(8, 0);
        rec->now.assign(8, 0);
        terrainProxies_[id] = std::move(rec);
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGW("physics",
                 "terrain contact: body %zu has no terrain shape record - using its "
                 "collision bounding box (coarse; later bodies like this are not logged)",
                 id);
        }
    }
    return terrainProxies_[id].get();
}

void PhysicsWorld::refreshTerrainMaterials() {
    if (!terrain_ || !terrain_->matRound || !terrain_->matFlat) return;
    TerrainState& t = *terrain_;
    applySurface(*t.matRound, t.options);
    applySurface(*t.matFlat, t.options);
    // 多点で触れる形には転がり・スピン摩擦を効かせない（SMC の材質には欄が無い）。
    if (auto* n = dynamic_cast<ChContactMaterialNSC*>(t.matFlat.get())) {
        n->SetRollingFriction(0.0f);
        n->SetSpinningFriction(0.0f);
    }
}

std::size_t PhysicsWorld::setTerrain(std::shared_ptr<const wterrain::HeightField> field,
                                     const BodyOptions& terrainOptions) {
    if (!field) {
        clearTerrain();
        return kInvalidId;
    }
    if (backend_ == PhysicsBackend::Multicore || !terrainCallback_) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGW("physics", "terrain contact needs the core backend - terrain is not collidable");
        }
        return kInvalidId;
    }
    if (!terrain_) terrain_ = std::make_shared<TerrainState>();
    TerrainState& t = *terrain_;
    if (t.ghost == kInvalidId) {
        auto ghost = chrono_types::make_shared<ChBody>();
        ghost->SetFixed(true);
        // 空の衝突モデル: ChCollisionInfo の modelB（GetContactable の元）に要る。
        // EnableCollision は呼ばない = Bullet に登録されない。
        ghost->AddCollisionModel(chrono_types::make_shared<ChCollisionModel>());
        sys_->AddBody(ghost);
        registerBody(ghost);
        t.ghost = bodies_.size() - 1;
    }
    t.field = std::move(field);
    t.contactTerrain = wterrain::ContactTerrain(*t.field);
    t.options = terrainOptions;
    if (!t.matRound || !t.matFlat) {
        t.matRound = makeMaterial();
        t.matFlat = makeMaterial();
    }
    refreshTerrainMaterials();

    // 地面が変わったので、前の地面に対する接触の印と眠りを捨てる。
    std::size_t bodies = 0, points = 0;
    for (std::size_t i = 0; i < terrainProxies_.size(); ++i) {
        const auto& rec = terrainProxies_[i];
        if (!rec) continue;
        std::fill(rec->was.begin(), rec->was.end(), char(0));
        if (!active_[i]) continue;
        ++bodies;
        points += rec->cache.size();
    }
    wakeAll();
    LOGI("physics",
         "terrain contact: ghost body %zu, %d x %d cells, %zu bodies with a proxy (%zu points)",
         t.ghost, t.field->resolution(), t.field->resolution(), bodies, points);
    return t.ghost;
}

void PhysicsWorld::setTerrainFrame(const ChVector3d& origin, const ChQuaternion<>& rot) {
    ChQuaternion<> q = rot;
    const double n = q.Length();
    q = (std::isfinite(n) && n > 1e-12) ? q * (1.0 / n) : ChQuaternion<>(1, 0, 0, 0);
    const ChVector3d o = (std::isfinite(origin.x()) && std::isfinite(origin.y()) &&
                          std::isfinite(origin.z()))
                             ? origin
                             : ChVector3d(0, 0, 0);
    if (o == terrainFrameOrigin_ && q == terrainFrameRot_) return;
    terrainFrameOrigin_ = o;
    terrainFrameRot_ = q;
    terrainFrameIdentity_ = o == ChVector3d(0, 0, 0) && q == ChQuaternion<>(1, 0, 0, 0);
    if (!hasTerrain()) return;
    // 地面が動いたので、前の位置に対する接触の印と眠りを捨てる（setTerrain と同じ）。
    for (const auto& rec : terrainProxies_) {
        if (rec) std::fill(rec->was.begin(), rec->was.end(), char(0));
    }
    wakeAll();
}

void PhysicsWorld::clearTerrain() {
    if (!terrain_ || !terrain_->field) return;
    terrain_->field.reset();
    terrain_->contactTerrain = wterrain::ContactTerrain();
    for (const auto& rec : terrainProxies_) {
        if (rec) std::fill(rec->was.begin(), rec->was.end(), char(0));
    }
    // 載っていた物が浮いたまま眠らないように起こす。
    wakeAll();
    LOGI("physics", "terrain contact cleared (ghost body %zu kept)", terrain_->ghost);
}

bool PhysicsWorld::hasTerrain() const {
    return terrain_ && terrain_->field && terrain_->ghost < bodies_.size();
}

std::size_t PhysicsWorld::terrainBodyId() const {
    return hasTerrain() ? terrain_->ghost : kInvalidId;
}

void PhysicsWorld::collectTerrainContacts(ChSystem& sys) {
    // テレインが無い（setTerrain されていない・clearTerrain 後）なら何もしない。
    if (!hasTerrain()) return;
    TerrainState& t = *terrain_;
    const auto container = sys.GetContactContainer();
    const auto& ghostBody = bodies_[t.ghost];
    ChCollisionModel* ghostModel = ghostBody->GetCollisionModel().get();
    if (!container || !ghostModel) return;
    t.params.dt = sys.GetStep();

    // テレインのフレーム（格子のローカル → ワールド）。ボディの姿勢と速度を
    // ローカルへ直して格子に問い合わせ、接触の点と法線をワールドへ戻す。
    const bool framed = !terrainFrameIdentity_;
    const wterrain::Mat3 frameRot = toTerrainMat(ChMatrix33<>(terrainFrameRot_));
    const wterrain::Vec3 frameOrigin = toTerrainVec(terrainFrameOrigin_);

    const std::size_t n = std::min(terrainProxies_.size(), bodies_.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (i == t.ghost) continue;
        TerrainProxyRec* rec = terrainProxies_[i].get();
        ChBody* b = bodies_[i].get();
        // 対象外（terrainEligible）と眠っている物は足さない（眠っている物は
        // 足しても AddContact が捨てる）。次に起きたら「前の接触なし」から。
        if (!terrainEligible(i) || b->IsSleeping()) {
            if (rec) std::fill(rec->was.begin(), rec->was.end(), char(0));
            continue;
        }
        ChCollisionModel* model = b->GetCollisionModel().get();
        // 形の記録が無い動く物 = 安全網（AABB の 8 角）。
        if (!rec) rec = terrainFallbackRec(i);
        if (!rec) continue;

        wterrain::Pose pose;
        pose.position = toTerrainVec(b->GetPos());
        pose.rotation = toTerrainMat(b->GetRotMat());
        if (rec->fallback) {
            // 衝突モデルの AABB はワールド軸・絶対座標（ベンチ B1 で確認）。
            // 角をボディ原点からの相対で入れ、回転は単位にする（点がワールドの
            // 箱の角そのものになる）。順は箱の 8 角と同じ = pointId が安定。
            const ChAABB box = model->GetBoundingBox();
            if (box.IsInverted() || !std::isfinite(box.min.x()) || !std::isfinite(box.max.x())) {
                std::fill(rec->was.begin(), rec->was.end(), char(0));
                continue;
            }
            auto& pts = rec->proxy.shapes[0].points;
            int k = 0;
            for (int a = 0; a < 2; ++a)
                for (int c = 0; c < 2; ++c)
                    for (int e = 0; e < 2; ++e)
                        pts[std::size_t(k++)] =
                            wterrain::Vec3(a ? box.max.x() : box.min.x(), c ? box.max.y() : box.min.y(),
                                           e ? box.max.z() : box.min.z()) -
                            pose.position;
            rec->proxy.boundRadius = wterrain::computeBoundRadius(rec->proxy);
            pose.rotation = wterrain::Mat3::Identity();
        }
        wterrain::BodyVelocity vel;
        vel.linear = toTerrainVec(getLinVel(b, 0));
        vel.angular = toTerrainVec(getAngVel(b, 0));
        if (framed) {
            const wterrain::Mat3 inv = frameRot.transpose();
            pose.position = inv * (pose.position - frameOrigin);
            pose.rotation = inv * pose.rotation;
            vel.linear = inv * vel.linear;
            vel.angular = inv * vel.angular;
        }
        wterrain::collectContacts(t.contactTerrain, rec->proxy, pose, vel, t.params, t.contacts);
        if (framed) {
            for (wterrain::Contact& c : t.contacts) {
                c.onBody = frameRot * c.onBody + frameOrigin;
                c.onTerrain = frameRot * c.onTerrain + frameOrigin;
                c.normal = frameRot * c.normal;
            }
        }

        std::fill(rec->now.begin(), rec->now.end(), char(0));
        const auto& bodyMat = mats_[i] ? mats_[i] : mat_;
        for (const wterrain::Contact& c : t.contacts) {
            if (c.pointId < 0 || std::size_t(c.pointId) >= rec->cache.size()) continue;
            const std::size_t pid = std::size_t(c.pointId);
            ChCollisionInfo ci;
            ci.modelA = model;
            ci.modelB = ghostModel;
            ci.shapeA = nullptr;
            ci.shapeB = nullptr;
            ci.vpA = fromTerrainVec(c.onBody);
            ci.vpB = fromTerrainVec(c.onTerrain);
            ci.vN = -fromTerrainVec(c.normal);  // A（物）→ B（テレイン）
            ci.distance = c.gap;
            // 曲率半径（SMC だけが使う）: 球はその半径（Multicore の球対平面と同じ）、
            // それ以外は Chrono の既定値のまま（Bullet の接触も既定値を渡している）。
            if (c.round && c.shape >= 0 && std::size_t(c.shape) < rec->proxy.shapes.size()) {
                const double r = rec->proxy.shapes[std::size_t(c.shape)].radius;
                if (std::isfinite(r) && r > 0.0) ci.eff_radius = r;
            }
            if (!rec->was[pid]) rec->cache[pid].fill(0.0f);
            ci.reaction_cache = rec->cache[pid].data();
            rec->now[pid] = 1;
            container->AddContact(ci, bodyMat, c.round ? t.matRound : t.matFlat);
        }
        rec->was.swap(rec->now);
    }
}
