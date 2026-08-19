#include "GltfLoader.h"
#include "Log.h"

#include "AssetError.h"

#include <algorithm>
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
    // 動的に作った実体のエンティティも外してからアセットを壊す
    // （getEntities が後から作った実体を含むかは版に依るので、両方外す。
    // シーンに無いエンティティの remove は無害）。
    for (auto* inst : instances_) {
        if (inst) scene_.removeEntities(inst->getEntities(), inst->getEntityCount());
    }
    for (auto& m : models_) {
        if (!m.asset) continue;
        scene_.removeEntities(m.asset->getEntities(), m.asset->getEntityCount());
        loader_->destroyAsset(m.asset);
    }
    models_.clear();
    filament::gltfio::AssetLoader::destroy(&loader_);
    delete materials_;
}

std::size_t GltfLoader::loadModel(const std::string& pathIn) {
    const std::string path = wizengine::assetPath(pathIn);
    for (std::size_t i = 0; i < models_.size(); ++i) {
        if (models_[i].path == path) return i;  // 同じファイルは 1 回だけ
    }

    const std::vector<uint8_t> bytes = readFileBytes(path);
    if (bytes.empty()) {
        throw wizengine::AssetError(path, "model file is missing or empty");
    }

    // 原型はインスタンス化アセットとして作る（あとから createInstance で
    // 実体を増やせるのはこの形だけ）。最初の 1 実体は「まだ誰の物でもない」
    // ので、スケール 0 で隠して空きリストに入れておく。
    filament::gltfio::FilamentInstance* first = nullptr;
    filament::gltfio::FilamentAsset* asset = loader_->createInstancedAsset(
        bytes.data(), static_cast<uint32_t>(bytes.size()), &first, 1);
    if (!asset || !first) {
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

    Model m;
    m.asset = asset;
    m.path = path;
    const filament::Aabb box = asset->getBoundingBox();
    const filament::math::float3 extent = box.max - box.min;
    m.size = std::max(extent.x, std::max(extent.y, extent.z));

    // 最初の実体を空きとして登録（スケール 0 で見えない）。
    const std::size_t firstIndex = instances_.size();
    instances_.push_back(first);
    instanceModel_.push_back(models_.size());
    {
        auto& tcm = engine_.getTransformManager();
        tcm.setTransform(tcm.getInstance(first->getRoot()),
                         filament::math::mat4f::scaling(
                             filament::math::float3{0.0f}));
    }
    m.freeInstances.push_back(firstIndex);

    models_.push_back(std::move(m));
    LOGI("model", "loaded '%s' (size %.4f)", path.c_str(),
         models_.back().size);
    return models_.size() - 1;
}

float GltfLoader::modelSize(std::size_t modelId) const {
    return modelId < models_.size() ? models_[modelId].size : 0.0f;
}

std::size_t GltfLoader::createInstance(std::size_t modelId) {
    if (modelId >= models_.size()) return kInvalid;
    Model& m = models_[modelId];

    // 空き（release 済み）があれば再利用。姿勢は呼び出し側が毎フレーム
    // 入れるので、ここではスケール 0 のままでよい（原点で 1 フレーム光る
    // より、遅れて現れる方がまし）。
    if (!m.freeInstances.empty()) {
        const std::size_t index = m.freeInstances.back();
        m.freeInstances.pop_back();
        return index;
    }

    filament::gltfio::FilamentInstance* inst = loader_->createInstance(m.asset);
    if (!inst) return kInvalid;
    scene_.addEntities(inst->getEntities(), inst->getEntityCount());
    instances_.push_back(inst);
    instanceModel_.push_back(modelId);
    return instances_.size() - 1;
}

void GltfLoader::releaseInstance(std::size_t index) {
    if (index >= instances_.size()) return;
    auto& tcm = engine_.getTransformManager();
    tcm.setTransform(tcm.getInstance(instances_[index]->getRoot()),
                     filament::math::mat4f::scaling(
                         filament::math::float3{0.0f}));
    // 二重 release を空きリストに二度積まない。
    auto& free = models_[instanceModel_[index]].freeInstances;
    if (std::find(free.begin(), free.end(), index) == free.end()) {
        free.push_back(index);
    }
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

#else  // no gltfio in this build

GltfLoader::GltfLoader(filament::Engine& engine, filament::Scene& scene)
    : engine_(engine), scene_(scene) {}

GltfLoader::~GltfLoader() = default;

std::size_t GltfLoader::loadModel(const std::string& path) {
    throw wizengine::AssetError(
        path, "this build has no glTF support (gltfio libraries were not found "
              "at configure time)");
}

float GltfLoader::modelSize(std::size_t) const { return 0.0f; }

std::size_t GltfLoader::createInstance(std::size_t) { return kInvalid; }

void GltfLoader::releaseInstance(std::size_t) {}

void GltfLoader::setInstanceTransform(std::size_t,
                                      const filament::math::mat4f&) {}

void GltfLoader::setInstanceTint(std::size_t,
                                 const filament::math::float3&, float) {}

#endif

}  // namespace wizengine
