// Renderer の液体（流体粒子から組んだ水の表面。fluid/FluidSurface が作るメッシュ）。
// 材質は liquid.mat（画面空間の屈折 + 吸収 = 透けて見える水）。頂点と三角形の数が
// 毎フレーム変わるので、席を多めに取っておき、余った三角形の席はインデックス 0
// （面積 0 = 何も描かない）で埋める（パーティクル・線バッチと同じ方針。実行中に
// プリミティブ数を変える版依存の API を使わない）。
#include "render/RendererInternal.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace filament;
using namespace filament::math;
using namespace render_detail;

namespace wizengine {

namespace {

constexpr std::size_t kMinVertices = 4096;
constexpr std::size_t kMinIndices = 3 * 8192;

}  // namespace

std::size_t Renderer::addLiquidMesh() {
    if (!liquidMaterial_) {
        liquidMaterial_ = loadMaterialPackage(*engine_, "liquid.filamat", "liquid material");
    }
    LiquidMesh mesh;
    mesh.used = true;
    mesh.mi = liquidMaterial_->createInstance();
    liquidMeshes_.push_back(mesh);
    const std::size_t id = liquidMeshes_.size() - 1;
    // .mat にパラメータの既定値は書けないので、必ず入れる（入れ忘れ = 真っ黒）。
    setLiquidStyle(id, LiquidStyle{});
    return id;
}

void Renderer::growLiquidMesh(LiquidMesh& mesh, std::size_t vertices, std::size_t indices) {
    if (mesh.entity) {
        destroyRenderable(*engine_, *scene_, mesh.entity, mesh.vb, mesh.ib, mesh.inScene);
    }
    mesh.vertexCapacity = std::max(kMinVertices, vertices + vertices / 2);
    mesh.indexCapacity = std::max(kMinIndices, (indices + indices / 2 + 2) / 3 * 3);
    mesh.vb = VertexBuffer::Builder()
                  .vertexCount(uint32_t(mesh.vertexCapacity))
                  .bufferCount(2)
                  .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3)
                  .attribute(VertexAttribute::TANGENTS, 1, VertexBuffer::AttributeType::FLOAT4)
                  .build(*engine_);
    {
        // 未初期化のバッファを描かせない（原点・上向き）。
        std::vector<float3> pos(mesh.vertexCapacity, float3{0.0f});
        std::vector<quatf> tan(mesh.vertexCapacity, quatf{1.0f, 0.0f, 0.0f, 0.0f});
        mesh.vb->setBufferAt(*engine_, 0, copyToDescriptor(pos.data(), pos.size()));
        mesh.vb->setBufferAt(*engine_, 1, copyToDescriptor(tan.data(), tan.size()));
    }
    mesh.ib = IndexBuffer::Builder()
                  .indexCount(uint32_t(mesh.indexCapacity))
                  .bufferType(IndexBuffer::IndexType::UINT)
                  .build(*engine_);
    {
        std::vector<uint32_t> zeros(mesh.indexCapacity, 0u);
        mesh.ib->setBuffer(*engine_, copyToDescriptor(zeros.data(), zeros.size()));
    }
    mesh.lastIndexCount = 0;
    mesh.entity = utils::EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {1, 1, 1}})  // setLiquidMesh が毎回更新
        .material(0, mesh.mi)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mesh.vb, mesh.ib, 0,
                  mesh.indexCapacity)
        .culling(false)
        .castShadows(false)   // 透ける物の影は不自然
        .receiveShadows(true)
        .build(*engine_, mesh.entity);
    scene_->addEntity(mesh.entity);
    mesh.inScene = true;
}

void Renderer::setLiquidMesh(std::size_t id, const float* positions, const float* normals,
                             std::size_t vertexCount, const uint32_t* indices,
                             std::size_t indexCount) {
    if (id >= liquidMeshes_.size() || !liquidMeshes_[id].used) return;
    LiquidMesh& mesh = liquidMeshes_[id];
    indexCount -= indexCount % 3;
    if (vertexCount == 0 || !positions || !normals || !indices) indexCount = 0;
    if (indexCount > 0 &&
        (!mesh.entity || vertexCount > mesh.vertexCapacity || indexCount > mesh.indexCapacity)) {
        growLiquidMesh(mesh, vertexCount, indexCount);
    }
    if (!mesh.entity) return;  // まだ一度も中身が無い

    if (indexCount > 0) {
        // 位置と、法線から作る接空間の四元数（lit の材質は TANGENTS で法線を受ける）。
        std::vector<float3> pos(vertexCount), nrm(vertexCount);
        float3 lo{1e30f}, hi{-1e30f};
        for (std::size_t i = 0; i < vertexCount; ++i) {
            pos[i] = float3{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
            nrm[i] = float3{normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]};
            lo = min(lo, pos[i]);
            hi = max(hi, pos[i]);
        }
        std::vector<quatf> tan(vertexCount);
        auto* orient = geometry::SurfaceOrientation::Builder()
                           .vertexCount(vertexCount)
                           .normals(nrm.data())
                           .build();
        orient->getQuats(tan.data(), vertexCount);
        delete orient;
        mesh.vb->setBufferAt(*engine_, 0, copyToDescriptor(pos.data(), vertexCount));
        mesh.vb->setBufferAt(*engine_, 1, copyToDescriptor(tan.data(), vertexCount));
        auto& rm = engine_->getRenderableManager();
        const float3 center = (lo + hi) * 0.5f;
        const float3 half = max((hi - lo) * 0.5f, float3{0.01f});
        rm.setAxisAlignedBoundingBox(rm.getInstance(mesh.entity), Box{center, half});
    }
    // インデックス: 今回のぶん + 前回より減ったぶんを 0（面積 0）で埋める。
    const std::size_t upload = std::max(indexCount, mesh.lastIndexCount);
    if (upload > 0) {
        std::vector<uint32_t> idx(upload, 0u);
        for (std::size_t i = 0; i < indexCount; ++i) {
            idx[i] = std::min<uint32_t>(indices[i], uint32_t(vertexCount - 1));
        }
        mesh.ib->setBuffer(*engine_, copyToDescriptor(idx.data(), upload));
    }
    mesh.lastIndexCount = indexCount;
}

void Renderer::setLiquidStyle(std::size_t id, const LiquidStyle& style) {
    if (id >= liquidMeshes_.size() || !liquidMeshes_[id].used) return;
    MaterialInstance* mi = liquidMeshes_[id].mi;
    mi->setParameter("baseColor", RgbType::LINEAR, style.color);
    mi->setParameter("absorption", style.absorption);
    mi->setParameter("roughness", std::clamp(style.roughness, 0.0f, 1.0f));
    mi->setParameter("ior", std::max(1.0f, style.ior));
    mi->setParameter("transmission", std::clamp(style.transmission, 0.0f, 1.0f));
    mi->setParameter("thickness", std::max(0.0f, style.thickness));
}

void Renderer::removeLiquidMesh(std::size_t id) {
    if (id >= liquidMeshes_.size() || !liquidMeshes_[id].used) return;
    LiquidMesh& mesh = liquidMeshes_[id];
    if (mesh.entity) {
        destroyRenderable(*engine_, *scene_, mesh.entity, mesh.vb, mesh.ib, mesh.inScene);
    }
    if (mesh.mi) engine_->destroy(mesh.mi);
    mesh = LiquidMesh{};  // used = false（番号は使い回さない）
}

}  // namespace wizengine
