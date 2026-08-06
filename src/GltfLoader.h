#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <math/mat4.h>
#include <math/quat.h>
#include <math/vec3.h>

namespace filament {
class Engine;
class Scene;
}  // namespace filament

namespace filament::gltfio {
class AssetLoader;
class FilamentAsset;
class FilamentInstance;
class MaterialProvider;
}  // namespace filament::gltfio

namespace wizengine {

// Loads glTF (.gltf) and binary glTF (.glb) files into a Filament scene via
// gltfio, which brings the model's own materials and textures with it.
//
// One instance owns the loader machinery and every asset loaded through it;
// destroying it releases all of them. The Renderer holds one and exposes
// addModel()/setModelTransform(), so the rest of the engine never sees gltfio.
//
// Built only when CMake found the gltfio libraries (WIZ_HAVE_GLTFIO). Without
// them add() reports the problem once and returns kInvalid, so a scene that
// asks for a model still runs - it just has no model in it.
class GltfLoader {
public:
    static constexpr std::size_t kInvalid = static_cast<std::size_t>(-1);

    GltfLoader(filament::Engine& engine, filament::Scene& scene);
    ~GltfLoader();

    GltfLoader(const GltfLoader&) = delete;
    GltfLoader& operator=(const GltfLoader&) = delete;

    // Loads a .gltf or .glb file (format detected from the contents) and adds
    // its entities to the scene. Returns an id for setTransform().
    //
    // THROWS AssetError when the file is missing, malformed, or glTF support
    // was not built. Failing here rather than returning an error code means a
    // new model added to scene.cpp is validated automatically - a caller
    // cannot forget to check.
    std::size_t add(const std::string& path);

    // Places a loaded model. Applied to the asset's root, so it moves the whole
    // hierarchy.
    void setTransform(std::size_t id, const filament::math::float3& position,
                      const filament::math::quatf& rotation, float scale);

    // Load one model and stamp out `count` copies of it. The mesh, materials
    // and textures are shared; each copy only carries its own transform, so
    // hundreds of them stay cheap. THROWS AssetError on failure.
    void createInstances(const std::string& path, std::size_t count);
    void setInstanceTransform(std::size_t index,
                              const filament::math::mat4f& transform);

    // Tint one instance towards a colour. amount 0 = untouched, 1 = strong.
    // Works on opaque materials (no alphaMode requirement): brightens the base
    // colour a little and adds the colour as emissive on top, so it reads
    // clearly even in shadow.
    void setInstanceTint(std::size_t index, const filament::math::float3& color,
                         float amount);
    std::size_t instanceCount() const { return instances_.size(); }

    // Largest dimension of the instanced model's bounding box, in the model's
    // own units. 0 when nothing is loaded. Lets the caller scale a model of
    // any size to a known physical size.
    float instancedModelSize() const { return instancedSize_; }

    std::size_t count() const { return assets_.size(); }

private:
    filament::Engine& engine_;
    filament::Scene& scene_;
    filament::gltfio::AssetLoader* loader_ = nullptr;
    filament::gltfio::MaterialProvider* materials_ = nullptr;
    std::vector<filament::gltfio::FilamentAsset*> assets_;
    std::vector<filament::gltfio::FilamentInstance*> instances_;
    float instancedSize_ = 0.0f;
};

}  // namespace wizengine
