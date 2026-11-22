#include "components/FluidComponent.h"

#include <algorithm>
#include <cmath>

#include "fluid/FluidSeed.h"
#include "fluid/WaterForces.h"
#include "physics/PhysicsWorld.h"
#include "render/Renderer.h"
#include "scene/GameObject.h"
#include "scene/Scene.h"

namespace ed = wizengine::editor;
namespace fl = wizengine::fluid;

namespace {

constexpr std::size_t kNone = std::size_t(-1);

// 文書の水面 → 純粋な計算の語彙。
fl::WaterParams toParams(const ed::WaterDesc& w) {
    fl::WaterParams p;
    p.centerX = w.position.x;
    p.centerZ = w.position.z;
    p.level = w.position.y;
    p.halfX = w.halfX;
    p.halfZ = w.halfZ;
    p.density = w.density;
    p.drag = w.drag;
    p.linearDrag = w.linearDrag;
    p.angularDrag = w.angularDrag;
    p.flow = {w.flow.x, w.flow.y, w.flow.z};
    p.amplitude = w.wave;
    p.period = w.period;
    p.heading = w.heading * fl::kPi / 180.0;
    p.wind = w.wind;
    p.spread = w.spread * fl::kPi / 180.0;
    p.waveCount = w.waves;
    p.seed = unsigned(w.seed);
    p.choppy = w.choppy;
    return p;
}

chrono::ChVector3d toCh(const fl::Vec3& v) { return {v.x, v.y, v.z}; }
fl::Vec3 fromCh(const chrono::ChVector3d& v) { return {v.x(), v.y(), v.z()}; }

// 重心まわりのモーメント tau を、重心の両側の点に掛ける逆向きの力の対にする
// （PhysicsWorld にトルクの口が無いので。制御器の torqueAsCouple と同じ作り）。
void applyTorque(PhysicsWorld& physics, std::size_t id, const fl::Vec3& center,
                 const fl::Vec3& tau, double arm, double dt) {
    const double t = tau.length();
    if (t < 1e-12 || arm <= 0.0) return;
    const fl::Vec3 n = tau / t;
    const fl::Vec3 ref = std::fabs(n.x) < 0.9 ? fl::Vec3{1.0, 0.0, 0.0} : fl::Vec3{0.0, 1.0, 0.0};
    fl::Vec3 a = n.cross(ref);
    a = a / a.length();
    const fl::Vec3 b = n.cross(a);  // a × b = n
    const fl::Vec3 offset = a * arm;
    const fl::Vec3 force = b * (t / (2.0 * arm));  // 2 arm (a × F) = τ
    physics.applyForceAtPoint(id, toCh(force), toCh(center + offset), dt);
    physics.applyForceAtPoint(id, toCh(-force), toCh(center - offset), dt);
}

}  // namespace

// ---- PHYSICS: 浮力と抗力 --------------------------------------------------------

void FluidComponent::onPhysicsStep(Scene& scene, double dt) {
    const std::uint64_t ver = scene.editor().waterVersion();
    const ed::SimSettings sim = scene.editor().sim();
    const fl::Vec3 gravity{sim.gravityX, sim.gravity, sim.gravityZ};
    if (ver != physVersion_ || gravity.length() != physGravity_) {
        physVersion_ = ver;
        physHas_ = scene.editor().hasWater();
        physWater_ = scene.editor().water();
        physGravity_ = gravity.length();
        physParams_ = toParams(physWater_);
        fl::prepareWaves(physParams_, physGravity_);
    }
    if (!physHas_ || !physWater_.enabled || dt <= 0.0) return;

    const fl::WaterParams& wp = physParams_;
    const double t = scene.physicsSimTime();
    PhysicsWorld& physics = scene.physics();

    // 物理スレッドは一覧の唯一の書き手なので、ロックを取らずに読む。
    for (std::size_t i = 0; i < scene.objectCount(); ++i) {
        const GameObject& o = scene.object(i);
        if (!o.alive || o.physId == GameObject::kInvalidId) continue;
        const ed::BodyDesc& d = o.desc;
        if (d.fixed || physics.bodyFixed(o.physId)) continue;

        fl::BodyState b;
        if (d.shape == ed::ShapeKind::Box) {
            b.shape = fl::BodyState::Shape::Box;
            b.half = {0.5 * d.size.x, 0.5 * d.size.y, 0.5 * d.size.z};
        } else if (d.shape == ed::ShapeKind::Sphere || d.shape == ed::ShapeKind::Model) {
            // 凸包のモデルは geom の半径の球（物理の原点 = 凸包の体積重心）。
            b.shape = fl::BodyState::Shape::Sphere;
            b.radius = 0.5 * d.size.x;
        } else {
            continue;  // geom の無いフレーム・三角メッシュ（形が分からない）
        }

        const BodyTransform tr = physics.bodyTransform(o.physId);
        b.position = {tr.px, tr.py, tr.pz};
        b.rotation = fl::Quat(tr.qw, tr.qx, tr.qy, tr.qz);
        // 水の範囲から十分離れた物は計算しない（ほとんどの物はここで抜ける）。
        const double reach = b.shape == fl::BodyState::Shape::Sphere
                                 ? b.radius
                                 : std::sqrt(b.half.dot(b.half));
        const double waveTop = wp.level + wp.crest;
        if (b.position.y - reach > waveTop) continue;
        if (std::fabs(b.position.x - wp.centerX) > wp.halfX + reach ||
            std::fabs(b.position.z - wp.centerZ) > wp.halfZ + reach) {
            continue;
        }
        b.velocity = fromCh(physics.bodyVelocity(o.physId));
        b.angularVelocity = fromCh(physics.bodyAngularVelocity(o.physId));
        b.mass = physics.bodyMass(o.physId);

        const fl::WaterLoad load = fl::computeWaterLoad(wp, b, gravity, t, dt);
        if (load.submergedVolume <= 0.0) continue;
        if (o.lattice) {
            // ソフトボディ: 全粒子へ等しく（回すモーメントは掛けない）。
            physics.applyForce(o.physId, toCh(load.force), dt);
            continue;
        }
        physics.applyForceAtPoint(o.physId, toCh(load.force), toCh(b.position), dt);
        applyTorque(physics, o.physId, b.position, load.torque, std::max(0.05, reach), dt);
    }
}

// ---- RENDER: 水面と流体粒子 ------------------------------------------------------

void FluidComponent::onRender(Scene& scene) {
    wizengine::Renderer& r = scene.renderer();
    const ed::SimSettings simNow = scene.editor().sim();
    // 波の成分は物理と同じ重力の大きさで作る（同じ波を描く）。
    const double gravity = fl::Vec3{simNow.gravityX, simNow.gravity, simNow.gravityZ}.length();

    // ---- 水面 ----
    const std::uint64_t wv = scene.editor().waterVersion();
    if (wv != waterVersion_ || gravity != waterGravity_) {
        waterVersion_ = wv;
        waterGravity_ = gravity;
        waterHas_ = scene.editor().hasWater();
        water_ = scene.editor().water();
        waterParams_ = toParams(water_);
        fl::prepareWaves(waterParams_, gravity);
        const bool show = waterHas_ && water_.enabled;
        if (waterMesh_ != kNone && (!show || water_.cells != waterCells_)) {
            r.removeWaterMesh(waterMesh_);
            waterMesh_ = kNone;
        }
        if (show && waterMesh_ == kNone) {
            waterCells_ = water_.cells;
            const std::vector<unsigned> idx = fl::waterGridIndices(waterCells_);
            const std::size_t verts = std::size_t(waterCells_ + 1) * std::size_t(waterCells_ + 1);
            waterMesh_ = r.addWaterMesh(verts, std::vector<uint32_t>(idx.begin(), idx.end()));
        }
        if (waterMesh_ != kNone) {
            wizengine::Renderer::WaterStyle st;
            st.color = {water_.color.r, water_.color.g, water_.color.b};
            // 透けた光の色付き: 深い水の色の向き（いちばん強い成分で割った比）を、
            // 透明度の距離で通したときにその比になる吸収係数（Beer-Lambert）。
            const float cmax = std::max({water_.color.r, water_.color.g, water_.color.b, 1e-4f});
            const float cl = float(water_.clarity);
            auto absorb = [cmax, cl](float c) {
                return -std::log(std::clamp(c / cmax, 0.05f, 1.0f)) / std::max(0.01f, cl);
            };
            st.absorption = {absorb(water_.color.r), absorb(water_.color.g),
                             absorb(water_.color.b)};
            st.roughness = float(water_.roughness);
            st.ior = float(water_.ior);
            st.clarity = cl;
            st.ripple = float(water_.ripple);
            st.heading = float(water_.heading * fl::kPi / 180.0);
            st.gravity = float(gravity > 0.1 ? gravity : 9.81);
            st.level = float(waterParams_.level);
            st.crest = float(waterParams_.crest);
            r.setWaterStyle(waterMesh_, st);
        }
        waterDrawnAt_ = -1.0;  // 頂点を組み直す
    }
    if (waterMesh_ != kNone) {
        const double t = scene.renderSimTime();
        r.setWaterTime(waterMesh_, t);
        // 水深を測る底 = 地面の面の高さ（回転は見ない。地面が無ければ深い海）。
        const bool hasFloor = scene.groundEnabled();
        const double floorY = scene.groundPosition().y;
        const bool floorChanged = hasFloor != waterHasFloor_ || floorY != waterFloor_;
        waterHasFloor_ = hasFloor;
        waterFloor_ = floorY;
        // 波が無ければ一度組めば足りる。
        if (waterDrawnAt_ < 0.0 || floorChanged ||
            (!waterParams_.waves.empty() && t != waterDrawnAt_)) {
            fl::waterGridVertices(waterParams_, waterCells_, t, water_.foam, pos_, nrm_, foam_);
            const std::size_t n = pos_.size() / 3;
            depth_.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                depth_[i] = hasFloor ? float(std::max(0.0, double(pos_[i * 3 + 1]) - floorY))
                                     : 1000.0f;
            }
            r.setWaterMeshVertices(waterMesh_, pos_.data(), nrm_.data(), foam_.data(),
                                   depth_.data(), n);
            waterDrawnAt_ = t;
        }
    }

    // ---- 流体粒子 ----
    const std::uint64_t fv = scene.editor().fluidVersion();
    bool restyle = false;
    if (fv != fluidVersion_) {
        fluidVersion_ = fv;
        fluidHas_ = scene.editor().hasFluid();
        fluid_ = scene.editor().fluid();
        designDirty_ = true;
        designDirtySurface_ = true;
        restyle = true;
    }
    const bool show = fluidHas_ && fluid_.enabled && !fluid_.blocks.empty();
    const std::vector<float>* live = scene.renderFluid();
    const std::vector<float>* points = nullptr;
    if (live) {
        points = live;  // シミュレート中の Chrono の粒子（補間済み）
    } else if (show) {
        if (designDirty_) {
            std::vector<fl::FluidBlock> blocks;
            for (const auto& b : fluid_.blocks) {
                fl::FluidBlock fb;
                fb.center = {b.position.x, b.position.y, b.position.z};
                fb.half = {b.half.x, b.half.y, b.half.z};
                blocks.push_back(fb);
            }
            designPoints_ =
                fl::seedFluid(blocks, fluid_.spacing, std::size_t(fluid_.maxParticles)).positions;
            designDirty_ = false;
            designUploaded_ = false;
            designDirtySurface_ = true;
        }
        points = &designPoints_;
    }
    const std::size_t count = points ? points->size() / 3 : 0;
    if (fluidHas_ && fluid_.surface) {
        // 表面の描き方: 玉の形状は要らない。
        if (fluidShape_ != kNone) {
            r.removeShape(fluidShape_);
            fluidShape_ = kNone;
            fluidShapeCount_ = 0;
        }
        renderSurface(r, points, live != nullptr, restyle);
        return;
    }
    // 玉の描き方: 表面のメッシュは要らない。
    if (liquidMesh_ != kNone) {
        r.removeLiquidMesh(liquidMesh_);
        liquidMesh_ = kNone;
    }
    if (count != fluidShapeCount_ && fluidShape_ != kNone) {
        r.removeShape(fluidShape_);
        fluidShape_ = kNone;
        fluidShapeCount_ = 0;
    }
    if (count == 0) return;
    if (fluidShape_ == kNone) {
        const std::vector<unsigned> idx = fl::particleMeshIndices(count);
        fluidShape_ = r.addSoftShape(count * fl::kParticleVerts,
                                     std::vector<uint32_t>(idx.begin(), idx.end()));
        fluidShapeCount_ = count;
        restyle = true;
        designUploaded_ = false;
    }
    if (restyle) {
        r.setShapeColor(fluidShape_, {fluid_.color.r, fluid_.color.g, fluid_.color.b});
        wizengine::ShapeMaterial m;
        m.roughness = float(fluid_.roughness);
        m.reflectance = 0.5f;
        r.setShapeMaterial(fluidShape_, m);
    }
    // 文書の配置は動かないので、組み直したときだけ送る（シミュレート中は毎フレーム）。
    if (!live && designUploaded_ && !restyle) return;
    fl::particleMeshVertices(*points, fluid_.spacing * fluid_.drawScale, pos_, nrm_);
    r.setSoftShapeVertices(fluidShape_, pos_.data(), nrm_.data(), pos_.size() / 3);
    designUploaded_ = !live;
}

// ---- 表面の描き方（メタボール + Surface Nets を別スレッドで組む）--------------------

FluidComponent::~FluidComponent() {
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        stop_ = true;
    }
    jobCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void FluidComponent::workerLoop() {
    fl::SurfaceScratch scratch;
    fl::SurfaceMesh mesh;
    std::vector<float> points;
    for (;;) {
        fl::SurfaceParams params;
        std::uint64_t serial = 0;
        {
            std::unique_lock<std::mutex> lk(jobMutex_);
            jobCv_.wait(lk, [this] { return stop_ || jobPending_; });
            if (stop_) return;
            points.swap(jobPoints_);
            params = jobParams_;
            serial = jobSerial_;
            jobPending_ = false;
            busy_ = true;
        }
        fl::buildFluidSurface(points, params, mesh, scratch);
        // 屈折と吸収に使う厚み = 水の高さの目安（粒子の上下の広がり + 粒子 1 個ぶん）。
        float ylo = 1e30f, yhi = -1e30f;
        for (std::size_t i = 1; i < points.size(); i += 3) {
            ylo = std::min(ylo, points[i]);
            yhi = std::max(yhi, points[i]);
        }
        const double thickness = ylo <= yhi ? double(yhi - ylo) + params.spacing : 0.0;
        {
            std::lock_guard<std::mutex> lk(jobMutex_);
            result_.swap(mesh);
            resultThickness_ = thickness;
            resultSerial_ = serial;
            resultReady_ = true;
            busy_ = false;
        }
    }
}

void FluidComponent::renderSurface(wizengine::Renderer& r, const std::vector<float>* points,
                                   bool live, bool restyle) {
    if (liquidMesh_ == kNone) {
        liquidMesh_ = r.addLiquidMesh();
        restyle = true;
        designDirtySurface_ = true;  // 新しい入れ物は空 = 文書の配置でも組み直す
        liquidShown_ = false;
    }
    if (!worker_.joinable()) worker_ = std::thread([this] { workerLoop(); });

    const std::size_t count = points ? points->size() / 3 : 0;
    // シミュレートの開始 / 停止で描く粒子が入れ替わる: 文書の配置は組み直しが要る
    // （止めた直後に、走っていた最後の表面を残さない）。
    if (live != lastLive_) {
        lastLive_ = live;
        designDirtySurface_ = true;
    }
    bool upload = false;
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        if (count == 0) {
            // 流体が無い: 隠す。仕掛かりの結果は捨てる（後から古い表面が出ないように）。
            hiddenAtSerial_ = submitSerial_;
            jobPending_ = false;
            resultReady_ = false;
        } else if (!jobPending_ && !busy_ && (live || designDirtySurface_)) {
            // 前の仕事が終わっていれば、今の粒子で次を頼む（描画スレッドは待たない）。
            jobPoints_.assign(points->begin(), points->end());
            jobParams_.spacing = fluid_.spacing;
            jobSerial_ = ++submitSerial_;
            jobPending_ = true;
            designDirtySurface_ = false;
            jobCv_.notify_one();
        }
        if (resultReady_) {
            resultReady_ = false;
            if (resultSerial_ > hiddenAtSerial_) {
                uploadMesh_.swap(result_);
                uploadThickness_ = resultThickness_;
                upload = true;
            }
        }
    }
    if (count == 0) {
        if (liquidShown_) r.setLiquidMesh(liquidMesh_, nullptr, nullptr, 0, nullptr, 0);
        liquidShown_ = false;
        return;
    }
    // 厚みが大きく変わったら材質も入れ直す（吸収の効き方が変わる）。
    if (upload && std::fabs(uploadThickness_ - styleThickness_) > 0.1 * styleThickness_ + 1e-3) {
        restyle = true;
    }
    if (restyle) {
        if (upload) styleThickness_ = uploadThickness_;
        wizengine::Renderer::LiquidStyle st;
        // 吸収（Beer-Lambert）: 厚み clarity の水を通すと rgba の色になる係数。
        const double c[3] = {fluid_.color.r, fluid_.color.g, fluid_.color.b};
        float a[3];
        for (int i = 0; i < 3; ++i) {
            a[i] = float(-std::log(std::clamp(c[i], 0.01, 1.0)) / std::max(1e-3, fluid_.clarity));
        }
        st.absorption = {a[0], a[1], a[2]};
        st.roughness = float(fluid_.roughness);
        st.ior = float(fluid_.ior);
        st.transmission = 1.0f;
        st.thickness = float(std::max(0.01, styleThickness_));
        r.setLiquidStyle(liquidMesh_, st);
    }
    if (upload) {
        r.setLiquidMesh(liquidMesh_, uploadMesh_.positions.data(), uploadMesh_.normals.data(),
                        uploadMesh_.positions.size() / 3, uploadMesh_.indices.data(),
                        uploadMesh_.indices.size());
        liquidShown_ = true;
    }
}
