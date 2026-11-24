// Renderer の水面（<water> の格子。fluid/WaterForces が波で動かす頂点）。
// 材質は water.mat（画面空間の屈折 + 水深で決まる透け方 + さざ波の法線 + 白波）。
// 三角形の並びは作ったときに決まり、毎フレーム変わるのは頂点（位置・法線・白波・
// 水深）だけ。接空間は「法線 + X 軸を面へ落とした接線」で作る（さざ波の法線を材質が
// 接空間へ直すので、頂点どうしで接線の向きが揃っている必要がある）。
#include "render/RendererInternal.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace filament;
using namespace filament::math;
using namespace render_detail;

namespace wizengine {

namespace {

// 法線 n と、X 軸を面へ落とした接線から接空間の四元数を作る。
quatf tangentFrame(float3 n) {
    const float len = length(n);
    n = len > 1e-6f ? n / len : float3{0.0f, 1.0f, 0.0f};
    float3 t = float3{1.0f, 0.0f, 0.0f} - n * n.x;
    if (length(t) < 1e-4f) t = float3{0.0f, 0.0f, 1.0f} - n * n.z;
    t = normalize(t);
    const float3 b = cross(n, t);
    return mat3f::packTangentFrame(mat3f{t, b, n});
}

}  // namespace

std::size_t Renderer::addWaterMesh(std::size_t vertexCount, const std::vector<uint32_t>& indices) {
    if (!waterMaterial_) {
        waterMaterial_ = loadMaterialPackage(*engine_, "water.filamat", "water material");
    }
    WaterMesh mesh;
    mesh.used = true;
    mesh.vertexCount = std::max<std::size_t>(1, vertexCount);
    mesh.mi = waterMaterial_->createInstance();
    mesh.vb = VertexBuffer::Builder()
                  .vertexCount(uint32_t(mesh.vertexCount))
                  .bufferCount(3)
                  .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3)
                  .attribute(VertexAttribute::TANGENTS, 1, VertexBuffer::AttributeType::FLOAT4)
                  .attribute(VertexAttribute::COLOR, 2, VertexBuffer::AttributeType::FLOAT4)
                  .build(*engine_);
    {
        // 未初期化のバッファを描かせない（原点・上向き・白波なし・深い）。
        std::vector<float3> pos(mesh.vertexCount, float3{0.0f});
        std::vector<quatf> tan(mesh.vertexCount, tangentFrame({0.0f, 1.0f, 0.0f}));
        std::vector<float4> attr(mesh.vertexCount, float4{0.0f, 1000.0f, 0.0f, 1.0f});
        mesh.vb->setBufferAt(*engine_, 0, copyToDescriptor(pos.data(), pos.size()));
        mesh.vb->setBufferAt(*engine_, 1, copyToDescriptor(tan.data(), tan.size()));
        mesh.vb->setBufferAt(*engine_, 2, copyToDescriptor(attr.data(), attr.size()));
    }
    std::vector<uint32_t> idx(indices.begin(), indices.end());
    for (uint32_t& i : idx) i = std::min<uint32_t>(i, uint32_t(mesh.vertexCount - 1));
    if (idx.empty()) idx = {0, 0, 0};
    mesh.ib = IndexBuffer::Builder()
                  .indexCount(uint32_t(idx.size()))
                  .bufferType(IndexBuffer::IndexType::UINT)
                  .build(*engine_);
    mesh.ib->setBuffer(*engine_, copyToDescriptor(idx.data(), idx.size()));
    mesh.entity = utils::EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {1, 1, 1}})  // setWaterMeshVertices が毎回更新
        .material(0, mesh.mi)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mesh.vb, mesh.ib, 0, idx.size())
        .culling(false)
        .castShadows(false)   // 透ける水の影は不自然
        .receiveShadows(true)
        .build(*engine_, mesh.entity);
    scene_->addEntity(mesh.entity);
    mesh.inScene = true;
    waterMeshes_.push_back(mesh);
    const std::size_t id = waterMeshes_.size() - 1;
    // .mat にパラメータの既定値は書けないので、必ず入れる（入れ忘れ = 真っ黒）。
    setWaterStyle(id, WaterStyle{});
    setWaterTime(id, 0.0);
    return id;
}

void Renderer::setWaterMeshVertices(std::size_t id, const float* positions, const float* normals,
                                    const float* foam, const float* depth,
                                    std::size_t vertexCount) {
    if (id >= waterMeshes_.size() || !waterMeshes_[id].used) return;
    WaterMesh& mesh = waterMeshes_[id];
    if (!positions || !normals) return;
    const std::size_t n = std::min(vertexCount, mesh.vertexCount);
    if (n == 0) return;
    std::vector<float3> pos(n);
    std::vector<quatf> tan(n);
    std::vector<float4> attr(n);
    float3 lo{1e30f}, hi{-1e30f};
    for (std::size_t i = 0; i < n; ++i) {
        pos[i] = float3{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
        tan[i] = tangentFrame({normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]});
        attr[i] = float4{foam ? foam[i] : 0.0f, depth ? depth[i] : 1000.0f, 0.0f, 1.0f};
        lo = min(lo, pos[i]);
        hi = max(hi, pos[i]);
    }
    mesh.vb->setBufferAt(*engine_, 0, copyToDescriptor(pos.data(), n));
    mesh.vb->setBufferAt(*engine_, 1, copyToDescriptor(tan.data(), n));
    mesh.vb->setBufferAt(*engine_, 2, copyToDescriptor(attr.data(), n));
    auto& rm = engine_->getRenderableManager();
    const float3 center = (lo + hi) * 0.5f;
    const float3 half = max((hi - lo) * 0.5f, float3{0.01f});
    rm.setAxisAlignedBoundingBox(rm.getInstance(mesh.entity), Box{center, half});
}

void Renderer::setWaterStyle(std::size_t id, const WaterStyle& style) {
    if (id >= waterMeshes_.size() || !waterMeshes_[id].used) return;
    MaterialInstance* mi = waterMeshes_[id].mi;
    mi->setParameter("deepColor", RgbType::LINEAR, style.color);
    mi->setParameter("absorption", style.absorption);
    mi->setParameter("roughness", std::clamp(style.roughness, 0.0f, 1.0f));
    mi->setParameter("ior", std::max(1.0f, style.ior));
    mi->setParameter("clarity", std::max(0.01f, style.clarity));
    mi->setParameter("ripple", std::max(0.0f, style.ripple));
    mi->setParameter("wind", float2{std::cos(style.heading), std::sin(style.heading)});
    mi->setParameter("gravity", std::max(0.1f, style.gravity));
    mi->setParameter("level", style.level);
    mi->setParameter("crest", std::max(0.0f, style.crest));
}

void Renderer::setWaterTime(std::size_t id, double time) {
    if (id >= waterMeshes_.size() || !waterMeshes_[id].used) return;
    // 材質の中は float なので、長く回すと位相の精度が落ちる。1 時間で折り返す
    // （さざ波が 1 時間に 1 回だけ不連続になる）。
    waterMeshes_[id].mi->setParameter("time", float(std::fmod(time, 3600.0)));
}

void Renderer::removeWaterMesh(std::size_t id) {
    if (id >= waterMeshes_.size() || !waterMeshes_[id].used) return;
    WaterMesh& mesh = waterMeshes_[id];
    if (mesh.entity) {
        destroyRenderable(*engine_, *scene_, mesh.entity, mesh.vb, mesh.ib, mesh.inScene);
    }
    if (mesh.mi) engine_->destroy(mesh.mi);
    mesh = WaterMesh{};  // used = false（番号は使い回さない）
}

}  // namespace wizengine
