#pragma once

#include <string>

#include "core/Xml.h"
#include "core/XmlWarnings.h"
#include "document/XmlValues.h"
#include "document/FluidTypes.h"

// 流体の節（<worldbody> の <water> と <fluid>）の XML の読み書き。書式:
//
//   <water pos="0 0 0" size="10 10" density="1000" drag="1" lindrag="2"
//          angdrag="1" flow="0 0 0" wave="0.1" period="4" heading="0"
//          wind="6" spread="45" waves="12" seed="1" choppy="1"
//          rgba="0.03 0.16 0.22 1" roughness="0.05" clarity="4" ior="1.33"
//          ripple="0.5" foam="0.4" cells="96"/>
//     pos の y が静水面の高さ、size は範囲の半分の広さ（X Z の 2 つ。1 つなら両方）。
//     wave / period はうねり 1 本、wind（m/s）は海の波のスペクトルで waves 本の風浪
//     （どちらも heading の向きへ進む）。rgba は深い水の色、clarity は透明度 (m)。
//     enabled="false" で設定を残したまま水を抜く。
//
//   <fluid spacing="0.04" density="1000" viscosity="0" pressure="0.01" friction="0"
//          cohesion="0" maxspeed="20" recovery="10" relaxation="4" epsilon="0.001"
//          max="40000" surface="true" rgba="0.1 0.32 0.8 1" clarity="0.3" ior="1.33"
//          roughness="0.15" drawscale="0.7">
//     surface="true"（既定）= 粒子をつないだ水の表面を透ける水の材質で描く（rgba は
//     厚み clarity (m) の水を通した色）。"false" = 粒子ごとの玉（rgba は玉の色、
//     drawscale は玉の半径の倍率）。
//     <block pos="0 0.5 0" size="0.3 0.3 0.3" velocity="0 0 0"/>   ... 水を置く箱（size は半寸法）
//   </fluid>
//
// どちらも worldbody 直下の単一要素（2 個目以降は警告して無視）。読み取りは既定値
// 付き（無い属性 = 既定値）、書き出しは既定値と同じ属性を省く（<fluid> の
// spacing・density と <water> の pos・size は常に書く）。
// ヘッダだけで完結させてある（文書層のテストのビルドに .cpp を足さなくて済むように）。
namespace wizengine {
namespace editor {

namespace fluidxml_detail {
inline bool isZero(const Vec3d& v) { return v.x == 0.0 && v.y == 0.0 && v.z == 0.0; }
}  // namespace fluidxml_detail

// ---- 水面 ----------------------------------------------------------------------

inline xml::Element waterElement(const WaterDesc& w) {
    xml::Element e("water");
    const WaterDesc d;
    if (!w.enabled) e.setBool("enabled", false);
    setVec3(e, "pos", w.position);
    const double size[2] = {w.halfX, w.halfZ};
    e.setNumbers("size", size, 2);
    if (w.density != d.density) e.setNumber("density", w.density);
    if (w.drag != d.drag) e.setNumber("drag", w.drag);
    if (w.linearDrag != d.linearDrag) e.setNumber("lindrag", w.linearDrag);
    if (w.angularDrag != d.angularDrag) e.setNumber("angdrag", w.angularDrag);
    if (!fluidxml_detail::isZero(w.flow)) setVec3(e, "flow", w.flow);
    if (w.wave != d.wave) e.setNumber("wave", w.wave);
    if (w.period != d.period) e.setNumber("period", w.period);
    if (w.heading != d.heading) e.setNumber("heading", w.heading);
    if (w.wind != d.wind) e.setNumber("wind", w.wind);
    if (w.spread != d.spread) e.setNumber("spread", w.spread);
    if (w.waves != d.waves) e.setInt("waves", w.waves);
    if (w.seed != d.seed) e.setInt("seed", w.seed);
    if (w.choppy != d.choppy) e.setNumber("choppy", w.choppy);
    setColor(e, "rgba", w.color);
    if (w.roughness != d.roughness) e.setNumber("roughness", w.roughness, 4);
    if (w.clarity != d.clarity) e.setNumber("clarity", w.clarity);
    if (w.ior != d.ior) e.setNumber("ior", w.ior);
    if (w.ripple != d.ripple) e.setNumber("ripple", w.ripple);
    if (w.foam != d.foam) e.setNumber("foam", w.foam);
    if (w.cells != d.cells) e.setInt("cells", w.cells);
    return e;
}

inline WaterDesc waterFromXml(const xml::Element& e, xml::Warn& warn) {
    WaterDesc w;
    w.enabled = e.boolean("enabled", w.enabled);
    w.position = getVec3(e, "pos", w.position);
    double size[2] = {w.halfX, w.halfZ};
    const std::size_t n = e.numbers("size", size, 2);
    if (n == 1) size[1] = size[0];  // 1 つだけなら正方形
    w.halfX = size[0];
    w.halfZ = size[1];
    w.density = e.number("density", w.density);
    w.drag = e.number("drag", w.drag);
    w.linearDrag = e.number("lindrag", w.linearDrag);
    w.angularDrag = e.number("angdrag", w.angularDrag);
    w.flow = getVec3(e, "flow", w.flow);
    w.wave = e.number("wave", w.wave);
    w.period = e.number("period", w.period);
    w.heading = e.number("heading", w.heading);
    w.wind = e.number("wind", w.wind);
    w.spread = e.number("spread", w.spread);
    w.waves = e.integer("waves", w.waves);
    w.seed = e.integer("seed", w.seed);
    w.choppy = e.number("choppy", w.choppy);
    w.color = getColor(e, "rgba", w.color);
    w.roughness = e.number("roughness", w.roughness);
    w.clarity = e.number("clarity", w.clarity);
    w.ior = e.number("ior", w.ior);
    w.ripple = e.number("ripple", w.ripple);
    w.foam = e.number("foam", w.foam);
    w.cells = e.integer("cells", w.cells);
    if (!e.children().empty()) warnUnknownChildren(e, {}, "<water>", warn);
    if (e.has("level")) {
        warn("<water level> is not an attribute - the still-water height is the y of pos");
    }
    return clampWater(w);
}

// ---- 流体粒子 ------------------------------------------------------------------

inline xml::Element fluidElement(const FluidDesc& f) {
    xml::Element e("fluid");
    const FluidDesc d;
    if (!f.enabled) e.setBool("enabled", false);
    e.setNumber("spacing", f.spacing);
    e.setNumber("density", f.density);
    if (f.viscosity != d.viscosity) e.setNumber("viscosity", f.viscosity);
    if (f.pressure != d.pressure) e.setNumber("pressure", f.pressure);
    if (f.friction != d.friction) e.setNumber("friction", f.friction);
    if (f.cohesion != d.cohesion) e.setNumber("cohesion", f.cohesion);
    if (f.maxSpeed != d.maxSpeed) e.setNumber("maxspeed", f.maxSpeed);
    if (f.recovery != d.recovery) e.setNumber("recovery", f.recovery);
    if (f.relaxation != d.relaxation) e.setNumber("relaxation", f.relaxation);
    if (f.epsilon != d.epsilon) e.setNumber("epsilon", f.epsilon);
    if (f.maxParticles != d.maxParticles) e.setInt("max", f.maxParticles);
    setColor(e, "rgba", f.color);
    if (f.surface != d.surface) e.setBool("surface", f.surface);
    if (f.clarity != d.clarity) e.setNumber("clarity", f.clarity);
    if (f.ior != d.ior) e.setNumber("ior", f.ior);
    if (f.roughness != d.roughness) e.setNumber("roughness", f.roughness, 4);
    if (f.drawScale != d.drawScale) e.setNumber("drawscale", f.drawScale);
    for (const auto& b : f.blocks) {
        xml::Element be("block");
        setVec3(be, "pos", b.position);
        setVec3(be, "size", b.half);
        if (!fluidxml_detail::isZero(b.velocity)) setVec3(be, "velocity", b.velocity);
        e.append(std::move(be));
    }
    return e;
}

inline FluidDesc fluidFromXml(const xml::Element& e, xml::Warn& warn) {
    FluidDesc f;
    f.enabled = e.boolean("enabled", f.enabled);
    f.spacing = e.number("spacing", f.spacing);
    f.density = e.number("density", f.density);
    f.viscosity = e.number("viscosity", f.viscosity);
    f.pressure = e.number("pressure", f.pressure);
    f.friction = e.number("friction", f.friction);
    f.cohesion = e.number("cohesion", f.cohesion);
    f.maxSpeed = e.number("maxspeed", f.maxSpeed);
    f.recovery = e.number("recovery", f.recovery);
    f.relaxation = e.number("relaxation", f.relaxation);
    f.epsilon = e.number("epsilon", f.epsilon);
    f.maxParticles = e.integer("max", f.maxParticles);
    f.color = getColor(e, "rgba", f.color);
    f.surface = e.boolean("surface", f.surface);
    f.clarity = e.number("clarity", f.clarity);
    f.ior = e.number("ior", f.ior);
    f.roughness = e.number("roughness", f.roughness);
    f.drawScale = e.number("drawscale", f.drawScale);
    warnUnknownChildren(e, {"block"}, "<fluid>", warn);
    for (const xml::Element* b : e.all("block")) {
        if (f.blocks.size() >= kMaxFluidBlocks) {
            warn("<fluid>: more than " + std::to_string(kMaxFluidBlocks) +
                 " <block> - the rest are ignored");
            break;
        }
        FluidBlockDesc bd;
        bd.position = getVec3(*b, "pos", bd.position);
        bd.half = getVec3(*b, "size", bd.half);
        bd.velocity = getVec3(*b, "velocity", bd.velocity);
        if (bd.half.x <= 0.0 || bd.half.y <= 0.0 || bd.half.z <= 0.0) {
            warn("<fluid><block>: size must be positive half-extents - clamped");
        }
        f.blocks.push_back(bd);
    }
    if (f.blocks.empty()) {
        warn("<fluid> has no <block> - no particles are made (add <block pos size/>)");
    }
    return clampFluid(f);
}

}  // namespace editor
}  // namespace wizengine
