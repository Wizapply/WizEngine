#include "components/EffectsComponent.h"

#include <algorithm>
#include <cmath>

#include "physics/PhysicsWorld.h"
#include "scene/GameObject.h"
#include "scene/Scene.h"
#include "scene/SceneMath.h"

namespace ed = wizengine::editor;
using scenemath::Quat;
using scenemath::Vec3;

namespace {

// 物理スレッドから描画へ渡す粒子の溜まりの上限（描画が止まっている間に
// 溢れないように。超えたぶんは古い方から捨てる）。
constexpr std::size_t kMaxQueued = 32768;
// 点光源の明るさが最大になる「生きている粒子の量」（明るさの重みの合計）。
constexpr double kLightFullWeight = 6.0;

// エミッタの範囲の座標系（付け先の姿勢 × エミッタのローカル）をワールドで。
struct Frame {
    Vec3 origin{0, 0, 0};    // 範囲の底面の中心（球なら中心）
    Quat q{1, 0, 0, 0};      // 範囲の向き（ローカル +Y = 円柱の軸）
    Vec3 bodyP{0, 0, 0};     // 付け先の位置（速度の腕の長さを測る基準）
    Vec3 v{0, 0, 0}, w{0, 0, 0};
};

Frame frameOf(const ed::EmitterDesc& e, const BodyTransform* body, const Vec3& v,
              const Vec3& w) {
    Frame f;
    Quat qb(1, 0, 0, 0);
    Vec3 pb(0, 0, 0);
    if (body) {
        qb = Quat(body->qw, body->qx, body->qy, body->qz);
        pb = Vec3(body->px, body->py, body->pz);
    }
    const Quat ql = scenemath::quatFromEulerDegrees(e.rotation.x, e.rotation.y, e.rotation.z);
    f.origin = pb + qb * Vec3(e.position.x, e.position.y, e.position.z);
    f.q = qb * ql;
    f.bodyP = pb;
    f.v = v;
    f.w = w;
    return f;
}

}  // namespace

void EffectsComponent::setRate(int emitter, double rate) {
    if (emitter < 0) return;
    const std::size_t i = std::size_t(emitter);
    if (i >= rateOverride_.size()) {
        rateOverride_.resize(i + 1, 0.0);
        overrideSet_.resize(i + 1, 0);
    }
    rateOverride_[i] = std::isfinite(rate) ? std::max(0.0, rate) : 0.0;
    overrideSet_[i] = 1;
}

void EffectsComponent::onPhysicsStep(Scene& scene, double dt) {
    const std::uint64_t gen = scene.runGeneration();
    if (gen != physGen_) {
        physGen_ = gen;
        std::fill(carry_.begin(), carry_.end(), 0.0);
    }
    const std::uint64_t ver = scene.editor().emitterVersion();
    if (ver != physVersion_) {
        physVersion_ = ver;
        physEmitters_ = scene.editor().emitters();
        carry_.assign(physEmitters_.size(), 0.0);
    }
    const std::size_t n = physEmitters_.size();
    if (rateOverride_.size() < n) {
        rateOverride_.resize(n, 0.0);
        overrideSet_.resize(n, 0);
    }
    if (n == 0 || dt <= 0.0) {
        std::fill(overrideSet_.begin(), overrideSet_.end(), 0);
        return;
    }

    const double t0 = scene.physicsSimTime();
    PhysicsWorld& physics = scene.physics();
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    pending_.clear();
    for (std::size_t i = 0; i < n; ++i) {
        const ed::EmitterDesc& e = physEmitters_[i];
        const double rate = overrideSet_[i] ? rateOverride_[i] : e.rate;
        overrideSet_[i] = 0;
        carry_[i] += rate * dt;
        if (carry_[i] < 1.0) continue;
        double whole = std::floor(carry_[i]);
        carry_[i] -= whole;
        // 1 ステップに出しすぎない（上限を超えた分は捨てる。見た目の飽和）。
        whole = std::min(whole, double(e.max));
        const int count = int(whole);

        // 付け先のこのステップの姿勢と速度（物理スレッドは一覧の唯一の書き手）。
        BodyTransform bt{0, 0, 0, 1, 0, 0, 0};
        const BodyTransform* body = nullptr;
        Vec3 v(0, 0, 0), w(0, 0, 0);
        if (e.body >= 0 && std::size_t(e.body) < scene.objectCount()) {
            const GameObject& o = scene.object(std::size_t(e.body));
            if (o.alive && o.physId != GameObject::kInvalidId) {
                bt = physics.bodyTransform(o.physId);
                body = &bt;
                const auto bv = physics.bodyVelocity(o.physId);
                const auto bw = physics.bodyAngularVelocity(o.physId);
                v = Vec3(bv.x(), bv.y(), bv.z());
                w = Vec3(bw.x(), bw.y(), bw.z());
            }
        }
        const Frame f = frameOf(e, body, v, w);
        for (int k = 0; k < count; ++k) {
            // 範囲の中の点（ローカル）。円柱は面積で一様（半径に √）、球は体積で一様。
            Vec3 local;
            if (e.height > 0.0) {
                const double r = e.radius * std::sqrt(uni(rng_));
                const double a = 2.0 * EIGEN_PI * uni(rng_);
                local = Vec3(r * std::cos(a), e.height * uni(rng_), r * std::sin(a));
            } else {
                Vec3 d;
                do {
                    d = Vec3(2 * uni(rng_) - 1, 2 * uni(rng_) - 1, 2 * uni(rng_) - 1);
                } while (d.squaredNorm() > 1.0 || d.squaredNorm() < 1e-12);
                local = d * e.radius;
            }
            const Vec3 p = f.origin + f.q * local;
            // 付け先の速度（回転の腕 ω × r も）を受け継ぎ、向きのばらばらな速さを足す。
            Vec3 dir;
            do {
                dir = Vec3(2 * uni(rng_) - 1, 2 * uni(rng_) - 1, 2 * uni(rng_) - 1);
            } while (dir.squaredNorm() > 1.0 || dir.squaredNorm() < 1e-12);
            const Vec3 vel = e.inherit * (f.v + f.w.cross(p - f.bodyP)) +
                             dir.normalized() * (e.speed * uni(rng_));
            Spawn s;
            s.p[0] = float(p.x());
            s.p[1] = float(p.y());
            s.p[2] = float(p.z());
            s.v[0] = float(vel.x());
            s.v[1] = float(vel.y());
            s.v[2] = float(vel.z());
            // ステップの中で均等にばらす（高い回転数でも粒子が 1 か所に固まらない）。
            s.t0 = t0 + dt * (double(k) + 0.5) / double(count);
            s.emitter = int(i);
            s.gen = gen;
            pending_.push_back(s);
        }
    }
    if (pending_.empty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    spawned_.insert(spawned_.end(), pending_.begin(), pending_.end());
    if (spawned_.size() > kMaxQueued) {
        spawned_.erase(spawned_.begin(),
                       spawned_.begin() + std::ptrdiff_t(spawned_.size() - kMaxQueued));
    }
}

void EffectsComponent::onRender(Scene& scene) {
    wizengine::Renderer& r = scene.renderer();
    const std::uint64_t gen = scene.runGeneration();
    if (gen != renderGen_) {
        renderGen_ = gen;
        alive_.clear();
    }
    const std::uint64_t ver = scene.editor().emitterVersion();
    if (ver != renderVersion_) {
        renderVersion_ = ver;
        renderEmitters_ = scene.editor().emitters();
        alive_.clear();
        // 点光源を作り直す（数も色も届く距離も変わりうる）。
        for (std::size_t idx : lights_) {
            if (idx != std::size_t(-1)) r.removeLight(idx);
        }
        lights_.assign(renderEmitters_.size(), std::size_t(-1));
        std::size_t capacity = 0;
        for (std::size_t i = 0; i < renderEmitters_.size(); ++i) {
            const ed::EmitterDesc& e = renderEmitters_[i];
            capacity += std::size_t(e.max);
            if (e.light <= 0.0) continue;
            wizengine::LightDesc ld;
            ld.type = wizengine::LightDesc::Type::Point;
            ld.color = {e.color.r, e.color.g, e.color.b};
            ld.intensity = 0.0f;
            ld.castShadows = false;
            ld.falloffRadius = float(e.lightRange);
            lights_[i] = r.addLight(ld);
        }
        // 粒子の入れ物は必要な数が今より多いときだけ作る（減ったときは
        // 使い回す。Renderer に入れ物を壊す口は持たせていない）。
        if (capacity > meshCapacity_) {
            if (mesh_ != std::size_t(-1)) r.setParticles(mesh_, {});
            mesh_ = r.addParticleMesh(std::max<std::size_t>(capacity, 256));
            meshCapacity_ = std::max<std::size_t>(capacity, 256);
        }
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const Spawn& s : spawned_) {
            if (s.gen == gen && s.emitter >= 0 &&
                std::size_t(s.emitter) < renderEmitters_.size()) {
                alive_.push_back(s);
            }
        }
        spawned_.clear();
    }
    if (mesh_ == std::size_t(-1)) return;

    // 描くフレームのシミュレーション時刻で年齢を数える（補間した姿勢と揃う。
    // まだ描くフレームに届いていない粒子 = 年齢が負は待たせる）。
    const double now = scene.renderSimTime();
    std::vector<double> weight(renderEmitters_.size(), 0.0);
    scratch_.clear();
    std::size_t keep = 0;
    for (std::size_t k = 0; k < alive_.size(); ++k) {
        const Spawn& s = alive_[k];
        const ed::EmitterDesc& e = renderEmitters_[std::size_t(s.emitter)];
        const double age = now - s.t0;
        if (age > e.life) continue;  // 寿命が尽きた = 捨てる
        alive_[keep++] = s;
        if (age < 0.0 || scratch_.size() >= meshCapacity_) continue;
        const double a = age / e.life;  // 0 → 1
        // 生まれてすぐ立ち上がり（寿命の 8%）、その後なだらかに消える。
        const double fade = std::min(1.0, a / 0.08) * std::pow(1.0 - a, 1.5);
        const double k1 = e.intensity * fade;
        wizengine::Renderer::Particle p;
        p.position = {float(s.p[0] + s.v[0] * age), float(s.p[1] + s.v[1] * age),
                      float(s.p[2] + s.v[2] * age)};
        p.diameter = float(e.size * (1.0 + (e.grow - 1.0) * a));
        p.color = {float(k1 * (e.color.r + (e.color2.r - e.color.r) * a)),
                   float(k1 * (e.color.g + (e.color2.g - e.color.g) * a)),
                   float(k1 * (e.color.b + (e.color2.b - e.color.b) * a))};
        scratch_.push_back(p);
        weight[std::size_t(s.emitter)] += fade;
    }
    alive_.resize(keep);
    r.setParticles(mesh_, scratch_);

    // 点光源: 範囲の中心へ置き、生きている粒子の量で明るさを揺らす。
    for (std::size_t i = 0; i < lights_.size(); ++i) {
        if (lights_[i] == std::size_t(-1)) continue;
        const ed::EmitterDesc& e = renderEmitters_[i];
        BodyTransform bt{0, 0, 0, 1, 0, 0, 0};
        const BodyTransform* body = nullptr;
        if (e.body >= 0 && scene.latestPose(std::size_t(e.body), bt)) body = &bt;
        const Frame f = frameOf(e, body, Vec3(0, 0, 0), Vec3(0, 0, 0));
        const Vec3 c = f.origin + f.q * Vec3(0, 0.5 * e.height, 0);
        const double glow = std::min(1.0, weight[i] / kLightFullWeight);
        r.updateLight(lights_[i], {e.color.r, e.color.g, e.color.b},
                      float(e.light * glow), {0.0f, -1.0f, 0.0f},
                      {float(c.x()), float(c.y()), float(c.z())});
    }
}
