// Renderer のパーティクル（加算合成の光る粒子。炎・火花など見た目だけ）。
// 粒子 1 個を「直交する 3 枚の円盤」（中心 1 + 縁 kRing 頂点の扇）で描く。
// 中心の頂点だけが色を持ち、縁は黒 = 加算合成で中心から縁へ柔らかく消える。
#include "render/RendererInternal.h"

#include <algorithm>
#include <cmath>

#include <Eigen/Core>
#include <Eigen/Geometry>

using namespace filament;
using namespace filament::math;
using namespace render_detail;

namespace wizengine {

namespace {

constexpr std::size_t kRing = 6;                              // 円盤の縁の頂点数
constexpr std::size_t kDiscVerts = kRing + 1;                 // 中心 + 縁
constexpr std::size_t kVertsPerParticle = 3 * kDiscVerts;     // 円盤 3 枚
constexpr std::size_t kIndicesPerParticle = 3 * kRing * 3;    // 三角形 3 x kRing 枚
constexpr std::size_t kMaxParticles = 16384;

}  // namespace

void Renderer::loadParticleMaterial() {
    particleMaterial_ =
        loadMaterialPackage(*engine_, "particle.filamat", "particle material");
}

std::size_t Renderer::addParticleMesh(std::size_t maxParticles) {
    ParticleMesh mesh;
    mesh.capacity = std::max<std::size_t>(1, std::min(maxParticles, kMaxParticles));
    const std::size_t vcount = mesh.capacity * kVertsPerParticle;
    const std::size_t icount = mesh.capacity * kIndicesPerParticle;
    mesh.mi = particleMaterial_->createInstance();
    mesh.vb = VertexBuffer::Builder()
                  .vertexCount(uint32_t(vcount))
                  .bufferCount(2)
                  .attribute(VertexAttribute::POSITION, 0,
                             VertexBuffer::AttributeType::FLOAT3, 0, sizeof(float3))
                  .attribute(VertexAttribute::COLOR, 1,
                             VertexBuffer::AttributeType::FLOAT4, 0, sizeof(float4))
                  .build(*engine_);
    // 最初は全部の席を原点・黒にしておく（未初期化のバッファを描かせない）。
    {
        std::vector<float3> pos(vcount, float3{0.0f});
        std::vector<float4> col(vcount, float4{0.0f});
        mesh.vb->setBufferAt(*engine_, 0, copyToDescriptor(pos.data(), vcount));
        mesh.vb->setBufferAt(*engine_, 1, copyToDescriptor(col.data(), vcount));
    }
    // インデックスは固定（粒子 i の円盤 d の扇）。
    {
        std::vector<uint32_t> idx(icount);
        std::size_t k = 0;
        for (std::size_t p = 0; p < mesh.capacity; ++p) {
            for (std::size_t d = 0; d < 3; ++d) {
                const uint32_t base = uint32_t(p * kVertsPerParticle + d * kDiscVerts);
                for (std::size_t r = 0; r < kRing; ++r) {
                    idx[k++] = base;
                    idx[k++] = base + 1 + uint32_t(r);
                    idx[k++] = base + 1 + uint32_t((r + 1) % kRing);
                }
            }
        }
        mesh.ib = IndexBuffer::Builder()
                      .indexCount(uint32_t(icount))
                      .bufferType(IndexBuffer::IndexType::UINT)
                      .build(*engine_);
        mesh.ib->setBuffer(*engine_, copyToDescriptor(idx.data(), icount));
    }
    mesh.entity = utils::EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{-1000.0f, -1000.0f, -1000.0f}, {1000.0f, 1000.0f, 1000.0f}})
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, mesh.vb, mesh.ib, 0,
                  icount)
        .material(0, mesh.mi)
        .culling(false)
        .castShadows(false)
        .receiveShadows(false)
        .build(*engine_, mesh.entity);
    scene_->addEntity(mesh.entity);
    particleMeshes_.push_back(mesh);
    return particleMeshes_.size() - 1;
}

void Renderer::setParticles(std::size_t id, const std::vector<Particle>& particles) {
    if (id >= particleMeshes_.size()) return;
    ParticleMesh& mesh = particleMeshes_[id];
    const std::size_t n = std::min(particles.size(), mesh.capacity);
    // 前回より減ったぶんの席は原点・黒に戻す（面積 0 = 何も描かない）。
    const std::size_t upload = std::max(n, mesh.lastCount);
    mesh.lastCount = n;
    if (upload == 0) return;
    const std::size_t vcount = upload * kVertsPerParticle;
    std::vector<float3> pos(vcount, float3{0.0f});
    std::vector<float4> col(vcount, float4{0.0f});
    // 円盤 3 枚の向き（法線 Z / X / Y）。縁の頂点 = 中心 + 半径 × (cos u + sin v)。
    static const float3 kU[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static const float3 kV[3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}};
    // 縁の方向は Eigen の 2 次元回転で作る（円周率は EIGEN_PI）。
    float ringCos[kRing], ringSin[kRing];
    for (std::size_t r = 0; r < kRing; ++r) {
        const Eigen::Vector2f d =
            Eigen::Rotation2Df(2.0f * float(EIGEN_PI) * float(r) / float(kRing)) *
            Eigen::Vector2f::UnitX();
        ringCos[r] = d.x();
        ringSin[r] = d.y();
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Particle& p = particles[i];
        const float rad = 0.5f * std::max(p.diameter, 0.0f);
        const float4 c{p.color.x, p.color.y, p.color.z, 1.0f};
        for (std::size_t d = 0; d < 3; ++d) {
            const std::size_t base = i * kVertsPerParticle + d * kDiscVerts;
            pos[base] = p.position;
            col[base] = c;
            for (std::size_t r = 0; r < kRing; ++r) {
                pos[base + 1 + r] = p.position + rad * (ringCos[r] * kU[d] + ringSin[r] * kV[d]);
                // 縁は黒（col の初期値のまま）= 加算で何も足さない
            }
        }
    }
    mesh.vb->setBufferAt(*engine_, 0, copyToDescriptor(pos.data(), vcount));
    mesh.vb->setBufferAt(*engine_, 1, copyToDescriptor(col.data(), vcount));
}

}  // namespace wizengine
