// Renderer の描画設定（影・後処理・露出・色作り）。
#include "render/RendererInternal.h"

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

namespace wizengine {

// ---- 描画設定（フォトリアル）------------------------------------------------
// シーン文書の <visual> が変わるたびに Scene が呼ぶ。Filament 側は
// 「ビューの後処理」「カメラの露出」「色作り（LUT）」「ライトの影」の
// 4 か所に分かれているので、ここで配り直す。
void Renderer::setRenderSettings(const RenderSettings& settings) {
    const RenderSettings before = settings_;
    settings_ = settings;
    // 色作りは LUT を焼く（32^3 のテクスチャ）ので、関係する値が変わった
    // ときだけ作り直す。毎フレーム呼ばれても安いままにしておきたい。
    const bool gradingChanged =
        !colorGrading_ || before.tonemap != settings_.tonemap ||
        before.contrast != settings_.contrast ||
        before.saturation != settings_.saturation ||
        before.temperature != settings_.temperature ||
        before.tint != settings_.tint;
    if (gradingChanged) rebuildColorGrading();
    for (auto& slot : views_) applyViewSettings(slot);
    applyShadowSettings();
}

// トーンマップ（HDR → 表示）と色調整を 1 個の ColorGrading に焼く。
// フォトリアルの見え方をいちばん左右するのがここ: 物理的に正しい HDR を
// どうフィルムに落とすか、という段。
void Renderer::rebuildColorGrading() {
    ColorGrading::Builder builder;
    // ToneMapper は build() が同期なので、この関数を抜けるときに壊れてよい
    // （Filament のヘッダにもそう書いてある）。
    filament::LinearToneMapper linear;
    filament::ACESToneMapper aces;
    filament::ACESLegacyToneMapper acesLegacy;
    filament::FilmicToneMapper filmic;
    filament::PBRNeutralToneMapper pbrNeutral;
    filament::AgxToneMapper agx(filament::AgxToneMapper::AgxLook::NONE);
    const ToneMapper* tm = &acesLegacy;
    switch (settings_.tonemap) {
        case RenderSettings::Tonemap::Aces: tm = &aces; break;
        case RenderSettings::Tonemap::Filmic: tm = &filmic; break;
        case RenderSettings::Tonemap::Agx: tm = &agx; break;
        case RenderSettings::Tonemap::PbrNeutral: tm = &pbrNeutral; break;
        case RenderSettings::Tonemap::Linear: tm = &linear; break;
        case RenderSettings::Tonemap::AcesLegacy: break;
    }
    ColorGrading* next = builder.toneMapper(tm)
                             .quality(ColorGrading::QualityLevel::HIGH)
                             .contrast(settings_.contrast)
                             .saturation(settings_.saturation)
                             .whiteBalance(settings_.temperature, settings_.tint)
                             .build(*engine_);
    if (!next) {
        // 焼けなかったときは前の設定のまま（既定のトーンマップに戻すより、
        // 直前まで見えていた絵を保つ方が驚きが少ない）。
        LOGW("render", "color grading could not be built - keeping the previous one");
        return;
    }
    // 先に全ビューへ差し替えてから古い方を壊す（使用中の LUT を消さない）。
    for (auto& slot : views_) slot.view->setColorGrading(next);
    if (colorGrading_) engine_->destroy(colorGrading_);
    colorGrading_ = next;
}

// ビュー 1 つぶん。addView（カメラの遅延生成）からも呼ぶので、あとから
// 開いたページでも設定が揃う。
void Renderer::applyViewSettings(ViewSlot& slot) {
    View* v = slot.view;
    const RenderSettings& s = settings_;

    v->setPostProcessingEnabled(s.postProcess);
    v->setColorGrading(colorGrading_);
    v->setDithering(Dithering::TEMPORAL);  // 暗部のバンディング対策
    v->setAntiAliasing(s.fxaa ? AntiAliasing::FXAA : AntiAliasing::NONE);

    MultiSampleAntiAliasingOptions msaa;
    msaa.enabled = s.msaa > 1;
    msaa.sampleCount = s.msaa;
    v->setMultiSampleAntiAliasingOptions(msaa);

    TemporalAntiAliasingOptions taa;
    taa.enabled = s.taa;
    v->setTemporalAntiAliasingOptions(taa);

    // TAA と被写界深度は画面の外の情報を使うので、縁に余白（ガードバンド）が
    // 要る。無いと画面端に尾を引く。
    GuardBandOptions guard;
    guard.enabled = s.taa || s.dofFocus > 0.0f;
    v->setGuardBandOptions(guard);

    AmbientOcclusionOptions ao;
    ao.enabled = s.ssao;
    ao.intensity = s.ssaoIntensity;
    ao.quality = QualityLevel::HIGH;
    ao.upsampling = QualityLevel::HIGH;
    ao.lowPassFilter = QualityLevel::MEDIUM;
    v->setAmbientOcclusionOptions(ao);

    BloomOptions bloom;
    bloom.enabled = s.bloom > 0.0f;
    bloom.strength = s.bloom;
    bloom.quality = QualityLevel::HIGH;
    v->setBloomOptions(bloom);

    ScreenSpaceReflectionsOptions ssr;
    ssr.enabled = s.ssr;
    v->setScreenSpaceReflectionsOptions(ssr);

    DepthOfFieldOptions dof;
    dof.enabled = s.dofFocus > 0.0f;
    dof.cocScale = s.dofBlur;
    v->setDepthOfFieldOptions(dof);

    VignetteOptions vignette;
    vignette.enabled = s.vignette > 0.0f;
    // 強さ 1 つのつまみを Filament の 3 つの値に配る。midPoint が小さいほど
    // 中心近くまで暗くなるので、強さをそこへ写す。
    vignette.midPoint = 1.0f - 0.5f * s.vignette;
    vignette.roundness = 0.5f;
    vignette.feather = 0.5f;
    v->setVignetteOptions(vignette);

    switch (s.shadow) {
        case RenderSettings::Shadow::Dpcf:
            v->setShadowType(ShadowType::DPCF);
            break;
        case RenderSettings::Shadow::Pcss:
            v->setShadowType(ShadowType::PCSS);
            break;
        case RenderSettings::Shadow::Vsm: {
            v->setShadowType(ShadowType::VSM);
            VsmShadowOptions vsm;
            vsm.anisotropy = 1;
            vsm.mipmapping = true;
            v->setVsmShadowOptions(vsm);
            break;
        }
        case RenderSettings::Shadow::Pcf:
            v->setShadowType(ShadowType::PCF);
            break;
    }
    SoftShadowOptions soft;  // DPCF / PCSS のぼけ方（既定のまま）
    v->setSoftShadowOptions(soft);

    // 露出は「写真と同じ 3 つの値」。ライトが lux / lumen の実単位なので、
    // ここを変えると現実のカメラと同じように絵の明るさが変わる。
    slot.camera->setExposure(s.aperture, 1.0f / s.shutter, s.sensitivity);
    if (s.dofFocus > 0.0f) slot.camera->setFocusDistance(s.dofFocus);
}

// 影の設定はライトごと（Filament の LightManager が持つ）。既にあるライトへ
// 流し込むので、シーンを読み直さなくても品質を上げ下げできる。
void Renderer::applyShadowSettings() {
    auto& lm = engine_->getLightManager();
    for (const auto& e : lightEntities_) {
        if (e.isNull()) continue;
        const auto li = lm.getInstance(e);
        if (!li) continue;
        lm.setShadowOptions(li, shadowOptionsFrom(settings_, lm.isDirectional(li)));
    }
}

}  // namespace wizengine
