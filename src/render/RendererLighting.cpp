// Renderer の環境光・スカイボックス・ライト。
#include "render/RendererInternal.h"
#include "render/EnvironmentLoader.h"

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

namespace wizengine {

// 一様な弱いアンビエント（環境マップ無し）。起動時と、シーンが環境光を
// 持たないとき（<environment hdr=""> やシーンの全消し）に使う。
void Renderer::installFlatAmbient() {
    const float3 ambientSH[1] = {float3{1.0f, 1.0f, 1.05f}};
    filament::IndirectLight* flat = filament::IndirectLight::Builder()
                                        .irradiance(1, ambientSH)
                                        .intensity(30000.0f)
                                        .build(*engine_);
    scene_->setIndirectLight(flat);
    // 背景は環境マップから作るので、一様アンビエントに戻すときは一緒に外す
    // （消えるテクスチャを参照させない）。
    if (skybox_) {
        scene_->setSkybox(nullptr);
        engine_->destroy(skybox_);
        skybox_ = nullptr;
    }
    if (ibl_) engine_->destroy(ibl_);
    if (iblTexture_) {
        engine_->destroy(iblTexture_);
        iblTexture_ = nullptr;
    }
    ibl_ = flat;
}

void Renderer::clearEnvironment() {
    installFlatAmbient();
}

std::size_t Renderer::addLight(const LightDesc& desc) {
    LightManager::Type type = LightManager::Type::DIRECTIONAL;
    switch (desc.type) {
        case LightDesc::Type::Directional:
            type = LightManager::Type::DIRECTIONAL;
            break;
        case LightDesc::Type::Point:
            type = LightManager::Type::POINT;
            break;
        case LightDesc::Type::Spot:
            // FOCUSED_SPOT keeps the light's total energy constant when the
            // cone angle changes - the physically sensible behaviour. Plain
            // SPOT keeps the per-area brightness instead.
            type = LightManager::Type::FOCUSED_SPOT;
            break;
    }

    utils::Entity e = EntityManager::get().create();
    LightManager::Builder builder(type);
    builder.color(desc.color)
        .intensity(desc.intensity)
        .direction(desc.direction)
        .castShadows(desc.castShadows)
        .shadowOptions(shadowOptionsFrom(
            settings_, type == LightManager::Type::DIRECTIONAL));
    if (desc.type != LightDesc::Type::Directional) {
        builder.position(desc.position).falloff(desc.falloffRadius);
    }
    if (desc.type == LightDesc::Type::Spot) {
        builder.spotLightCone(desc.spotInnerRadians, desc.spotOuterRadians);
    }
    builder.build(*engine_, e);
    scene_->addEntity(e);
    // removeLight で空いた席があれば再利用（番号を詰めない）。
    for (std::size_t i = 0; i < lightEntities_.size(); ++i) {
        if (lightEntities_[i].isNull()) {
            lightEntities_[i] = e;
            return i;
        }
    }
    lightEntities_.push_back(e);
    return lightEntities_.size() - 1;
}

void Renderer::removeLight(std::size_t index) {
    if (index >= lightEntities_.size()) return;
    utils::Entity& e = lightEntities_[index];
    if (e.isNull()) return;
    // removeShape と同じ後始末（engine_->destroy がコンポーネントも壊す）。
    scene_->remove(e);
    engine_->destroy(e);
    EntityManager::get().destroy(e);
    e = utils::Entity();  // 空席の印。addLight が再利用する
}

void Renderer::updateLight(std::size_t index, const float3& color,
                           float intensity, const float3& direction,
                           const float3& position) {
    if (index >= lightEntities_.size() || lightEntities_[index].isNull()) return;
    auto& lm = engine_->getLightManager();
    const auto li = lm.getInstance(lightEntities_[index]);
    if (!li) return;
    lm.setColor(li, color);
    lm.setIntensity(li, intensity);
    lm.setDirection(li, direction);
    // Harmless on a directional light (which ignores its position).
    lm.setPosition(li, position);
}

bool Renderer::loadEnvironment(const std::string& hdrName, float intensity) {
    // Decode + GPU prefilter live in EnvironmentLoader; this method only
    // installs the result in the scene and manages ownership of the previous
    // environment.
    const EnvironmentIBL env = loadEnvironmentIBL(*engine_, hdrName, intensity);
    scene_->setIndirectLight(env.light);
    // 背景（スカイボックス）は古いキューブマップを参照しているので、
    // 差し替える前に外す。
    if (skybox_) {
        scene_->setSkybox(nullptr);
        engine_->destroy(skybox_);
        skybox_ = nullptr;
    }
    if (ibl_) engine_->destroy(ibl_);
    if (iblTexture_) engine_->destroy(iblTexture_);
    ibl_ = env.light;
    iblTexture_ = env.reflections;
    envIntensity_ = intensity;
    refreshSkybox();
    return true;
}

void Renderer::setSkyboxEnabled(bool enabled) {
    if (skyboxWanted_ == enabled && (skybox_ != nullptr) == enabled) return;
    skyboxWanted_ = enabled;
    refreshSkybox();
}

// いまの環境マップから背景を作り直す（無効・環境マップ無しなら外す）。
// プリフィルタ済みのキューブマップをそのまま使う: mip 0 は元のパノラマ
// そのものなので、映り込みと背景が必ず一致する（別々に持つとずれる）。
void Renderer::refreshSkybox() {
    const bool want = skyboxWanted_ && iblTexture_ != nullptr;
    if (!want) {
        if (skybox_) {
            scene_->setSkybox(nullptr);
            engine_->destroy(skybox_);
            skybox_ = nullptr;
        }
        return;
    }
    Skybox* next = Skybox::Builder()
                       .environment(iblTexture_)
                       .intensity(envIntensity_)
                       .build(*engine_);
    if (!next) {
        LOGW("render", "skybox could not be built - keeping the flat background");
        return;
    }
    scene_->setSkybox(next);
    if (skybox_) engine_->destroy(skybox_);
    skybox_ = next;
}

}  // namespace wizengine
