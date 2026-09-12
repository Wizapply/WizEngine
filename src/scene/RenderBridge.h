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

}  // namespace wizengine
