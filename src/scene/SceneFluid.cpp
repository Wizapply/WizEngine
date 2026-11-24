// 流体: 水面（<water>）と流体粒子（<fluid>）の Scene 側の受け持ち。
//   ・流体粒子の実体を作る（buildFluid。シミュレート開始と、その最中の系の作り直し）
//   ・ブラウザの edit.water / edit.fluid（部分更新）
//   ・/scene の water / fluid / fluidStatus
// 水面の浮力・抗力と、水面・粒子の描画は FluidComponent の担当（ここは設計値と
// Chrono の流体コンテナまで）。

#include "scene/SceneInternal.h"

#include "fluid/FluidSeed.h"

namespace {

// 文書の箱 → 純粋な計算の箱。
std::vector<wizengine::fluid::FluidBlock> toBlocks(const ed::FluidDesc& f) {
    std::vector<wizengine::fluid::FluidBlock> out;
    out.reserve(f.blocks.size());
    for (const auto& b : f.blocks) {
        wizengine::fluid::FluidBlock fb;
        fb.center = {b.position.x, b.position.y, b.position.z};
        fb.half = {b.half.x, b.half.y, b.half.z};
        fb.velocity = {b.velocity.x, b.velocity.y, b.velocity.z};
        out.push_back(fb);
    }
    return out;
}

}  // namespace

void Scene::buildFluid() {
    FluidStatus st;
    fluidLive_ = false;
    fluidBuiltVersion_ = editor_.fluidVersion();
    if (editor_.hasFluid()) {
        const ed::FluidDesc f = editor_.fluid();
        if (!f.enabled) {
            st.error = "fluid is disabled (enabled=\"false\")";
        } else if (f.blocks.empty()) {
            st.error = "no <block> in <fluid>";
        } else if (physics_.hasFluid()) {
            // enterMode が先に系を作り直すので、ここへは来ないはず（二重に足さない）。
            st.error = "the system already has fluid particles";
        } else {
            const wizengine::fluid::FluidSeedResult seed =
                wizengine::fluid::seedFluid(toBlocks(f), f.spacing, std::size_t(f.maxParticles));
            st.requested = seed.requested;
            st.truncated = seed.truncated;
            const ed::SimSettings sim = editor_.sim();
            const double dt = 1.0 / (double(std::max(1, sim.hz)) * double(std::max(1, sim.substeps)));
            FluidSpec spec;
            spec.positions = seed.positions;
            spec.velocities = seed.velocities;
            spec.kernelRadius = wizengine::fluid::kernelRadius(f.spacing);
            spec.particleMass = f.density * wizengine::fluid::particleVolume(f.spacing);
            spec.density = f.density;
            spec.viscosity = f.viscosity;
            spec.artificialPressure = f.pressure;
            spec.friction = f.friction;
            spec.cohesion = f.cohesion;
            spec.maxVelocity = f.maxSpeed;
            spec.recovery = f.recovery;
            spec.tau = f.relaxation * dt;
            spec.epsilon = f.epsilon;
            std::string why;
            if (physics_.addFluid(spec, why)) {
                st.particles = physics_.fluidParticleCount();
                fluidLive_ = true;
                if (seed.truncated) {
                    LOGW("scene", "fluid: %zu particles requested, capped at max=%d",
                         seed.requested, f.maxParticles);
                }
                // 粒子の流体は細かい刻みが要る（Chrono のデモは 1 ms）。粗い刻みは
                // 圧縮されたまま弾ける・剛体を抜ける、の元。
                if (dt > 0.0025) {
                    LOGW("scene", "fluid: the physics step is %.2f ms - fluid particles "
                                  "usually need <= 2 ms (raise <option rate> or substeps)",
                         dt * 1000.0);
                }
            } else {
                st.error = why;
                LOGW("scene", "fluid particles not created: %s", why.c_str());
                editor_.setStatus("流体粒子を作れませんでした: " + why);
            }
        }
    }
    std::lock_guard<std::mutex> lk(poseMutex_);
    fluidStatus_ = st;
}

bool Scene::resetFluid() {
    if (editor_.mode() != ed::AppMode::Simulate) return false;
    bool want = false;
    if (editor_.hasFluid()) {
        const ed::FluidDesc f = editor_.fluid();
        want = f.enabled && !f.blocks.empty();
    }
    const bool changed = editor_.fluidVersion() != fluidBuiltVersion_;
    // いつもの Reset: 作ったときのままなら、粒子をその場で書き戻すだけ。
    if (fluidLive_ && !changed) {
        physics_.resetFluid();
        return false;
    }
    if (!want && !fluidLive_ && !physics_.hasFluid()) return false;  // 流体と無関係
    // シミュレート中に足した / 外した / 設定を変えた: 開始と同じく系ごと作り直す
    // （粒子は外せない = FluidParticles.h。作り直しは置き直し・ジョイント・ケーブル・
    // 流体・snapshot まで済ませる）。系の種類が変わるなら syncBackend がそれをやる。
    if (!syncBackend()) rebuildPhysicsWorld(physics_.backend(), physics_.contactMethod());
    return true;
}

void Scene::applyWaterOp(const nlohmann::json& a) {
    // "present": false = 水面の節ごと外す。それ以外は送られてきたキーだけ上書きし、
    // 無かった水面は既定値から作る（アセットパネルの Water タイル）。
    if (a.contains("present") && a["present"].is_boolean() && !a["present"].get<bool>()) {
        editor_.setWater(false, editor_.water());
        editor_.setStatus("水面を外しました");
        return;
    }
    const bool had = editor_.hasWater();
    ed::WaterDesc base = had ? editor_.water() : ed::WaterDesc{};
    // 新しく置く水面は地面（y = 0）から少し上げる（同じ高さだと面が重なってちらつく）。
    if (!had && !a.contains("pos")) base.position.y = 0.5;
    editor_.setWater(true, ed::clampWater(ed::waterFromJson(a, base)));
    editor_.setStatus(had ? "水面を更新" : "水面を追加しました");
}

void Scene::applyFluidOp(const nlohmann::json& a) {
    const bool simulating = editor_.mode() == ed::AppMode::Simulate;
    if (a.contains("present") && a["present"].is_boolean() && !a["present"].get<bool>()) {
        editor_.setFluid(false, editor_.fluid());
        editor_.setStatus("流体粒子を外しました" +
                          std::string(simulating ? "（Reset か次のシミュレート開始で消えます）" : ""));
        syncBackend();
        return;
    }
    const bool had = editor_.hasFluid();
    ed::FluidDesc base = had ? editor_.fluid() : ed::FluidDesc{};
    if (!had && base.blocks.empty() && !a.contains("blocks")) {
        // 新しく足すときは箱を 1 つ置いておく（空の流体は何も作らない）。
        base.blocks.push_back(ed::FluidBlockDesc{});
    }
    editor_.setFluid(true, ed::clampFluid(ed::fluidFromJson(a, base)));
    // 流体があれば Multicore を選ぶ（無くなれば既定へ戻す）。
    syncBackend();
    editor_.setStatus(std::string(had ? "流体粒子を更新" : "流体粒子を追加しました") +
                      (simulating ? "（Reset か次のシミュレート開始で作り直します）" : ""));
}

nlohmann::json Scene::fluidJson() {
    nlohmann::json j;
    j["water"] = editor_.hasWater() ? ed::toJson(editor_.water()) : nlohmann::json();
    j["fluid"] = editor_.hasFluid() ? ed::toJson(editor_.fluid()) : nlohmann::json();
    FluidStatus st;
    {
        std::lock_guard<std::mutex> lk(poseMutex_);
        st = fluidStatus_;
    }
    j["fluidStatus"] = {{"particles", st.particles},
                        {"requested", st.requested},
                        {"truncated", st.truncated},
                        {"error", st.error}};
    return j;
}

// ---- 空気 ---------------------------------------------------------------------------

namespace {
// 文書の値 → 純粋な計算の語彙（fluid/ は文書を知らない）。
wizengine::fluid::AirSettings airSettingsOf(const ed::SimSettings& s) {
    wizengine::fluid::AirSettings a;
    a.density = s.airDensity;
    a.atmosphere = s.airAtmosphere;
    a.temperature = s.airTemperature;
    a.humidity = s.airHumidity;
    a.pressure = s.airPressure;
    a.altitude = s.airAltitude;
    a.wind = {s.windX, s.windY, s.windZ};
    a.drag = s.airDrag;
    a.lift = s.airLift;
    a.magnus = s.airMagnus;
    return a;
}
}  // namespace

wizengine::fluid::AirState Scene::airNow() {
    const wizengine::fluid::AirSettings doc = airSettingsOf(editor_.sim());
    wizengine::fluid::AirOverride ov;
    const bool simulating = editor_.mode() == ed::AppMode::Simulate;
    const std::uint64_t gen = runGeneration();
    {
        std::lock_guard<std::mutex> lk(airMutex_);
        if (simulating && airOverrideGeneration_ == gen) ov = airOverride_;
    }
    return wizengine::fluid::resolveAir(doc, ov);
}

void Scene::setAirOverride(const wizengine::fluid::AirOverride& ov) {
    const std::uint64_t gen = runGeneration();
    std::lock_guard<std::mutex> lk(airMutex_);
    airOverride_ = ov;
    airOverrideGeneration_ = gen;
}

nlohmann::json Scene::airJson() {
    const wizengine::fluid::AirState a = airNow();
    return nlohmann::json{{"density", a.density},
                          {"fromConditions", a.fromConditions},
                          {"conditionsDensity", a.conditionsDensity},
                          {"temperature", a.temperature},
                          {"humidity", a.humidity},
                          {"pressure", a.pressure},
                          {"altitude", a.altitude},
                          {"localPressure", a.localPressure},
                          {"wind", {{"x", a.wind.x}, {"y", a.wind.y}, {"z", a.wind.z}}},
                          {"drag", a.drag},
                          {"lift", a.lift},
                          {"magnus", a.magnus},
                          {"scripted", a.scripted}};
}
