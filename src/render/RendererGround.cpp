// Renderer の地面。
#include "render/RendererInternal.h"
#include "render/ImageLoader.h"

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

namespace {

// Flat ground quad at y=0, facing +Y. Positions are built per-size in
// addGround(); here we keep the shared normals and indices.
const float3 kGroundNrm[4] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
const uint16_t kGroundIdx[6] = {0, 2, 1, 0, 3, 2};

}  // namespace

namespace wizengine {

void Renderer::addGround(float halfSize, const filament::math::float3& color,
                         float tileMeters, const std::string& texturePath,
                         const ShapeMaterial& material) {
    // 2 回目以降の呼び出しは作り直し（シーン文書の <ground> が実行時に
    // 変わるため）。Filament の destroy は使用中の GPU 資源を安全に遅延破棄
    // するので、前フレームが参照していても構わない。
    if (!groundEntity_.isNull()) {
        scene_->remove(groundEntity_);
        engine_->destroy(groundEntity_);
        EntityManager::get().destroy(groundEntity_);
        groundEntity_ = utils::Entity();
    }
    if (groundVb_) { engine_->destroy(groundVb_); groundVb_ = nullptr; }
    if (groundIb_) { engine_->destroy(groundIb_); groundIb_ = nullptr; }
    if (groundTexture_) {
        engine_->destroy(groundTexture_);
        groundTexture_ = nullptr;
    }

    groundMatInstance_->setParameter("baseColor", RgbType::LINEAR, color);
    // 地面の材質（つや消しのままか、濡れた路面のように映り込ませるか）。
    // 地面は自己発光もクリアコートも持たないので、必要な 3 つだけ。
    groundMatInstance_->setParameter("roughness", material.roughness);
    groundMatInstance_->setParameter("metallic", material.metallic);
    groundMatInstance_->setParameter("reflectance", material.reflectance);

    // ---- Ground texture --------------------------------------------------
    // An image file (PNG/JPEG/TGA/BMP) when the scene names one. 読めない・
    // 無いときは市松模様に落として警告する（テクスチャはシーン文書の内容 =
    // 手で書けるので、glTF メッシュと同じく止めずに降格する）。Image
    // colours are sRGB; the checker is a linear multiplier on baseColor.
    std::vector<uint8_t> pixels;
    int texW = 0;
    int texH = 0;
    const bool fromFile =
        !texturePath.empty() &&
        loadImageRGBA(assetPath(texturePath), pixels, texW, texH);
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

}  // namespace wizengine
