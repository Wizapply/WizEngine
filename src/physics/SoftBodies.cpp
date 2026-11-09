// ソフトボディ: 質点ばねの粒子群の組み立て・置き直し・姿勢の当てはめ・ばねの解法。

#include "physics/SoftBodies.h"
#include "physics/BodyFactory.h"
#include "physics/BodyRegistry.h"
#include "physics/ChronoCompat.h"
#include "physics/ContactMaterials.h"
#include "core/Log.h"
#include "core/MathConstants.h"
#include "terrain/TerrainContact.h"

#include <chrono/collision/ChCollisionModel.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChBodyEasy.h>

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <Eigen/SVD>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace wizengine {
namespace physics {

// soft_ は root の昇順（番号は増える一方で、add が末尾に足す）なので
// 二分探索で引く。剛体と代表でない粒子は名簿の判定で先に落ちる（探索しない）。
const SoftBodies::SoftBody* SoftBodies::find(std::size_t id) const {
    if (!bodies_.isGroupRoot(id)) return nullptr;
    const auto it = std::lower_bound(
        soft_.begin(), soft_.end(), id,
        [](const SoftBody& s, std::size_t root) { return s.root < root; });
    return (it != soft_.end() && it->root == id) ? &*it : nullptr;
}

SoftBodies::SoftBody* SoftBodies::find(std::size_t id) {
    return const_cast<SoftBody*>(static_cast<const SoftBodies*>(this)->find(id));
}

bool SoftBodies::isRoot(std::size_t id) const {
    return find(id) != nullptr;
}

std::size_t SoftBodies::particleCount(std::size_t id) const {
    const SoftBody* s = find(id);
    return s ? bodies_.members(s->root).size() : 0;
}

void SoftBodies::particlePositions(std::size_t id, std::vector<float>& out) const {
    out.clear();
    const SoftBody* s = find(id);
    if (!s) return;
    out.reserve(bodies_.members(s->root).size() * 3);
    for (const std::size_t p : bodies_.members(s->root)) {
        const ChVector3d v = bodies_.body(p)->GetPos();
        out.push_back(float(v.x()));
        out.push_back(float(v.y()));
        out.push_back(float(v.z()));
    }
}

std::size_t SoftBodies::add(const SoftBodySpec& spec, const ChVector3d& pos,
                            const ChQuaternion<>& rot) {
    if (spec.rest.empty()) return kInvalidId;

    SoftBody soft;
    soft.rest = spec.rest;
    soft.springs = spec.springs;
    soft.iterations = std::max(1, spec.iterations);
    soft.restCentroid = ChVector3d(0, 0, 0);
    for (const auto& r : soft.rest) soft.restCentroid += r;
    soft.restCentroid *= 1.0 / double(soft.rest.size());

    // 衝突ファミリは 8〜15 を順に使う（0〜7 は剛体の衝突レイヤ）。9 個目
    // 以降は番号を使い回すので、その組は互いに当たらない - 現実的な数では
    // 起きない。剛体側の nocollide でソフトボディを除外することはできない
    // （レイヤは 0〜7 だけ）。
    const int family = kCollisionLayerCount + int(soft_.size() % kCollisionLayerCount);
    const std::size_t softIndex = soft_.size();
    const double radius = std::max(spec.radius, 1e-4);
    const double volume = (4.0 / 3.0) * wizengine::kPi * radius * radius * radius;
    const double density = std::max(spec.particleMass, 1e-9) / volume;
    // 物性は粒子全部で 1 個の材質を共有する（専用が要るときだけ作る）。
    const auto mat = materials_.materialFor(spec.options);
    // 粒子のレイヤは家族番号で決まるので、剛体レイヤは付けない（nocollide と
    // 重力だけ効かせる）。
    BodyOptions particleOpts = spec.options;
    particleOpts.layer = 0;
    // テレインの接触: 粒子ごとに半径 radius の球（1 点接触 = round。材質は
    // 球と同じ「転がりあり」= 平らな床と粒子の接触と同じ扱い）。
    const wizengine::terrain::BodyProxy proxy = BodyFactory::sphereProxy(radius);

    std::vector<std::size_t> particles;
    particles.reserve(spec.rest.size());
    bool familyOk = true;
    for (std::size_t i = 0; i < spec.rest.size(); ++i) {
        auto b = chrono_types::make_shared<ChBodyEasySphere>(
            radius, density, /*visualize*/ false, /*collide*/ true, mat);
        b->SetPos(pos + rot.Rotate(spec.rest[i]));
        b->SetRot(rot);
        b->SetFixed(spec.fixed);
        b->EnableCollision(true);
        if (auto model = b->GetCollisionModel()) {
            // 同じソフトボディの粒子どうしを当てない（ばねで結んだ隣が接触で
            // 押し合うと硬さが二重になる）。版差は setFamilyOf / disallowFamily。
            // どちらも無い版では粒子どうしも当たる（半径をセルより小さくしてある
            // ので静止状態では触れない）。
            const bool ok = setFamilyOf(&*model, family, 0) && disallowFamily(&*model, family, 0);
            if (!ok) familyOk = false;
        }
        // 粒子は眠らせない: 一部だけ眠ると、ばねの相手が動いても起きずに
        // 形が固まる（起こすのは接触だけ、という Chrono の約束のため）。
        particles.push_back(
            factory_.finish(b, {particleOpts, mat, /*allowSleeping*/ false, &proxy}));
    }
    soft.root = particles.front();
    bodies_.defineGroup(soft.root, std::move(particles));
    soft_.push_back(std::move(soft));
    if (!familyOk && softIndex == 0) {
        LOGW("physics",
             "soft body: this Chrono has no collision family API - particles of "
             "the same body also collide with each other");
    }
    LOGI("physics", "soft body #%zu: %zu particles, %zu springs (root body %zu)",
         softIndex, spec.rest.size(), spec.springs.size(),
         soft_.back().root);
    return soft_.back().root;
}

void SoftBodies::place(std::size_t id, const ChVector3d& pos, const ChQuaternion<>& rot) {
    const SoftBody* soft = find(id);
    if (!soft) return;
    const auto particles = bodies_.members(soft->root);
    for (std::size_t i = 0; i < particles.size(); ++i) {
        auto& b = bodies_.body(particles[i]);
        b->SetPos(pos + rot.Rotate(soft->rest[i]));
        b->SetRot(rot);
        b->ForceToRest();
        wakeUp(b.get(), 0);
        bodies_.events().bodyPlaced(particles[i]);
    }
}

// 粒子群に「剛体だったらどこにあるか」を当てはめる。重心はそのまま平均、
// 回転は静止形状との相関行列の極分解（SVD で U V^T）= 最小二乗の回転。
// 選択の当たり判定・ギズモ・引っぱり線・保存される姿勢がこれを使う。
BodyTransform SoftBodies::transform(std::size_t id) const {
    const SoftBody* found = find(id);
    if (!found) return BodyTransform{0, 0, 0, 1, 0, 0, 0};
    const SoftBody& soft = *found;
    const auto particles = bodies_.members(soft.root);
    const std::size_t n = particles.size();
    ChVector3d c(0, 0, 0);
    for (const std::size_t p : particles) c += bodies_.body(p)->GetPos();
    c *= 1.0 / double(n);

    Eigen::Matrix3d A = Eigen::Matrix3d::Zero();
    for (std::size_t i = 0; i < n; ++i) {
        const ChVector3d p = bodies_.body(particles[i])->GetPos() - c;
        const ChVector3d q = soft.rest[i] - soft.restCentroid;
        const Eigen::Vector3d pv(p.x(), p.y(), p.z());
        const Eigen::Vector3d qv(q.x(), q.y(), q.z());
        A += pv * qv.transpose();
    }
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(A, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d U = svd.matrixU();
    const Eigen::Matrix3d V = svd.matrixV();
    Eigen::Matrix3d R = U * V.transpose();
    if (R.determinant() < 0.0) {  // 鏡映になったら最小の特異値の軸を反転
        U.col(2) *= -1.0;
        R = U * V.transpose();
    }
    const Eigen::Quaterniond q(R);
    // 粒子 ≈ pos + R * rest なので、pos = 重心 - R * 静止重心。
    const Eigen::Vector3d c0(soft.restCentroid.x(), soft.restCentroid.y(),
                             soft.restCentroid.z());
    const Eigen::Vector3d pos = Eigen::Vector3d(c.x(), c.y(), c.z()) - R * c0;
    return {pos.x(), pos.y(), pos.z(), q.w(), q.x(), q.y(), q.z()};
}

void SoftBodies::solveSprings(double dt) {
    for (auto& soft : soft_) solve(soft, dt);
}

// ばね 1 本を implicit Euler で解いて速度を直す（vehicle のクラッチと同じ
// 「陰解法の粘性要素」の考え方をばねに広げたもの）:
//   相対座標 x（伸び）、相対速度 v、換算質量 m とすると
//   m v' = v - dt (k (x + dt v') + c v')
//   → v' = (v - dt k x / m) / (1 + dt (c + dt k) / m)
// 1 本ずつは無条件安定で、ガウス・ザイデルで回すと網全体も暴れない。
// 位置はステップ開始時の値、速度は直しながら読む。
void SoftBodies::solve(SoftBody& soft, double dt) {
    if (!soft.active || dt <= 0.0 || soft.springs.empty()) return;
    const auto particles = bodies_.members(soft.root);
    const std::size_t n = particles.size();
    std::vector<ChVector3d> pos(n), vel(n);
    std::vector<double> invMass(n, 0.0);
    bool anyFree = false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& b = bodies_.body(particles[i]);
        pos[i] = b->GetPos();
        vel[i] = getLinVel(b.get(), 0);
        const double m = b->GetMass();
        invMass[i] = (b->IsFixed() || m <= 0.0) ? 0.0 : 1.0 / m;
        if (invMass[i] > 0.0) anyFree = true;
    }
    if (!anyFree) return;  // 固定されたソフトボディは動かない

    for (int iter = 0; iter < soft.iterations; ++iter) {
        for (const auto& s : soft.springs) {
            const std::size_t a = s.a, b = s.b;
            if (a >= n || b >= n) continue;
            const double w = invMass[a] + invMass[b];
            if (w <= 0.0) continue;
            const ChVector3d d = pos[b] - pos[a];
            const double len = d.Length();
            if (len < 1e-9) continue;
            const ChVector3d dir = d * (1.0 / len);
            const double meff = 1.0 / w;
            const double vrel = (vel[b] - vel[a]).Dot(dir);
            const double x = len - s.rest;
            const double denom = 1.0 + dt * (s.c + dt * s.k) / meff;
            const double vNew = (vrel - dt * s.k * x / meff) / denom;
            const double impulse = meff * (vNew - vrel);
            vel[a] -= dir * (impulse * invMass[a]);
            vel[b] += dir * (impulse * invMass[b]);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (invMass[i] <= 0.0) continue;
        setLinVel(bodies_.body(particles[i]).get(), vel[i], 0);
    }
}

void SoftBodies::clear() {
    soft_.clear();
}

void SoftBodies::onBodyRetiring(std::size_t id) {
    // 粒子が退場したら、ばねも解かない（代表の退場で判定。disableBody は
    // 全粒子をまとめて退場させる）。
    if (SoftBody* soft = find(id)) soft->active = false;
}

}  // namespace physics
}  // namespace wizengine
