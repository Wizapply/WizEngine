#include "Renderer.h"
#include "Log.h"

#include <filament/Camera.h>
#include <filament/Color.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/IndirectLight.h>
#include <filament/Texture.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <backend/PixelBufferDescriptor.h>
#include <geometry/SurfaceOrientation.h>
#include <utils/EntityManager.h>

#include <math/vec3.h>
#include <math/vec4.h>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <vector>

#include "GltfLoader.h"
#include "AssetError.h"
#include "EnvironmentLoader.h"
#include "ImageLoader.h"
#include <iterator>
#include <stdexcept>

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;

namespace {

// 24 vertices: 4 per face, so each face can carry its own normal (needed for
// lit shading). Faces are wound CCW when viewed from outside.
const float3 kCubePos[24] = {
    // front (+Z)
    {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
    // back (-Z)
    {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
    // left (-X)
    {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f},
    // right (+X)
    {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f},
    // top (+Y)
    {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
    // bottom (-Y)
    {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f},
};

// Per-face normals, in the same order as kCubePos (front, back, left, right,
// top, bottom). Used to build the tangent frames a lit material needs.
const float3 kCubeNrm[24] = {
    {0, 0, 1},  {0, 0, 1},  {0, 0, 1},  {0, 0, 1},   // front (+Z)
    {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},  // back (-Z)
    {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0},  // left (-X)
    {1, 0, 0},  {1, 0, 0},  {1, 0, 0},  {1, 0, 0},   // right (+X)
    {0, 1, 0},  {0, 1, 0},  {0, 1, 0},  {0, 1, 0},   // top (+Y)
    {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0},  // bottom (-Y)
};

// Flat ground quad at y=0, facing +Y. Positions are built per-size in
// addGround(); here we keep the shared normals and indices.
const float3 kGroundNrm[4] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
const uint16_t kGroundIdx[6] = {0, 2, 1, 0, 3, 2};

// Two triangles per face: (0,1,2) (2,3,0), offset by 4*face.
const uint16_t kCubeIdx[36] = {
    0, 1, 2, 2, 3, 0,        4, 5, 6, 6, 7, 4,
    8, 9, 10, 10, 11, 8,     12, 13, 14, 14, 15, 12,
    16, 17, 18, 18, 19, 16,  20, 21, 22, 22, 23, 20,
};

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open material package: " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

}  // namespace

namespace wizengine {

Renderer::Renderer(int width, int height, const std::string& materialPath)
    : width_(width), height_(height) {
    // The builder picks a default backend. For headless servers where the
    // default GL context fails, force Vulkan with the env var
    // FILAMENT_BACKEND=vulkan or .backend(Engine::Backend::VULKAN) here.
    //
    // driverHandleArenaSizeMB: the handle arena holds every backend object
    // handle (buffers, textures, render targets, ...). The platform default
    // is not sized for this scene - 512 renderables, three views with their
    // swap chains, and the IBL prefilter's per-mip render targets - so
    // Filament warns ("HandleAllocator arena is full") and falls back to
    // slower heap allocations. Empirically 32 was still not enough on the
    // Vulkan backend (its handle structs are large); 128 MiB is where the
    // warning stops for this scene. The cost is just that much reserved
    // memory - trim it if the scene ever shrinks.
    Engine::Config engineConfig = {};
    engineConfig.driverHandleArenaSizeMB = 128;
    engine_ = Engine::Builder().config(&engineConfig).build();

    renderer_ = engine_->createRenderer();
    scene_ = engine_->createScene();
    addView();  // view 0 - always present

    // Solid clear colour (linear RGBA).
    filament::Renderer::ClearOptions clear;
    clear.clear = true;
    clear.clearColor = {0.10f, 0.12f, 0.15f, 1.0f};
    renderer_->setClearOptions(clear);

    // Box material (lit), shared by all box renderables. Scene sets the colour.
    wizengine::requireFile(materialPath, "object material");
    const auto pkg = readFile(assetPath(materialPath));
    if (pkg.empty()) throw AssetError(assetPath(materialPath), "is empty");
    material_ = Material::Builder().package(pkg.data(), pkg.size()).build(*engine_);
    if (!material_) {
        throw AssetError(assetPath(materialPath), "is not a valid .filamat");
    }
    matInstance_ = material_->createInstance();
    matInstance_->setParameter("baseColor", RgbType::LINEAR,
                               float3{0.80f, 0.36f, 0.18f});

    // Shared cube mesh for boxes: positions + tangent frames (for lit shading).
    auto* cubeOrient = filament::geometry::SurfaceOrientation::Builder()
                           .vertexCount(24)
                           .normals(kCubeNrm)
                           .build();
    cubeOrient->getQuats(cubeTangents_, 24);
    delete cubeOrient;

    vb_ = VertexBuffer::Builder()
              .vertexCount(24)
              .bufferCount(2)
              .attribute(VertexAttribute::POSITION, 0,
                         VertexBuffer::AttributeType::FLOAT3)
              .attribute(VertexAttribute::TANGENTS, 1,
                         VertexBuffer::AttributeType::FLOAT4)
              .build(*engine_);
    vb_->setBufferAt(*engine_, 0,
                     VertexBuffer::BufferDescriptor(kCubePos, sizeof(kCubePos)));
    vb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(cubeTangents_, sizeof(cubeTangents_)));

    ib_ = IndexBuffer::Builder()
              .indexCount(36)
              .bufferType(IndexBuffer::IndexType::USHORT)
              .build(*engine_);
    ib_->setBuffer(*engine_,
                   IndexBuffer::BufferDescriptor(kCubeIdx, sizeof(kCubeIdx)));

    // Lit material for the ground (loaded now; the ground geometry is created
    // later in addGround). Boxes and ground are added by the Scene.
    wizengine::requireFile("ground_lit.filamat", "ground material");
    const auto gpkg = readFile(assetPath("ground_lit.filamat"));
    if (gpkg.empty()) {
        throw AssetError(assetPath("ground_lit.filamat"), "is empty");
    }
    groundMaterial_ =
        Material::Builder().package(gpkg.data(), gpkg.size()).build(*engine_);
    if (!groundMaterial_) {
        throw AssetError(assetPath("ground_lit.filamat"),
                         "is not a valid .filamat");
    }
    groundMatInstance_ = groundMaterial_->createInstance();

    // Direct lights are no longer created here: the scene owns them as
    // LightObjects (scene.cpp, lightConfigs()) and adds them through
    // addLight() during Scene::build. Only the ambient below is built in,
    // so a scene with no lights configured still isn't pitch black.

    // Uniform ambient (constant SH, no environment map) so shadowed areas of the
    // lit ground are a soft gray instead of pure black. Tune intensity to taste:
    // higher = lighter shadows, lower = darker.
    const float3 ambientSH[1] = {float3{1.0f, 1.0f, 1.05f}};
    ibl_ = filament::IndirectLight::Builder()
               .irradiance(1, ambientSH)
               .intensity(30000.0f)
               .build(*engine_);
    scene_->setIndirectLight(ibl_);

}

std::size_t Renderer::addBox() {
    // A cube renderable sharing the mesh and lit material. It casts shadows
    // onto the ground and onto other boxes, and receives them too. Its
    // transform is set each frame by the Scene.
    utils::Entity e = EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {1, 1, 1}})
        .material(0, matInstance_)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vb_, ib_, 0, 36)
        .culling(true)  // frustum-cull off-screen boxes (matters at high counts)
        .castShadows(true)
        .receiveShadows(true)
        .build(*engine_, e);
    scene_->addEntity(e);
    boxEntities_.push_back(e);
    return boxEntities_.size() - 1;
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
        .castShadows(desc.castShadows);
    if (desc.type != LightDesc::Type::Directional) {
        builder.position(desc.position).falloff(desc.falloffRadius);
    }
    if (desc.type == LightDesc::Type::Spot) {
        builder.spotLightCone(desc.spotInnerRadians, desc.spotOuterRadians);
    }
    builder.build(*engine_, e);
    scene_->addEntity(e);
    lightEntities_.push_back(e);
    return lightEntities_.size() - 1;
}

void Renderer::updateLight(std::size_t index, const float3& color,
                           float intensity, const float3& direction,
                           const float3& position) {
    if (index >= lightEntities_.size()) return;
    auto& lm = engine_->getLightManager();
    const auto li = lm.getInstance(lightEntities_[index]);
    if (!li) return;
    lm.setColor(li, color);
    lm.setIntensity(li, intensity);
    lm.setDirection(li, direction);
    // Harmless on a directional light (which ignores its position).
    lm.setPosition(li, position);
}

std::size_t Renderer::addModel(const std::string& path) {
    // Created on first use: an engine that never loads a model pays nothing.
    if (!gltf_) gltf_ = std::make_unique<GltfLoader>(*engine_, *scene_);
    return gltf_->add(path);  // throws AssetError on failure
}

void Renderer::createModelInstances(const std::string& path,
                                    std::size_t count) {
    if (!gltf_) gltf_ = std::make_unique<GltfLoader>(*engine_, *scene_);
    gltf_->createInstances(path, count);  // throws AssetError on failure
}

void Renderer::setModelInstanceTransform(
    std::size_t index, const filament::math::mat4f& transform) {
    if (gltf_) gltf_->setInstanceTransform(index, transform);
}

bool Renderer::loadEnvironment(const std::string& hdrName, float intensity) {
    // Decode + GPU prefilter live in EnvironmentLoader; this method only
    // installs the result in the scene and manages ownership of the previous
    // environment.
    const EnvironmentIBL env = loadEnvironmentIBL(*engine_, hdrName, intensity);
    scene_->setIndirectLight(env.light);
    if (ibl_) engine_->destroy(ibl_);
    if (iblTexture_) engine_->destroy(iblTexture_);
    ibl_ = env.light;
    iblTexture_ = env.reflections;
    return true;
}

void Renderer::configureGrabLines(
    const std::vector<filament::math::float3>& colors) {
    if (!lineMaterial_) {
        const auto pkg = readFile(assetPath("line.filamat"));
        if (pkg.empty()) {
            LOGW("render", "grab line: line.filamat not found - lines disabled");
            return;
        }
        lineMaterial_ =
            Material::Builder().package(pkg.data(), pkg.size()).build(*engine_);
    }

    for (std::size_t i = 0; i < colors.size(); ++i) {
        GrabLine line;
        // Two vertices, rewritten every frame; the index buffer never changes.
        line.vb = VertexBuffer::Builder()
                      .vertexCount(2)
                      .bufferCount(1)
                      .attribute(VertexAttribute::POSITION, 0,
                                 VertexBuffer::AttributeType::FLOAT3, 0,
                                 sizeof(float) * 3)
                      .build(*engine_);
        static const uint16_t kIndices[2] = {0, 1};
        line.ib = IndexBuffer::Builder()
                      .indexCount(2)
                      .bufferType(IndexBuffer::IndexType::USHORT)
                      .build(*engine_);
        line.ib->setBuffer(*engine_,
                           IndexBuffer::BufferDescriptor(kIndices,
                                                         sizeof(kIndices),
                                                         nullptr));

        line.mi = lineMaterial_->createInstance();
        line.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                              float4{colors[i].x, colors[i].y, colors[i].z,
                                     1.0f});

        line.entity = EntityManager::get().create();
        RenderableManager::Builder(1)
            .boundingBox({{-1000.0f, -1000.0f, -1000.0f},
                          {1000.0f, 1000.0f, 1000.0f}})  // never culled
            .geometry(0, RenderableManager::PrimitiveType::LINES, line.vb,
                      line.ib, 0, 2)
            .material(0, line.mi)
            .culling(false)
            .castShadows(false)
            .receiveShadows(false)
            .build(*engine_, line.entity);

        grabLines_.push_back(line);  // added to the scene only while visible
    }
}

void Renderer::setGrabLine(std::size_t index,
                           const filament::math::float3& from,
                           const filament::math::float3& to, bool visible) {
    if (index >= grabLines_.size()) return;
    GrabLine& line = grabLines_[index];

    if (!visible) {
        if (line.inScene) {
            scene_->remove(line.entity);
            line.inScene = false;
        }
        return;
    }

    // The buffer must outlive the call: Filament copies it asynchronously, so
    // hand over heap memory and free it from the completion callback.
    auto* verts = new float[6]{from.x, from.y, from.z, to.x, to.y, to.z};
    line.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            verts, sizeof(float) * 6,
            [](void* buffer, size_t, void*) {
                delete[] static_cast<float*>(buffer);
            }));

    if (!line.inScene) {
        scene_->addEntity(line.entity);
        line.inScene = true;
    }
}

void Renderer::setModelInstanceTint(std::size_t index,
                                    const filament::math::float3& color,
                                    float amount) {
    if (gltf_) gltf_->setInstanceTint(index, color, amount);
}

float Renderer::modelInstanceSize() const {
    return gltf_ ? gltf_->instancedModelSize() : 0.0f;
}

void Renderer::setModelTransform(std::size_t id,
                                 const filament::math::float3& position,
                                 const filament::math::quatf& rotation,
                                 float scale) {
    if (gltf_ && id != kInvalidModel) {
        gltf_->setTransform(id, position, rotation, scale);
    }
}

void Renderer::setBoxColor(const filament::math::float3& color) {
    matInstance_->setParameter("baseColor", RgbType::LINEAR, color);
}

void Renderer::configureHighlightColors(
    const std::vector<filament::math::float3>& colors) {
    for (auto* mi : highlightInstances_) engine_->destroy(mi);
    highlightInstances_.clear();
    // One instance per camera: same material, different baseColor. Swapping a
    // box between them is just a parameter change for the renderer.
    for (const auto& c : colors) {
        auto* mi = material_->createInstance();
        mi->setParameter("baseColor", RgbType::LINEAR, c);
        highlightInstances_.push_back(mi);
    }
}

float Renderer::verticalFovDegrees() const {
    return 45.0f;  // must match the setProjection call in the constructor
}

void Renderer::setBoxHighlighted(std::size_t id, int styleIndex) {
    if (id >= boxEntities_.size()) return;
    if (boxHighlightStyle_.size() != boxEntities_.size()) {
        boxHighlightStyle_.resize(boxEntities_.size(), -1);
    }
    if (styleIndex >= int(highlightInstances_.size())) styleIndex = -1;
    if (boxHighlightStyle_[id] == styleIndex) return;
    boxHighlightStyle_[id] = styleIndex;

    auto& rm = engine_->getRenderableManager();
    rm.setMaterialInstanceAt(
        rm.getInstance(boxEntities_[id]), 0,
        styleIndex < 0 ? matInstance_ : highlightInstances_[styleIndex]);
}

void Renderer::addGround(float halfSize, const filament::math::float3& color,
                         float tileMeters, const std::string& texturePath) {
    groundMatInstance_->setParameter("baseColor", RgbType::LINEAR, color);

    // ---- Ground texture --------------------------------------------------
    // An image file (PNG/JPEG/TGA/BMP) when the scene names one - and then it
    // must load, or the run stops. Only when no texture is configured at all
    // does the ground fall back to a generated 2 x 2 checkerboard. Image
    // colours are sRGB; the checker is a linear multiplier on baseColor.
    std::vector<uint8_t> pixels;
    int texW = 0;
    int texH = 0;
    const bool fromFile =
        !texturePath.empty() &&
        loadImageRGBA(assetPath(texturePath), pixels, texW, texH);
    if (!texturePath.empty() && !fromFile) {
        // A named texture that will not decode is a configuration error, not
        // something to paper over with the checkerboard.
        throw AssetError(assetPath(texturePath),
                         "ground texture is missing or is not a readable image");
    }
    if (fromFile) {
        LOGI("render", "ground texture: %s (%dx%d)",
             assetPath(texturePath).c_str(), texW, texH);
    } else {
        if (!texturePath.empty()) {
            LOGW("render", "ground texture '%s' not found, using generated checker",
                 texturePath.c_str());
        }
        texW = texH = 256;  // 2 x 2 squares, 128 px each
        pixels.resize(size_t(texW) * size_t(texH) * 4);
        for (int y = 0; y < texH; ++y) {
            for (int x = 0; x < texW; ++x) {
                const bool dark = (x < texW / 2) != (y < texH / 2);
                const uint8_t v = dark ? 105 : 255;
                uint8_t* p = pixels.data() + (size_t(y) * texW + size_t(x)) * 4;
                p[0] = v;
                p[1] = v;
                p[2] = v;
                p[3] = 255;
            }
        }
    }

    // Mip level count from the larger dimension.
    uint8_t levels = 1;
    for (int m = (texW > texH ? texW : texH); m > 1; m >>= 1) ++levels;

    groundTexture_ =
        filament::Texture::Builder()
            .width(uint32_t(texW))
            .height(uint32_t(texH))
            .levels(levels)
            .format(fromFile ? filament::Texture::InternalFormat::SRGB8_A8
                             : filament::Texture::InternalFormat::RGBA8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .build(*engine_);

    // Upload every mip level ourselves (simple box filter). Filament's
    // generateMipmaps() needs a texture usage flag whose name varies between
    // versions, so building the chain by hand keeps this version-proof.
    std::vector<uint8_t> mip = pixels;
    int w = texW;
    int h = texH;
    for (int level = 0; level < int(levels); ++level) {
        const size_t bytes = size_t(w) * size_t(h) * 4;
        auto* buf = new uint8_t[bytes];
        std::memcpy(buf, mip.data(), bytes);
        filament::Texture::PixelBufferDescriptor pb(
            buf, bytes, filament::Texture::Format::RGBA,
            filament::Texture::Type::UBYTE,
            [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); });
        groundTexture_->setImage(*engine_, uint8_t(level), std::move(pb));

        if (w == 1 && h == 1) break;
        const int nw = (w > 1) ? w / 2 : 1;
        const int nh = (h > 1) ? h / 2 : 1;
        std::vector<uint8_t> next(size_t(nw) * size_t(nh) * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                const int x0 = (w > 1) ? 2 * x : 0;
                const int y0 = (h > 1) ? 2 * y : 0;
                const int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
                const int y1 = (y0 + 1 < h) ? y0 + 1 : y0;
                for (int c = 0; c < 4; ++c) {
                    const int sum = mip[(size_t(y0) * w + x0) * 4 + c] +
                                    mip[(size_t(y0) * w + x1) * 4 + c] +
                                    mip[(size_t(y1) * w + x0) * 4 + c] +
                                    mip[(size_t(y1) * w + x1) * 4 + c];
                    next[(size_t(y) * nw + size_t(x)) * 4 + c] = uint8_t(sum / 4);
                }
            }
        }
        mip.swap(next);
        w = nw;
        h = nh;
    }

    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    sampler.setAnisotropy(8.0f);
    groundMatInstance_->setParameter("checker", groundTexture_, sampler);

    // Flat quad at y=0 facing +Y, sized to halfSize. Positions/UVs kept in
    // members (Filament references, not copies).
    groundPos_[0] = {-halfSize, 0.0f, -halfSize};
    groundPos_[1] = {halfSize, 0.0f, -halfSize};
    groundPos_[2] = {halfSize, 0.0f, halfSize};
    groundPos_[3] = {-halfSize, 0.0f, halfSize};

    // One texture repeat spans tileMeters metres.
    const float tiles = (tileMeters > 0.0f) ? (2.0f * halfSize) / tileMeters : 1.0f;
    groundUv_[0] = {0.0f, 0.0f};
    groundUv_[1] = {tiles, 0.0f};
    groundUv_[2] = {tiles, tiles};
    groundUv_[3] = {0.0f, tiles};

    auto* groundOrient = filament::geometry::SurfaceOrientation::Builder()
                             .vertexCount(4)
                             .normals(kGroundNrm)
                             .build();
    groundOrient->getQuats(groundTangents_, 4);
    delete groundOrient;

    groundVb_ = VertexBuffer::Builder()
                    .vertexCount(4)
                    .bufferCount(3)
                    .attribute(VertexAttribute::POSITION, 0,
                               VertexBuffer::AttributeType::FLOAT3)
                    .attribute(VertexAttribute::TANGENTS, 1,
                               VertexBuffer::AttributeType::FLOAT4)
                    .attribute(VertexAttribute::UV0, 2,
                               VertexBuffer::AttributeType::FLOAT2)
                    .build(*engine_);
    groundVb_->setBufferAt(*engine_, 0,
                           VertexBuffer::BufferDescriptor(groundPos_, sizeof(groundPos_)));
    groundVb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(groundTangents_, sizeof(groundTangents_)));
    groundVb_->setBufferAt(*engine_, 2,
                           VertexBuffer::BufferDescriptor(groundUv_, sizeof(groundUv_)));

    groundIb_ = IndexBuffer::Builder()
                    .indexCount(6)
                    .bufferType(IndexBuffer::IndexType::USHORT)
                    .build(*engine_);
    groundIb_->setBuffer(
        *engine_, IndexBuffer::BufferDescriptor(kGroundIdx, sizeof(kGroundIdx)));

    groundEntity_ = EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {halfSize, 0.1f, halfSize}})
        .material(0, groundMatInstance_)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, groundVb_, groundIb_, 0, 6)
        .culling(false)
        .castShadows(false)
        .receiveShadows(true)
        .build(*engine_, groundEntity_);
    scene_->addEntity(groundEntity_);
}

std::size_t Renderer::addView() {
    ViewSlot slot;
    slot.width = width_;
    slot.height = height_;
    // CONFIG_READABLE lets us read the framebuffer back with readPixels().
    // Every view needs its own swap chain because each one is read back and
    // encoded separately.
    slot.swapChain = engine_->createSwapChain(
        uint32_t(slot.width), uint32_t(slot.height), SwapChain::CONFIG_READABLE);

    slot.cameraEntity = EntityManager::get().create();
    slot.camera = engine_->createCamera(slot.cameraEntity);
    const double aspect = double(slot.width) / double(slot.height);
    slot.camera->setProjection(45.0, aspect, 0.1, 200.0, Camera::Fov::VERTICAL);
    slot.camera->lookAt({7.0, 5.0, 9.0}, {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0});

    slot.view = engine_->createView();
    slot.view->setScene(scene_);   // all views share one scene
    slot.view->setCamera(slot.camera);
    slot.view->setViewport({0, 0, uint32_t(slot.width), uint32_t(slot.height)});

    for (auto& cap : slot.captures) {
        cap.pixels.resize(std::size_t(slot.width) * std::size_t(slot.height) * 4);
        cap.ready = std::make_shared<std::atomic<bool>>(false);
    }

    views_.push_back(std::move(slot));
    return views_.size() - 1;
}

void Renderer::requestViewResize(std::size_t viewIndex, int width, int height) {
    if (viewIndex >= views_.size() || width <= 0 || height <= 0) return;
    ViewSlot& slot = views_[viewIndex];
    if (width == slot.width && height == slot.height) {
        slot.pendingW = slot.pendingH = 0;  // nothing to do (or cancel)
        return;
    }
    slot.pendingW = width;
    slot.pendingH = height;
}

void Renderer::setCamera(const filament::math::double3& eye,
                         const filament::math::double3& target) {
    setCamera(0, eye, target);
}

void Renderer::setCamera(std::size_t viewIndex,
                         const filament::math::double3& eye,
                         const filament::math::double3& target) {
    if (viewIndex >= views_.size()) return;
    views_[viewIndex].camera->lookAt(eye, target, {0.0, 1.0, 0.0});
}

Renderer::~Renderer() {
    if (!engine_) return;
    finishPendingReadbacks();  // no buffer may be freed while the GPU has it
    for (auto e : boxEntities_) {
        scene_->remove(e);
        engine_->destroy(e);
    }
    scene_->remove(groundEntity_);
    engine_->destroy(groundEntity_);
    for (auto e : lightEntities_) {
        scene_->remove(e);
        engine_->destroy(e);
    }
    gltf_.reset();  // models must go before the engine
    engine_->destroy(ibl_);
    if (iblTexture_) engine_->destroy(iblTexture_);
    engine_->destroy(groundTexture_);
    engine_->destroy(matInstance_);
    for (auto& line : grabLines_) {
        if (line.inScene) scene_->remove(line.entity);
        engine_->destroy(line.entity);
        engine_->destroy(line.vb);
        engine_->destroy(line.ib);
        engine_->destroy(line.mi);
    }
    grabLines_.clear();
    if (lineMaterial_) engine_->destroy(lineMaterial_);
    for (auto* mi : highlightInstances_) engine_->destroy(mi);
    engine_->destroy(material_);
    engine_->destroy(groundMatInstance_);
    engine_->destroy(groundMaterial_);
    engine_->destroy(vb_);
    engine_->destroy(groundVb_);
    engine_->destroy(ib_);
    engine_->destroy(groundIb_);
    for (auto& slot : views_) {
        engine_->destroyCameraComponent(slot.cameraEntity);
        engine_->destroy(slot.cameraEntity);
        engine_->destroy(slot.view);
    }
    engine_->destroy(scene_);
    engine_->destroy(renderer_);
    for (auto& slot : views_) engine_->destroy(slot.swapChain);
    views_.clear();
    Engine::destroy(&engine_);
}

void Renderer::setBoxTransform(std::size_t i, const filament::math::mat4f& transform) {
    auto& tcm = engine_->getTransformManager();
    tcm.setTransform(tcm.getInstance(boxEntities_[i]), transform);
}

void Renderer::renderFrame(
    const std::function<void(const uint8_t*, size_t)>& onFrame) {
    renderFrame(0, onFrame);
}

void Renderer::renderFrame(
    std::size_t viewIndex,
    const std::function<void(const uint8_t*, size_t)>& onFrame) {
    if (viewIndex >= views_.size()) return;
    ViewSlot& slot = views_[viewIndex];

    // Deliver the previous frame first, if the GPU has finished with it. Doing
    // this before rendering means the encoder gets work while the new frame is
    // still being drawn.
    // Oldest first, so frames reach the encoder in the order they were drawn.
    while (!slot.pending.empty()) {
        ViewSlot::Capture& done = slot.captures[slot.pending.front()];
        if (!done.ready->load(std::memory_order_acquire)) break;  // still copying
        onFrame(done.pixels.data(), done.pixels.size());
        done.ready->store(false, std::memory_order_release);
        done.inFlight = false;
        slot.pending.pop_front();
    }

    // A requested resize is applied only when no readback is in flight: the
    // GPU may still be copying into the old-size buffers. Until then no new
    // capture is started, so the queue drains within a frame or two.
    const bool resizePending = slot.pendingW != 0;
    if (resizePending && slot.pending.empty()) {
        slot.width = slot.pendingW;
        slot.height = slot.pendingH;
        slot.pendingW = slot.pendingH = 0;
        engine_->destroy(slot.swapChain);
        slot.swapChain = engine_->createSwapChain(
            uint32_t(slot.width), uint32_t(slot.height),
            SwapChain::CONFIG_READABLE);
        slot.view->setViewport(
            {0, 0, uint32_t(slot.width), uint32_t(slot.height)});
        // Same fov/near/far as addView - only the aspect follows the size.
        slot.camera->setProjection(45.0,
                                   double(slot.width) / double(slot.height),
                                   0.1, 200.0, Camera::Fov::VERTICAL);
        for (auto& cap : slot.captures) {
            cap.pixels.assign(
                std::size_t(slot.width) * std::size_t(slot.height) * 4, 0);
            cap.inFlight = false;
            cap.ready->store(false, std::memory_order_release);
        }
        slot.next = 0;
        LOGI("render", "view %zu resized to %dx%d", viewIndex, slot.width,
             slot.height);
    }

    if (!renderer_->beginFrame(slot.swapChain)) return;
    renderer_->render(slot.view);

    // Start a readback into whichever buffer is free. If both are still with
    // the GPU we skip the capture for this frame rather than blocking - the
    // stream drops a frame, which is far better than stalling the CPU.
    ViewSlot::Capture& cap = slot.captures[slot.next];
    if (!cap.inFlight && !slot.pendingW) {
        using namespace filament::backend;
        cap.ready->store(false, std::memory_order_release);
        // The flag is shared with the callback so a late completion after the
        // renderer is gone cannot touch freed memory.
        auto* flag = new std::shared_ptr<std::atomic<bool>>(cap.ready);
        PixelBufferDescriptor pbd(
            cap.pixels.data(), cap.pixels.size(), PixelDataFormat::RGBA,
            PixelDataType::UBYTE,
            [](void* /*buffer*/, size_t /*size*/, void* user) {
                auto* held = static_cast<std::shared_ptr<std::atomic<bool>>*>(user);
                (*held)->store(true, std::memory_order_release);
                delete held;
            },
            flag);

        // Reads the framebuffer back to CPU. On the Vulkan backend the data is
        // top-down, so no flip is applied downstream (see VideoStreamer).
        renderer_->readPixels(0, 0, uint32_t(slot.width),
                              uint32_t(slot.height), std::move(pbd));
        cap.inFlight = true;
        slot.pending.push_back(slot.next);
        slot.next = (slot.next + 1) % ViewSlot::kCaptureBuffers;
    }

    // No flushAndWait() here: that call was the stall. The frame is delivered
    // on a later call, once the copy has landed.
    renderer_->endFrame();
}

void Renderer::finishPendingReadbacks() {
    if (!engine_) return;
    engine_->flushAndWait();  // shutdown only: every callback has now fired
    for (auto& slot : views_) {
        for (auto& cap : slot.captures) cap.inFlight = false;
        slot.pending.clear();
    }
}

}  // namespace wizengine
