#pragma once

// 「文書の値」を「レンダラの語彙」へ直す小さな橋渡し。MathBridge.h が
// Chrono → Filament の姿勢を受け持つのと同じ位置づけで、こちらは
// document/EditorTypes.h の材質・描画設定（文字列と double の設計値）を
// render/Renderer.h の構造体（列挙型と float）へ付け替えるだけを行う。
//
// Scene（scene/SceneInternal.h 経由）と PrefabComponent の両方が使うので、
// Scene の私的ヘッダではなくここに置く - 同じ変換を 2 か所に書くと、
// 材質の値を足したときに片方だけ直して黙って食い違うため。

#include <cstdint>

#include "document/EditorTypes.h"
#include "render/Renderer.h"
#include "terrain/TerrainFile.h"  // TerrainLayers

namespace wizengine {

// 材質（PBR）。文書の <geom> / <part> の属性 -> Filament のマテリアル
// パラメータ。
inline ShapeMaterial toRendererMaterial(const editor::MaterialDesc& m) {
    ShapeMaterial r;
    r.roughness = float(m.roughness);
    r.metallic = float(m.metallic);
    r.reflectance = float(m.reflectance);
    r.clearCoat = float(m.clearCoat);
    r.clearCoatRoughness = float(m.clearCoatRoughness);
    r.emissive = float(m.emissive);
    return r;
}

// 描画設定。文書の <visual> -> View / Camera / ColorGrading の設定。
// 種類の名前（文字列）を列挙型に直すのはここだけ - Renderer は文字列を
// 知らないし、文書側は手で書ける文字列のままがよい。
inline RenderSettings toRendererSettings(const editor::RenderDesc& d) {
    RenderSettings r;
    r.shadowMap = uint32_t(d.shadowMap);
    r.cascades = uint8_t(d.cascades);
    r.shadow = d.shadow == "dpcf"   ? RenderSettings::Shadow::Dpcf
               : d.shadow == "pcss" ? RenderSettings::Shadow::Pcss
               : d.shadow == "vsm"  ? RenderSettings::Shadow::Vsm
                                    : RenderSettings::Shadow::Pcf;
    r.contactShadows = d.contactShadows;
    r.msaa = uint8_t(d.msaa);
    r.taa = d.taa;
    r.fxaa = d.fxaa;
    r.postProcess = d.postProcess;
    r.ssao = d.ssao;
    r.ssaoIntensity = float(d.ssaoIntensity);
    r.bloom = float(d.bloom);
    r.ssr = d.ssr;
    r.dofFocus = float(d.dof);
    r.dofBlur = float(d.dofBlur);
    r.vignette = float(d.vignette);
    r.aperture = float(d.aperture);
    r.shutter = float(d.shutter);
    r.sensitivity = float(d.sensitivity);
    r.tonemap = d.tonemap == "aces"         ? RenderSettings::Tonemap::Aces
                : d.tonemap == "filmic"     ? RenderSettings::Tonemap::Filmic
                : d.tonemap == "agx"        ? RenderSettings::Tonemap::Agx
                : d.tonemap == "pbrneutral" ? RenderSettings::Tonemap::PbrNeutral
                : d.tonemap == "linear"     ? RenderSettings::Tonemap::Linear
                                            : RenderSettings::Tonemap::AcesLegacy;
    r.contrast = float(d.contrast);
    r.saturation = float(d.saturation);
    r.temperature = float(d.temperature);
    r.tint = float(d.tint);
    return r;
}

// テレインの表面。文書の <terrain> の色・材質 + .terrain の層（LAYR）->
// setTerrainSurface の値。層の数は terrain::kSplatChannels と
// TerrainSurface::kLayers が同じ（SceneTerrainRender.cpp の static_assert）。
// 層の粗さ・金属の「テレイン全体に従う」はここで全体の値へ解決する（Renderer は
// 解決済みの値だけを受け、従うかどうかを知らない）。
inline TerrainSurface toTerrainSurface(const editor::TerrainDesc& desc,
                                       const terrain::TerrainLayers& layers) {
    TerrainSurface r;
    const float roughness = float(desc.roughness);
    const float metallic = float(desc.metallic);
    for (int i = 0; i < TerrainSurface::kLayers; ++i) {
        const terrain::TerrainLayer& l = layers[std::size_t(i)];
        TerrainSurface::Layer& out = r.layers[i];
        out.texture = l.texture;
        out.normal = l.normal;
        out.mask = l.mask;
        out.tileMeters = l.tile;
        out.roughness = terrain::layerFollowsTerrain(l.roughness) ? roughness : l.roughness;
        out.metallic = terrain::layerFollowsTerrain(l.metallic) ? metallic : l.metallic;
        out.normalScale = l.normalScale;
    }
    r.color = filament::math::float4{desc.color.r, desc.color.g, desc.color.b, 1.0f};
    return r;
}

}  // namespace wizengine
