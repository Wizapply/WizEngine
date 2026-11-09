// テレイン: 高さ格子との自前の接触を Chrono の接触へ詰め替える。

#include "physics/TerrainContact.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/ContactMaterials.h"
#include "physics/PhysicsSystem.h"
#include "physics/TerrainConvert.h"
#include "core/Log.h"
#include "terrain/HeightField.h"
#include "terrain/TerrainContact.h"

#include <chrono/collision/ChCollisionInfo.h>
#include <chrono/collision/ChCollisionModel.h>
#include <chrono/core/ChMatrix33.h>
#include <chrono/geometry/ChGeometry.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChContactContainer.h>
#include <chrono/physics/ChContactMaterial.h>
#include <chrono/physics/ChContactMaterialNSC.h>
#include <chrono/physics/ChSystem.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wterrain = wizengine::terrain;

namespace {

// テレインの接触の判定の設定 = terrain::ContactParams の既定値（ベンチ B1 / B2 の
// 推薦値）。pointId（反力キャッシュの添字）は faceSamples で変わるので、proxy の
// 点数を数える側（setProxy）と判定する側（collectContacts）は必ずこの同じ値を使う
// （食い違うとキャッシュの範囲外の接触が黙って捨てられ、物が沈む）。
const wterrain::ContactParams kTerrainContactParams{};

}  // namespace

namespace wizengine {
namespace physics {

// ボディ 1 個の形の記録と、点ごとの反力キャッシュ（Chrono の
// ChCollisionInfo::reaction_cache へアドレスを渡す。接触は毎ステップ作り直される
// ので、ウォームスタートはこれ頼み）。was = 前のステップにその点が接触したか
// （していなければ、今回渡す前にキャッシュを 0 に戻す = 古い反力で始めない）。
// fallback = 形の記録を作れなかった動くボディの安全網（collectContacts が
// 初めて見たときに作る）。proxy は 8 点の Points で、毎ステップ衝突モデルの
// AABB（ワールド軸、絶対座標）の 8 角をボディ原点からの相対で入れ直す。
struct TerrainContact::ProxyRec {
    wterrain::BodyProxy proxy;
    std::vector<std::array<float, 6>> cache;  // 点ごとの永続 float[6]（再確保しない）
    std::vector<char> was;
    std::vector<char> now;  // 作業用（was と入れ替える）
    bool fallback = false;
};

struct TerrainContact::State {
    std::shared_ptr<const wterrain::HeightField> field;  // null = 接触しない
    wterrain::ContactTerrain contactTerrain;             // field から作った早期棄却の値
    std::size_t ghost = kInvalidId;                      // 接触の相手のボディ（physId）
    BodyOptions options;                                 // テレイン側の物性
    // 丸い形（Contact::round = 球）との接触 = options のまま。それ以外 = 転がり・
    // スピン摩擦 0（多点で触れる箱が BB ソルバで斜面に沈むため。ベンチ B1 / B2）。
    std::shared_ptr<chrono::ChContactMaterial> matRound;
    std::shared_ptr<chrono::ChContactMaterial> matFlat;
    // 判定の設定（kTerrainContactParams の写し）。dt だけ毎ステップ書く。
    wterrain::ContactParams params = kTerrainContactParams;
    std::vector<wterrain::Contact> contacts;  // 作業用（ボディ 1 個ぶん）
};

// ChSystem::ComputeCollisions の末尾（Bullet の接触を足した後）から呼ばれる。
// TerrainContact は系より先に壊れるので、壊れるときに owner を null にする。
struct TerrainContact::Callback : public chrono::ChSystem::CustomCollisionCallback {
    explicit Callback(TerrainContact* o) : owner(o) {}
    void OnCustomCollision(chrono::ChSystem* sys) override {
        if (owner && sys) owner->collectContacts(*sys);
    }
    TerrainContact* owner = nullptr;
};

TerrainContact::~TerrainContact() { dropCallback(); }

void TerrainContact::dropCallback() {
    if (callback_) callback_->owner = nullptr;
    callback_.reset();
}

void TerrainContact::attach() {
    dropCallback();
    if (system_.backend() != PhysicsBackend::Multicore) {
        callback_ = std::make_shared<Callback>(this);
        system_.sys()->RegisterCustomCollisionCallback(callback_);
    }
}

void TerrainContact::onBodiesCleared() { proxies_.clear(); }

void TerrainContact::clear() {
    state_.reset();
    dropCallback();
}

void TerrainContact::onBodyAdded(std::size_t id, const BodyAddInfo& info) {
    proxies_.emplace_back();  // 無ければ対象外
    if (info.proxy) setProxy(id, *info.proxy);
}

void TerrainContact::onBodyPlaced(std::size_t id) {
    forgetContacts(id);  // 置き直した = 前の場所での反力は使わない
}

void TerrainContact::onWakeAll() {
    // テレインの反力キャッシュも捨てる（シミュレートの開始・Reset で呼ばれる。
    // 前回の走行の最後の反力で次の走行の最初のステップを始めない）。
    for (std::size_t i = 0; i < proxies_.size(); ++i) forgetContacts(i);
}

void TerrainContact::onMaterialsChanged() { refreshMaterials(); }

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
// null）。AddBody と名簿の登録は通す（接触の相手の変数・逆引きに要る）。
// 追加のときに BodyFactory::finish は通さない（元から追加時の眠りの設定と
// prepareBody を通っていない）。固定なので眠らない。

void TerrainContact::setProxy(std::size_t id, const wterrain::BodyProxy& proxy) {
    if (id >= proxies_.size()) return;
    auto rec = std::make_shared<ProxyRec>();
    rec->proxy = proxy;
    rec->proxy.boundRadius = wterrain::computeBoundRadius(rec->proxy);
    const int points = wterrain::proxyPointCount(rec->proxy, kTerrainContactParams);
    if (points <= 0) {
        proxies_[id].reset();
        return;
    }
    // ここで 1 回だけ確保する（以後は大きさを変えない = アドレスが動かない）。
    rec->cache.assign(std::size_t(points), std::array<float, 6>{0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
    rec->was.assign(std::size_t(points), 0);
    rec->now.assign(std::size_t(points), 0);
    proxies_[id] = std::move(rec);
}

void TerrainContact::forgetContacts(std::size_t id) {
    if (id >= proxies_.size() || !proxies_[id]) return;
    ProxyRec& rec = *proxies_[id];
    std::fill(rec.was.begin(), rec.was.end(), char(0));
    for (auto& c : rec.cache) c.fill(0.0f);
}

bool TerrainContact::hasProxy(std::size_t id) const {
    return id < proxies_.size() && proxies_[id] && !proxies_[id]->fallback;
}

bool TerrainContact::isGhost(std::size_t id) const { return state_ && id == state_->ghost; }

// テレインと接触しうるボディか（眠りは見ない = 一時的なので）。
// contactShape と collectContacts の共通の条件はここ 1 か所:
// 範囲内・ゴーストでない・退場していない・系に居る・固定でない・当たり判定が
// 有効・衝突モデルがあって形が 1 つ以上ある（Chrono 側に形が無い物 = 例えば
// 円柱の口の無い版で円柱の部品だけのフレーム・退化した凸包は、他の何とも
// 当たらないのでテレインにも当てない = 安全網も作らない）。
bool TerrainContact::eligible(std::size_t id) const {
    if (id >= bodies_.count()) return false;
    if (isGhost(id)) return false;
    const ChBody* b = bodies_.body(id).get();
    if (!bodies_.active(id) || !b->GetSystem() || b->IsFixed() || !b->IsCollisionEnabled())
        return false;
    const auto model = b->GetCollisionModel();
    return model && !model->GetShapeInstances().empty();
}

TerrainContactShape TerrainContact::contactShape(std::size_t id) const {
    if (!eligible(id)) return TerrainContactShape::None;
    return hasProxy(id) ? TerrainContactShape::Exact : TerrainContactShape::BoundingBox;
}

// 安全網: 形の記録の無い動くボディに、衝突モデルの AABB の 8 角で当てる記録を
// 作る（中身の点は毎ステップ collectContacts が入れ直す）。
TerrainContact::ProxyRec* TerrainContact::fallbackRec(std::size_t id) {
    if (id >= proxies_.size()) return nullptr;
    if (!proxies_[id]) {
        auto rec = std::make_shared<ProxyRec>();
        rec->fallback = true;
        wterrain::ShapeProxy sp;
        sp.kind = wterrain::ShapeProxy::Kind::Points;
        sp.points.assign(8, wterrain::Vec3::Zero());
        rec->proxy.shapes.push_back(sp);
        rec->cache.assign(8, std::array<float, 6>{0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
        rec->was.assign(8, 0);
        rec->now.assign(8, 0);
        proxies_[id] = std::move(rec);
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGW("physics",
                 "terrain contact: body %zu has no terrain shape record - using its "
                 "collision bounding box (coarse; later bodies like this are not logged)",
                 id);
        }
    }
    return proxies_[id].get();
}

void TerrainContact::refreshMaterials() {
    if (!state_ || !state_->matRound || !state_->matFlat) return;
    State& t = *state_;
    materials_.applySurface(*t.matRound, t.options);
    materials_.applySurface(*t.matFlat, t.options);
    // 多点で触れる形には転がり・スピン摩擦を効かせない（SMC の材質には欄が無い）。
    if (auto* n = dynamic_cast<ChContactMaterialNSC*>(t.matFlat.get())) {
        n->SetRollingFriction(0.0f);
        n->SetSpinningFriction(0.0f);
    }
}

std::size_t TerrainContact::set(std::shared_ptr<const wterrain::HeightField> field,
                                const BodyOptions& options) {
    if (system_.backend() == PhysicsBackend::Multicore || !callback_) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGW("physics", "terrain contact needs the core backend - terrain is not collidable");
        }
        return kInvalidId;
    }
    if (!state_) state_ = std::make_shared<State>();
    State& t = *state_;
    if (t.ghost == kInvalidId) {
        auto ghost = chrono_types::make_shared<ChBody>();
        ghost->SetFixed(true);
        // 空の衝突モデル: ChCollisionInfo の modelB（GetContactable の元）に要る。
        // EnableCollision は呼ばない = Bullet に登録されない。
        ghost->AddCollisionModel(chrono_types::make_shared<ChCollisionModel>());
        system_.sys()->AddBody(ghost);
        t.ghost = bodies_.add(ghost, BodyAddInfo{BodyOptions{}, nullptr, /*allowSleeping*/ true});
    }
    t.field = std::move(field);
    t.contactTerrain = wterrain::ContactTerrain(*t.field);
    t.options = options;
    if (!t.matRound || !t.matFlat) {
        t.matRound = materials_.make();
        t.matFlat = materials_.make();
    }
    refreshMaterials();

    // 前の地面に対する接触の印と眠りは呼び出し側が捨てる（窓口の wakeAll →
    // onWakeAll → forgetContacts）。ここは記録のある物を数えるだけ。
    std::size_t bodies = 0, points = 0;
    for (std::size_t i = 0; i < proxies_.size(); ++i) {
        const auto& rec = proxies_[i];
        if (!rec) continue;
        if (!bodies_.active(i)) continue;
        ++bodies;
        points += rec->cache.size();
    }
    LOGI("physics",
         "terrain contact: ghost body %zu, %d x %d cells, %zu bodies with a proxy (%zu points)",
         t.ghost, t.field->resolution(), t.field->resolution(), bodies, points);
    return t.ghost;
}

bool TerrainContact::setFrame(const ChVector3d& origin, const ChQuaternion<>& rot) {
    ChQuaternion<> q = rot;
    const double n = q.Length();
    q = (std::isfinite(n) && n > 1e-12) ? q * (1.0 / n) : ChQuaternion<>(1, 0, 0, 0);
    const ChVector3d o = (std::isfinite(origin.x()) && std::isfinite(origin.y()) &&
                          std::isfinite(origin.z()))
                             ? origin
                             : ChVector3d(0, 0, 0);
    if (o == frameOrigin_ && q == frameRot_) return false;
    frameOrigin_ = o;
    frameRot_ = q;
    frameIdentity_ = o == ChVector3d(0, 0, 0) && q == ChQuaternion<>(1, 0, 0, 0);
    if (!hasTerrain()) return false;
    // 地面が動いた。前の位置に対する接触の印と眠りは呼び出し側が捨てる（set と同じ）。
    return true;
}

bool TerrainContact::clearField() {
    if (!state_ || !state_->field) return false;
    state_->field.reset();
    state_->contactTerrain = wterrain::ContactTerrain();
    LOGI("physics", "terrain contact cleared (ghost body %zu kept)", state_->ghost);
    // 載っていた物が浮いたまま眠らないように起こす・接触の印を捨てる（呼び出し側）。
    return true;
}

bool TerrainContact::hasTerrain() const {
    return state_ && state_->field && state_->ghost < bodies_.count();
}

std::size_t TerrainContact::bodyId() const {
    return hasTerrain() ? state_->ghost : kInvalidId;
}

void TerrainContact::collectContacts(ChSystem& sys) {
    // テレインが無い（set されていない・clearField 後）なら何もしない。
    if (!hasTerrain()) return;
    State& t = *state_;
    const auto container = sys.GetContactContainer();
    const auto& ghostBody = bodies_.body(t.ghost);
    ChCollisionModel* ghostModel = ghostBody->GetCollisionModel().get();
    if (!container || !ghostModel) return;
    t.params.dt = sys.GetStep();

    // テレインのフレーム（格子のローカル → ワールド）。ボディの姿勢と速度を
    // ローカルへ直して格子に問い合わせ、接触の点と法線をワールドへ戻す。
    const bool framed = !frameIdentity_;
    const wterrain::Mat3 frameRot = toTerrainMat(ChMatrix33<>(frameRot_));
    const wterrain::Vec3 frameOrigin = toTerrainVec(frameOrigin_);

    const std::size_t n = std::min(proxies_.size(), bodies_.count());
    for (std::size_t i = 0; i < n; ++i) {
        ProxyRec* rec = proxies_[i].get();
        ChBody* b = bodies_.body(i).get();
        // 対象外（eligible）と眠っている物は足さない（眠っている物は
        // 足しても AddContact が捨てる）。次に起きたら「前の接触なし」から。
        if (!eligible(i) || b->IsSleeping()) {
            if (rec) std::fill(rec->was.begin(), rec->was.end(), char(0));
            continue;
        }
        ChCollisionModel* model = b->GetCollisionModel().get();
        // 形の記録が無い動く物 = 安全網（AABB の 8 角）。
        if (!rec) rec = fallbackRec(i);
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
        const auto& bodyMat = materials_.materialOf(i);
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

}  // namespace physics
}  // namespace wizengine
