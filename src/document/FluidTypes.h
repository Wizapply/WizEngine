#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Clamp.h"
#include "document/EditorTypes.h"

// 流体の文書の型（<worldbody> の <water> と <fluid>）。EditorTypes.h と同じく
// Chrono も Filament も出てこないので、どのスレッドからでもコピーできる。
// 保存フォーマット（document/FluidXml）とブラウザ API（/scene の water / fluid、
// edit.water / edit.fluid）のキーはここが唯一の定義。
namespace wizengine {
namespace editor {

// ---- 水面（浮力・抗力。fluid/WaterForces.h）----------------------------------
// 「水が満ちている範囲」と水面の揺れ。物理の系の中に水は無く、FluidComponent が
// 毎ステップ水面の下にある物へ浮力と抗力を掛ける（Core / Multicore どちらでも）。
// 範囲は position を中心にした X / Z の長方形（halfX / halfZ は半分の広さ）で、
// 高さは position.y が静水面（水は下へ無限に続く）。範囲の外の物には効かない。
// 波はうねり（wave / period / heading の 1 本）と風浪（wind から海のスペクトルで
// waves 本）の Gerstner 波の和（fluid/WaterForces.h）。見た目は水面の格子 1 枚を
// 透けて屈折する水の材質（water.mat）で描く: 浅い所は底が透けて見え、深い所は
// color（水の中で散った光の色）になる。さざ波（ripple）は材質の中の法線だけ、
// 波頭の白波（foam）は格子の縮みから。
struct WaterDesc {
    bool enabled = true;             // false = 設定を残したまま水を抜く
    Vec3d position{0.0, 0.0, 0.0};   // 範囲の中心。y = 静水面の高さ (m)
    double halfX = 10.0;             // 範囲の半分の広さ (m)
    double halfZ = 10.0;
    double density = 1000.0;         // kg/m^3（海水 1025、油 900）
    double drag = 1.0;               // 二乗の抗力係数 Cd
    double linearDrag = 2.0;         // 線形の減衰 (1/s。上下の揺れの収まり方)
    double angularDrag = 1.0;        // 回転の減衰 (1/s)
    Vec3d flow{0.0, 0.0, 0.0};       // 流れ (m/s)
    double wave = 0.0;               // 波の振幅 (m、0 = 波なし)
    double period = 4.0;             // 波の周期 (s)
    double heading = 0.0;            // 波と風の進む向き（Y まわりの度、0 = +X）
    // 風浪（海の波のスペクトル Pierson-Moskowitz。有義波高 0.21 U² / g）
    double wind = 0.0;               // 風速 (m/s、0 = 風浪なし。5 で波高 0.5 m・10 で 2 m)
    double spread = 45.0;            // 波の向きのばらつき（heading ± 度）
    int waves = 12;                  // 風浪の成分の数（多いほど不規則、重い）
    int seed = 1;                    // 成分の向きと位相を決める種（変えると別の海）
    double choppy = 1.0;             // 波頭の尖り方（0 = 正弦波、1 = Gerstner 波、> 1 = 誇張）
    // 見た目
    Color3 color{0.03f, 0.16f, 0.22f};  // 深い水の色（水の中で散った光。リニア）
    double roughness = 0.05;         // 水面の粗さ（小さいほど映り込む）
    double clarity = 4.0;            // 透明度 (m)。この深さで底がほぼ見えなくなる
    double ior = 1.33;               // 屈折率（水 1.33）
    double ripple = 0.5;             // さざ波の強さ（0 = 鏡のような水面）
    double foam = 0.4;               // 波頭の白波の量（0 = 出さない、1 = 多い）
    int cells = 96;                  // 水面の格子の分割（1 辺。波の細かさ）
};

inline WaterDesc clampWater(WaterDesc w) {
    const WaterDesc d;
    const auto cl = clampOrFallback;
    w.position.x = cl(w.position.x, -1.0e5, 1.0e5, d.position.x);
    w.position.y = cl(w.position.y, -1.0e5, 1.0e5, d.position.y);
    w.position.z = cl(w.position.z, -1.0e5, 1.0e5, d.position.z);
    w.halfX = cl(w.halfX, 0.05, 5000.0, d.halfX);
    w.halfZ = cl(w.halfZ, 0.05, 5000.0, d.halfZ);
    w.density = cl(w.density, 1.0, 20000.0, d.density);
    w.drag = cl(w.drag, 0.0, 10.0, d.drag);
    w.linearDrag = cl(w.linearDrag, 0.0, 100.0, d.linearDrag);
    w.angularDrag = cl(w.angularDrag, 0.0, 100.0, d.angularDrag);
    w.flow.x = cl(w.flow.x, -100.0, 100.0, 0.0);
    w.flow.y = cl(w.flow.y, -100.0, 100.0, 0.0);
    w.flow.z = cl(w.flow.z, -100.0, 100.0, 0.0);
    w.wave = cl(w.wave, 0.0, 20.0, d.wave);
    w.period = cl(w.period, 0.2, 60.0, d.period);
    w.heading = clampAngleDegrees(w.heading);
    w.wind = cl(w.wind, 0.0, 40.0, d.wind);
    w.spread = cl(w.spread, 0.0, 180.0, d.spread);
    if (w.waves < 1) w.waves = 1;
    if (w.waves > 64) w.waves = 64;
    if (w.seed < 0) w.seed = -w.seed;
    w.choppy = cl(w.choppy, 0.0, 3.0, d.choppy);
    w.roughness = cl(w.roughness, 0.0, 1.0, d.roughness);
    w.clarity = cl(w.clarity, 0.01, 1000.0, d.clarity);
    w.ior = cl(w.ior, 1.0, 2.5, d.ior);
    w.ripple = cl(w.ripple, 0.0, 2.0, d.ripple);
    w.foam = cl(w.foam, 0.0, 1.0, d.foam);
    if (w.cells < 1) w.cells = 1;
    if (w.cells > 256) w.cells = 256;
    return w;
}

inline nlohmann::json toJson(const WaterDesc& w) {
    nlohmann::json j;
    j["enabled"] = w.enabled;
    j["pos"] = toJson(w.position);
    j["sizeX"] = w.halfX;
    j["sizeZ"] = w.halfZ;
    j["density"] = w.density;
    j["drag"] = w.drag;
    j["linearDrag"] = w.linearDrag;
    j["angularDrag"] = w.angularDrag;
    j["flow"] = toJson(w.flow);
    j["wave"] = w.wave;
    j["period"] = w.period;
    j["heading"] = w.heading;
    j["wind"] = w.wind;
    j["spread"] = w.spread;
    j["waves"] = w.waves;
    j["seed"] = w.seed;
    j["choppy"] = w.choppy;
    j["color"] = colorToHex(w.color);
    j["roughness"] = w.roughness;
    j["clarity"] = w.clarity;
    j["ior"] = w.ior;
    j["ripple"] = w.ripple;
    j["foam"] = w.foam;
    j["cells"] = w.cells;
    return j;
}

// 送られてきたキーだけ base に上書きする（edit.water の部分更新）。
inline WaterDesc waterFromJson(const nlohmann::json& j, const WaterDesc& base) {
    WaterDesc w = base;
    if (!j.is_object()) return w;
    if (j.contains("enabled") && j["enabled"].is_boolean()) w.enabled = j["enabled"].get<bool>();
    if (const auto it = j.find("pos"); it != j.end()) w.position = vec3FromJson(*it, w.position);
    w.halfX = jsonNumber(j, "sizeX", w.halfX);
    w.halfZ = jsonNumber(j, "sizeZ", w.halfZ);
    w.density = jsonNumber(j, "density", w.density);
    w.drag = jsonNumber(j, "drag", w.drag);
    w.linearDrag = jsonNumber(j, "linearDrag", w.linearDrag);
    w.angularDrag = jsonNumber(j, "angularDrag", w.angularDrag);
    if (const auto it = j.find("flow"); it != j.end()) w.flow = vec3FromJson(*it, w.flow);
    w.wave = jsonNumber(j, "wave", w.wave);
    w.period = jsonNumber(j, "period", w.period);
    w.heading = jsonNumber(j, "heading", w.heading);
    w.wind = jsonNumber(j, "wind", w.wind);
    w.spread = jsonNumber(j, "spread", w.spread);
    w.waves = jsonInt(j, "waves", w.waves);
    w.seed = jsonInt(j, "seed", w.seed);
    w.choppy = jsonNumber(j, "choppy", w.choppy);
    if (j.contains("color") && j["color"].is_string()) w.color = colorFromHex(j["color"], w.color);
    w.roughness = jsonNumber(j, "roughness", w.roughness);
    w.clarity = jsonNumber(j, "clarity", w.clarity);
    w.ior = jsonNumber(j, "ior", w.ior);
    w.ripple = jsonNumber(j, "ripple", w.ripple);
    w.foam = jsonNumber(j, "foam", w.foam);
    w.cells = jsonInt(j, "cells", w.cells);
    return w;
}

// ---- 流体粒子（Chrono::Multicore の ChFluidContainer）--------------------------
// 水そのものを粒子で解く（注ぐ・跳ねる・かき混ぜる・物を浮かべる。粒子と剛体の
// 接触は双方向）。粒子は blocks（中心・半寸法・初速の箱）を間隔 spacing の
// 六方最密格子で満たして作る（fluid/FluidSeed.h）。Multicore 専用なので、文書に
// 流体があれば Scene が Multicore を選ぶ（Core 専用の機能と同じシーンには置けない =
// そのときは流体を作らずに警告する）。ジョイントと同じく「シミュレート開始で作り、
// 次の開始で作り直す」。
struct FluidBlockDesc {
    Vec3d position{0.0, 0.5, 0.0};   // 箱の中心
    Vec3d half{0.2, 0.2, 0.2};       // 半寸法（MJCF の size と同じ半分）
    Vec3d velocity{0.0, 0.0, 0.0};   // 初速 (m/s)
};

struct FluidDesc {
    bool enabled = true;
    double spacing = 0.04;           // 粒子の間隔 (m)。小さいほど細かい（数は 3 乗で増える）
    double density = 1000.0;         // 静止密度 kg/m^3（粒子の質量はここから）
    double viscosity = 0.0;          // 粘性（0 = 切る）
    double pressure = 0.01;          // 人工圧力の係数（表面張力のような凝集。0 = 切る）
    double friction = 0.0;           // 剛体との摩擦
    double cohesion = 0.0;           // 剛体との付着
    double maxSpeed = 20.0;          // 粒子の速さの上限 (m/s)
    double recovery = 10.0;          // 剛体へのめり込みを押し戻す速さ (m/s)
    double relaxation = 4.0;         // 密度拘束の緩和（ステップ数。小さいほど硬い）
    double epsilon = 1.0e-3;         // 拘束の正則化（大きいほど柔らかく安定）
    int maxParticles = 40000;        // 作る粒子の上限
    // 見た目。surface = true（既定）は粒子どうしをつないだ水の表面（fluid/FluidSurface の
    // メタボール + Surface Nets）を、透けて屈折する水の材質（liquid.mat）で描く。
    // false は従来の粒子ごとの玉（不透明）。
    bool surface = true;
    // 水の色: 表面の描き方では「厚み clarity (m) の水を通した光の色」（吸収 =
    // Beer-Lambert。浅い所は透明に近く、深い所ほどこの色に寄る）。玉の描き方では
    // 玉の色そのもの。
    Color3 color{0.10f, 0.32f, 0.80f};
    double clarity = 0.3;            // 上の色になる厚み (m)
    double ior = 1.33;               // 屈折率（水 1.33）
    double roughness = 0.15;
    double drawScale = 0.7;          // 玉の描き方の玉の半径 = spacing × これ
    std::vector<FluidBlockDesc> blocks;
};

constexpr std::size_t kMaxFluidBlocks = 64;

inline FluidDesc clampFluid(FluidDesc f) {
    const FluidDesc d;
    const auto cl = clampOrFallback;
    f.spacing = cl(f.spacing, 0.002, 1.0, d.spacing);
    f.density = cl(f.density, 1.0, 20000.0, d.density);
    f.viscosity = cl(f.viscosity, 0.0, 100.0, d.viscosity);
    f.pressure = cl(f.pressure, 0.0, 1.0, d.pressure);
    f.friction = cl(f.friction, 0.0, 2.0, d.friction);
    f.cohesion = cl(f.cohesion, 0.0, 100.0, d.cohesion);
    f.maxSpeed = cl(f.maxSpeed, 0.1, 1000.0, d.maxSpeed);
    f.recovery = cl(f.recovery, 0.0, 1000.0, d.recovery);
    f.relaxation = cl(f.relaxation, 1.0, 100.0, d.relaxation);
    f.epsilon = cl(f.epsilon, 1.0e-6, 1.0, d.epsilon);
    if (f.maxParticles < 1) f.maxParticles = 1;
    if (f.maxParticles > 500000) f.maxParticles = 500000;
    f.roughness = cl(f.roughness, 0.0, 1.0, d.roughness);
    f.drawScale = cl(f.drawScale, 0.1, 3.0, d.drawScale);
    f.clarity = cl(f.clarity, 0.001, 100.0, d.clarity);
    f.ior = cl(f.ior, 1.0, 3.0, d.ior);
    if (f.blocks.size() > kMaxFluidBlocks) f.blocks.resize(kMaxFluidBlocks);
    for (auto& b : f.blocks) {
        b.half.x = cl(b.half.x, 0.001, 1000.0, 0.2);
        b.half.y = cl(b.half.y, 0.001, 1000.0, 0.2);
        b.half.z = cl(b.half.z, 0.001, 1000.0, 0.2);
        b.position.x = cl(b.position.x, -1.0e5, 1.0e5, 0.0);
        b.position.y = cl(b.position.y, -1.0e5, 1.0e5, 0.0);
        b.position.z = cl(b.position.z, -1.0e5, 1.0e5, 0.0);
        b.velocity.x = cl(b.velocity.x, -100.0, 100.0, 0.0);
        b.velocity.y = cl(b.velocity.y, -100.0, 100.0, 0.0);
        b.velocity.z = cl(b.velocity.z, -100.0, 100.0, 0.0);
    }
    return f;
}

inline nlohmann::json toJson(const FluidBlockDesc& b) {
    return nlohmann::json{{"pos", toJson(b.position)},
                          {"size", toJson(b.half)},
                          {"velocity", toJson(b.velocity)}};
}

inline nlohmann::json toJson(const FluidDesc& f) {
    nlohmann::json j;
    j["enabled"] = f.enabled;
    j["spacing"] = f.spacing;
    j["density"] = f.density;
    j["viscosity"] = f.viscosity;
    j["pressure"] = f.pressure;
    j["friction"] = f.friction;
    j["cohesion"] = f.cohesion;
    j["maxSpeed"] = f.maxSpeed;
    j["recovery"] = f.recovery;
    j["relaxation"] = f.relaxation;
    j["epsilon"] = f.epsilon;
    j["max"] = f.maxParticles;
    j["color"] = colorToHex(f.color);
    j["roughness"] = f.roughness;
    j["drawScale"] = f.drawScale;
    j["surface"] = f.surface;
    j["clarity"] = f.clarity;
    j["ior"] = f.ior;
    j["blocks"] = nlohmann::json::array();
    for (const auto& b : f.blocks) j["blocks"].push_back(toJson(b));
    return j;
}

// 送られてきたキーだけ base に上書きする（edit.fluid の部分更新）。blocks は
// 送られてきたら丸ごと置き換える（配列の部分更新は持たない）。
inline FluidDesc fluidFromJson(const nlohmann::json& j, const FluidDesc& base) {
    FluidDesc f = base;
    if (!j.is_object()) return f;
    if (j.contains("enabled") && j["enabled"].is_boolean()) f.enabled = j["enabled"].get<bool>();
    f.spacing = jsonNumber(j, "spacing", f.spacing);
    f.density = jsonNumber(j, "density", f.density);
    f.viscosity = jsonNumber(j, "viscosity", f.viscosity);
    f.pressure = jsonNumber(j, "pressure", f.pressure);
    f.friction = jsonNumber(j, "friction", f.friction);
    f.cohesion = jsonNumber(j, "cohesion", f.cohesion);
    f.maxSpeed = jsonNumber(j, "maxSpeed", f.maxSpeed);
    f.recovery = jsonNumber(j, "recovery", f.recovery);
    f.relaxation = jsonNumber(j, "relaxation", f.relaxation);
    f.epsilon = jsonNumber(j, "epsilon", f.epsilon);
    f.maxParticles = jsonInt(j, "max", f.maxParticles);
    if (j.contains("color") && j["color"].is_string()) f.color = colorFromHex(j["color"], f.color);
    f.roughness = jsonNumber(j, "roughness", f.roughness);
    f.drawScale = jsonNumber(j, "drawScale", f.drawScale);
    if (j.contains("surface") && j["surface"].is_boolean()) f.surface = j["surface"].get<bool>();
    f.clarity = jsonNumber(j, "clarity", f.clarity);
    f.ior = jsonNumber(j, "ior", f.ior);
    if (const auto it = j.find("blocks"); it != j.end() && it->is_array()) {
        f.blocks.clear();
        for (const auto& bj : *it) {
            if (!bj.is_object() || f.blocks.size() >= kMaxFluidBlocks) continue;
            FluidBlockDesc b;
            if (const auto p = bj.find("pos"); p != bj.end()) b.position = vec3FromJson(*p, b.position);
            if (const auto s = bj.find("size"); s != bj.end()) b.half = vec3FromJson(*s, b.half);
            if (const auto v = bj.find("velocity"); v != bj.end()) {
                b.velocity = vec3FromJson(*v, b.velocity);
            }
            f.blocks.push_back(b);
        }
    }
    return f;
}

}  // namespace editor
}  // namespace wizengine
