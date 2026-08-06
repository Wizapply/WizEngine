#include "GltfLoader.h"
#include "Log.h"

#include "AssetError.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include <filament/Box.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/TransformManager.h>
#include <utils/EntityManager.h>

#ifdef WIZ_HAVE_GLTFIO
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentInstance.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/TextureProvider.h>
#include <gltfio/materials/uberarchive.h>
#endif

namespace wizengine {

namespace {

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

}  // namespace

#ifdef WIZ_HAVE_GLTFIO

GltfLoader::GltfLoader(filament::Engine& engine, filament::Scene& scene)
    : engine_(engine), scene_(scene) {
    // Ubershader materials: one prebuilt archive covering the glTF material
    // model, so no material compilation is needed at load time.
    materials_ = filament::gltfio::createUbershaderProvider(
        &engine_, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);

    filament::gltfio::AssetConfiguration config{};
    config.engine = &engine_;
    config.materials = materials_;
    config.entities = &utils::EntityManager::get();
    loader_ = filament::gltfio::AssetLoader::create(config);
}

GltfLoader::~GltfLoader() {
    for (auto* asset : assets_) {
        scene_.removeEntities(asset->getEntities(), asset->getEntityCount());
        loader_->destroyAsset(asset);
    }
    assets_.clear();
    filament::gltfio::AssetLoader::destroy(&loader_);
    delete materials_;
}

std::size_t GltfLoader::add(const std::string& pathIn) {
    const std::string path = wizengine::assetPath(pathIn);
    const std::vector<uint8_t> bytes = readFileBytes(path);
    if (bytes.empty()) {
        throw wizengine::AssetError(path, "model file is missing or empty");
    }

    // createAsset handles both .glb (binary) and .gltf (JSON) contents.
    filament::gltfio::FilamentAsset* asset =
        loader_->createAsset(bytes.data(), static_cast<uint32_t>(bytes.size()));
    if (!asset) {
        throw wizengine::AssetError(path, "is not a readable glTF/GLB file");
    }

    // Resolve external buffers/images. gltfPath tells the loader where the
    // file lives so relative URIs in a .gltf resolve; .glb has none.
    filament::gltfio::ResourceConfiguration rc{};
    rc.engine = &engine_;
    rc.gltfPath = path.c_str();
    rc.normalizeSkinningWeights = true;
    filament::gltfio::ResourceLoader resourceLoader(rc);
    resourceLoader.addTextureProvider(
        "image/png", filament::gltfio::createStbProvider(&engine_));
    resourceLoader.addTextureProvider(
        "image/jpeg", filament::gltfio::createStbProvider(&engine_));
    resourceLoader.addTextureProvider(
        "image/ktx2", filament::gltfio::createKtx2Provider(&engine_));
    if (!resourceLoader.loadResources(asset)) {
        throw wizengine::AssetError(
            path, "buffers or textures referenced by the model could not be loaded");
    }

    scene_.addEntities(asset->getEntities(), asset->getEntityCount());
    asset->releaseSourceData();  // the CPU-side glTF is no longer needed

    LOGI("model", "loaded '%s' (%zu entities)", path.c_str(),
                static_cast<std::size_t>(asset->getEntityCount()));
    assets_.push_back(asset);
    return assets_.size() - 1;
}

void GltfLoader::createInstances(const std::string& pathIn, std::size_t count) {
    if (count == 0) return;
    const std::string path = wizengine::assetPath(pathIn);
    const std::vector<uint8_t> bytes = readFileBytes(path);
    if (bytes.empty()) {
        throw wizengine::AssetError(path, "model file is missing or empty");
    }

    // One asset, many instances: geometry and materials are shared, so this is
    // far cheaper than loading the file N times.
    instances_.resize(count);
    filament::gltfio::FilamentAsset* asset = loader_->createInstancedAsset(
        bytes.data(), static_cast<uint32_t>(bytes.size()), instances_.data(),
        static_cast<size_t>(count));
    if (!asset) {
        instances_.clear();
        throw wizengine::AssetError(path, "is not a readable glTF/GLB file");
    }

    filament::gltfio::ResourceConfiguration rc{};
    rc.engine = &engine_;
    rc.gltfPath = path.c_str();
    rc.normalizeSkinningWeights = true;
    filament::gltfio::ResourceLoader resourceLoader(rc);
    resourceLoader.addTextureProvider(
        "image/png", filament::gltfio::createStbProvider(&engine_));
    resourceLoader.addTextureProvider(
        "image/jpeg", filament::gltfio::createStbProvider(&engine_));
    resourceLoader.addTextureProvider(
        "image/ktx2", filament::gltfio::createKtx2Provider(&engine_));
    if (!resourceLoader.loadResources(asset)) {
        throw wizengine::AssetError(
            path, "buffers or textures referenced by the model could not be loaded");
    }

    scene_.addEntities(asset->getEntities(), asset->getEntityCount());
    asset->releaseSourceData();
    assets_.push_back(asset);

    // Model's own size, so the scene can scale it to a physical size.
    const filament::Aabb box = asset->getBoundingBox();
    const filament::math::float3 extent = box.max - box.min;
    instancedSize_ = std::max(extent.x, std::max(extent.y, extent.z));

    LOGI("model", "loaded '%s' as %zu instances (model size %.4f)",
                path.c_str(), count, instancedSize_);
}

void GltfLoader::setInstanceTransform(std::size_t index,
                                      const filament::math::mat4f& transform) {
    if (index >= instances_.size()) return;
    auto& tcm = engine_.getTransformManager();
    tcm.setTransform(tcm.getInstance(instances_[index]->getRoot()), transform);
}

void GltfLoader::setInstanceTint(std::size_t index,
                                 const filament::math::float3& color,
                                 float amount) {
    if (index >= instances_.size()) return;
    auto* inst = instances_[index];

    // baseColorFactor multiplies the model's colour/texture: pushing it above
    // 1 brightens without changing hue. The emissive term carries the actual
    // highlight colour and shows up regardless of lighting, so the grabbed
    // object reads clearly even in shadow.
    const float brighten = 1.0f + amount * 0.5f;
    const filament::math::float4 base{brighten, brighten, brighten, 1.0f};
    const filament::math::float3 emissive{color.x * amount * 0.8f,
                                          color.y * amount * 0.8f,
                                          color.z * amount * 0.8f};

    filament::MaterialInstance* const* mis = inst->getMaterialInstances();
    for (std::size_t i = 0, n = inst->getMaterialInstanceCount(); i < n; ++i) {
        mis[i]->setParameter("baseColorFactor", base);
        // Not every ubershader variant exposes emissiveFactor; ignore failures
        // by only setting it when the material declares it.
        if (mis[i]->getMaterial()->hasParameter("emissiveFactor")) {
            mis[i]->setParameter("emissiveFactor",
                                 filament::math::float4{emissive.x, emissive.y,
                                                        emissive.z, 1.0f});
        }
    }
}

void GltfLoader::setTransform(std::size_t id,
                              const filament::math::float3& position,
                              const filament::math::quatf& rotation,
                              float scale) {
    if (id >= assets_.size()) return;
    auto& tcm = engine_.getTransformManager();
    const auto instance = tcm.getInstance(assets_[id]->getRoot());
    const filament::math::mat4f m =
        filament::math::mat4f::translation(position) *
        filament::math::mat4f(rotation) * filament::math::mat4f::scaling(scale);
    tcm.setTransform(instance, m);
}

#else  // no gltfio in this build

GltfLoader::GltfLoader(filament::Engine& engine, filament::Scene& scene)
    : engine_(engine), scene_(scene) {}

GltfLoader::~GltfLoader() = default;

std::size_t GltfLoader::add(const std::string& path) {
    throw wizengine::AssetError(
        path, "this build has no glTF support (gltfio libraries were not found "
              "at configure time)");
}

void GltfLoader::setTransform(std::size_t, const filament::math::float3&,
                              const filament::math::quatf&, float) {}

void GltfLoader::createInstances(const std::string& path, std::size_t) {
    throw wizengine::AssetError(
        path, "this build has no glTF support (gltfio libraries were not found "
              "at configure time)");
}

void GltfLoader::setInstanceTransform(std::size_t,
                                      const filament::math::mat4f&) {}

void GltfLoader::setInstanceTint(std::size_t,
                                 const filament::math::float3&, float) {}

#endif

}  // namespace wizengine
