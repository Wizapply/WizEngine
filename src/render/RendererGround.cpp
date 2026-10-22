// Renderer の地面。
#include "render/RendererInternal.h"

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

void Renderer::removeGround() {
    // Filament の destroy は使用中の GPU 資源を安全に遅延破棄するので、
    // 前フレームが参照していても構わない。
    // **テクスチャはここで壊さない**: 地面のマテリアルインスタンス
    // （groundMatInstance_）に "checker" として付いたままなので、壊すと次の
    // 描画で Filament が "Invalid texture still bound to MaterialInstance" で
    // 止まる（地面を外したシーン = <ground enabled="false"> で起きた）。
    // 差し替えは addGround が新しいテクスチャを付けてから古い方を壊す。
    if (!groundEntity_.isNull()) {
        scene_->remove(groundEntity_);
        engine_->destroy(groundEntity_);
        EntityManager::get().destroy(groundEntity_);
        groundEntity_ = utils::Entity();
    }
    if (groundVb_) { engine_->destroy(groundVb_); groundVb_ = nullptr; }
    if (groundIb_) { engine_->destroy(groundIb_); groundIb_ = nullptr; }
}

void Renderer::addGround(float halfSize, const filament::math::float3& color,
                         float tileMeters, const std::string& texturePath,
                         const ShapeMaterial& material) {
    // 2 回目以降の呼び出しは作り直し（シーン文書の <ground> が実行時に
    // 変わるため）。
    removeGround();

    groundMatInstance_->setParameter("baseColor", RgbType::LINEAR, color);
    // 地面の材質（つや消しのままか、濡れた路面のように映り込ませるか）。
    // 地面は自己発光もクリアコートも持たないので、必要な 3 つだけ。
    groundMatInstance_->setParameter("roughness", material.roughness);
    groundMatInstance_->setParameter("metallic", material.metallic);
    groundMatInstance_->setParameter("reflectance", material.reflectance);

    // ---- Ground texture --------------------------------------------------
    // 画像（シーン文書の <ground texture>）。読めない・無いときは市松模様に
    // 落として警告する（RendererTextures.cpp の loadTextureRGBA）。
    // 古いテクスチャは新しい方をマテリアルに付けてから壊す（付いたまま壊すと
    // Filament が止まる。removeGround の注意と同じ）。
    filament::Texture* oldTexture = groundTexture_;
    groundTexture_ = loadTextureRGBA(*engine_, texturePath, "ground",
                                     TextureFallback::Checker);

    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    sampler.setAnisotropy(8.0f);
    groundMatInstance_->setParameter("checker", groundTexture_, sampler);
    if (oldTexture && oldTexture != groundTexture_) engine_->destroy(oldTexture);

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
    setEntityTransform(*engine_, groundEntity_, groundTransform_);
    scene_->addEntity(groundEntity_);
}

void Renderer::setGroundTransform(const filament::math::mat4f& transform) {
    groundTransform_ = transform;
    if (!groundEntity_.isNull()) setEntityTransform(*engine_, groundEntity_, transform);
}

}  // namespace wizengine
