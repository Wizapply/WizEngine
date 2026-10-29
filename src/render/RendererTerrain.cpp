// Renderer のテレイン（チャンク分割の高さメッシュ）。
//
// 高さの作り方は知らない: 呼び出し側がチャンクごとの頂点（TerrainChunkMesh）を
// 作って setTerrainChunk で渡す。ここが持つのは Filament の実体だけ。
//
// チャンクの頂点数とインデックス数は addTerrain の時点で固定し、実行中に
// プリミティブ数を変えない（版によって名前が違う API を避ける - ギズモの
// 太線バッチ・細線セットと同じ方針）。穴は面積 0 の三角形で表す。
//
// 表面（層のテクスチャ配列・スプラット・材質）は RendererTerrainSurface.cpp。
#include "render/RendererInternal.h"

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

namespace {

// 接線の W（従法線の向き）。terrain.mat は層の画像を tuv.y = 1 - uv.y で引くので
// 画像の上 = ローカルの +Z。法線マップ（緑が画像の上を向く向き）の緑 = +Z が
// 従法線になり、接線 +X・従法線 +Z・法線 +Y は左手系 = W は負。
// 目視で凹凸が逆（出っ張りが凹みに見える）なら +1 にする。
constexpr float kTerrainTangentSign = -1.0f;

}  // namespace

namespace wizengine {

bool Renderer::addTerrain(int chunksX, int chunksZ, int quadsPerChunk) {
    if (chunksX < 1 || chunksZ < 1 || chunksX > kMaxTerrainChunks ||
        chunksZ > kMaxTerrainChunks || quadsPerChunk < 1 ||
        quadsPerChunk > kMaxTerrainQuads) {
        LOGW("render", "addTerrain: invalid size %dx%d chunks, %d quads per chunk",
             chunksX, chunksZ, quadsPerChunk);
        return false;
    }
    // 2 回目以降は作り直し（解像度・チャンク数の変更）。
    removeTerrain();

    terrainChunksX_ = chunksX;
    terrainChunksZ_ = chunksZ;
    terrainQuads_ = quadsPerChunk;
    const uint32_t side = uint32_t(quadsPerChunk) + 1u;
    const uint32_t vertexCount = side * side;
    const uint32_t indexCount = uint32_t(quadsPerChunk) * uint32_t(quadsPerChunk) * 6u;

    terrainChunks_.resize(std::size_t(chunksX) * std::size_t(chunksZ));
    for (TerrainChunk& chunk : terrainChunks_) {
        chunk.vb = VertexBuffer::Builder()
                       .vertexCount(vertexCount)
                       .bufferCount(3)
                       .attribute(VertexAttribute::POSITION, 0,
                                  VertexBuffer::AttributeType::FLOAT3)
                       .attribute(VertexAttribute::TANGENTS, 1,
                                  VertexBuffer::AttributeType::FLOAT4)
                       .attribute(VertexAttribute::UV0, 2,
                                  VertexBuffer::AttributeType::FLOAT2)
                       .build(*engine_);
        // 最初は原点に潰しておく（未初期化のバッファを描かせない）。中身は
        // setTerrainChunk が入れる。バッファは Filament が非同期にコピーする
        // のでヒープに置き、完了コールバックで解放する。
        auto* pos = new float3[vertexCount];
        auto* tan = new quatf[vertexCount];
        auto* uv = new float2[vertexCount];
        for (uint32_t i = 0; i < vertexCount; ++i) {
            pos[i] = float3{0.0f};
            tan[i] = quatf{1.0f, 0.0f, 0.0f, 0.0f};  // (w, x, y, z) = 単位
            uv[i] = float2{0.0f};
        }
        chunk.vb->setBufferAt(
            *engine_, 0,
            VertexBuffer::BufferDescriptor(
                pos, sizeof(float3) * vertexCount,
                [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));
        chunk.vb->setBufferAt(
            *engine_, 1,
            VertexBuffer::BufferDescriptor(
                tan, sizeof(quatf) * vertexCount,
                [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));
        chunk.vb->setBufferAt(
            *engine_, 2,
            VertexBuffer::BufferDescriptor(
                uv, sizeof(float2) * vertexCount,
                [](void* p, size_t, void*) { delete[] static_cast<float2*>(p); }));

        // インデックスは全部 0 = 何も描かない。頂点数は 65^2 なら 16 ビットに
        // 収まるが、quadsPerChunk を引数で受けるので 32 ビットにしておく。
        auto* idx = new uint32_t[indexCount];
        std::fill(idx, idx + indexCount, 0u);
        chunk.ib = IndexBuffer::Builder()
                       .indexCount(indexCount)
                       .bufferType(IndexBuffer::IndexType::UINT)
                       .build(*engine_);
        chunk.ib->setBuffer(
            *engine_,
            IndexBuffer::BufferDescriptor(
                idx, sizeof(uint32_t) * indexCount,
                [](void* p, size_t, void*) { delete[] static_cast<uint32_t*>(p); }));

        chunk.entity = EntityManager::get().create();
        // チャンクは動かないので AABB は頂点から正しく作れる = フラスタム
        // カリングを効かせる（広いテレインで画面外のチャンクを描かない）。
        // レイヤは既定（kLayerScene）のまま = 全ビューに映る。
        RenderableManager::Builder(1)
            .boundingBox({{0, 0, 0}, {0.01f, 0.01f, 0.01f}})  // setTerrainChunk が更新
            .material(0, terrainMatInstance_)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, chunk.vb,
                      chunk.ib, 0, indexCount)
            .culling(true)
            .castShadows(true)
            .receiveShadows(true)
            .build(*engine_, chunk.entity);
        setEntityTransform(*engine_, chunk.entity, terrainTransform_);
        scene_->addEntity(chunk.entity);
    }
    LOGI("render", "terrain: %dx%d chunks, %d quads per chunk", chunksX, chunksZ,
         quadsPerChunk);
    return true;
}

bool Renderer::setTerrainChunk(int cx, int cz, const TerrainChunkMesh& mesh) {
    if (terrainChunks_.empty() || cx < 0 || cz < 0 || cx >= terrainChunksX_ ||
        cz >= terrainChunksZ_) {
        LOGW("render", "setTerrainChunk: chunk (%d, %d) does not exist", cx, cz);
        return false;
    }
    const std::size_t side = std::size_t(terrainQuads_) + 1;
    const std::size_t n = side * side;
    const std::size_t indexCount =
        std::size_t(terrainQuads_) * std::size_t(terrainQuads_) * 6;
    if (mesh.positions.size() != n * 3 || mesh.normals.size() != n * 3 ||
        mesh.uvs.size() != n * 2 ||
        (!mesh.indices.empty() && mesh.indices.size() != indexCount)) {
        LOGW("render",
             "setTerrainChunk: chunk (%d, %d) has wrong array sizes "
             "(positions %zu, normals %zu, uvs %zu, indices %zu; expected "
             "%zu vertices, %zu indices)",
             cx, cz, mesh.positions.size(), mesh.normals.size(), mesh.uvs.size(),
             mesh.indices.size(), n, indexCount);
        return false;
    }
    // 有限でない値（NaN / Inf）が混じったチャンクは丸ごと送らない。AABB が
    // 壊れると影の範囲とカリングが全体で狂うため、前の中身を残す方が安全。
    auto allFinite = [](const std::vector<float>& v) {
        for (float f : v) {
            if (!std::isfinite(f)) return false;
        }
        return true;
    };
    if (!allFinite(mesh.positions) || !allFinite(mesh.normals) ||
        !allFinite(mesh.uvs)) {
        LOGW("render",
             "setTerrainChunk: chunk (%d, %d) contains non-finite values, not uploaded",
             cx, cz);
        return false;
    }
    TerrainChunk& chunk =
        terrainChunks_[std::size_t(cz) * std::size_t(terrainChunksX_) + std::size_t(cx)];

    // 位置（とバウンディングボックス）。
    auto* pos = new float3[n];
    float3 lo{1e30f}, hi{-1e30f};
    for (std::size_t i = 0; i < n; ++i) {
        pos[i] = float3{mesh.positions[i * 3], mesh.positions[i * 3 + 1],
                        mesh.positions[i * 3 + 2]};
        lo = min(lo, pos[i]);
        hi = max(hi, pos[i]);
    }
    chunk.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            pos, sizeof(float3) * n,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));

    // 法線 + 接線 → 接空間の四元数（lit マテリアルは TANGENTS で法線を受ける。
    // 層の法線マップはこの接空間で読む）。接線は法線に直交するローカル +X 寄りの
    // 向き = (n.y, -n.x, 0) を正規化したもの（平らなら +X）。n.x = n.y = 0（法線が
    // ±Z を向く面）だけは決まらないので +X に倒す（+X は ±Z の法線と直交する）。
    std::vector<float3> nrm(n);
    std::vector<float4> tng(n);
    for (std::size_t i = 0; i < n; ++i) {
        nrm[i] = float3{mesh.normals[i * 3], mesh.normals[i * 3 + 1],
                        mesh.normals[i * 3 + 2]};
        const float3 t{nrm[i].y, -nrm[i].x, 0.0f};
        const float len = length(t);
        tng[i] = float4{len < 1e-6f ? float3{1.0f, 0.0f, 0.0f} : t / len,
                        kTerrainTangentSign};
    }
    auto* tan = new quatf[n];
    auto* orient = filament::geometry::SurfaceOrientation::Builder()
                       .vertexCount(n)
                       .normals(nrm.data())
                       .tangents(tng.data())
                       .build();
    orient->getQuats(tan, n);
    delete orient;
    chunk.vb->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(
            tan, sizeof(quatf) * n,
            [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));

    // UV（メートル単位。タイルは材質の uvScale が決める）。
    auto* uv = new float2[n];
    for (std::size_t i = 0; i < n; ++i) {
        uv[i] = float2{mesh.uvs[i * 2], mesh.uvs[i * 2 + 1]};
    }
    chunk.vb->setBufferAt(
        *engine_, 2,
        VertexBuffer::BufferDescriptor(
            uv, sizeof(float2) * n,
            [](void* p, size_t, void*) { delete[] static_cast<float2*>(p); }));

    // インデックス（穴の変更のときだけ送られてくる想定）。範囲外は 0 に
    // 丸めて、壊れた入力で GPU に範囲外を読ませない。
    if (!mesh.indices.empty()) {
        auto* idx = new uint32_t[indexCount];
        for (std::size_t i = 0; i < indexCount; ++i) {
            const uint32_t v = mesh.indices[i];
            idx[i] = v < n ? v : 0u;
        }
        chunk.ib->setBuffer(
            *engine_,
            IndexBuffer::BufferDescriptor(
                idx, sizeof(uint32_t) * indexCount,
                [](void* p, size_t, void*) { delete[] static_cast<uint32_t*>(p); }));
    }

    // バウンディングボックスは頂点から（影の範囲とカリングがこれを見る）。
    // 平らなチャンクで厚み 0 にならないよう、各軸に最小の半幅を持たせる。
    auto& rm = engine_->getRenderableManager();
    const float3 center = (lo + hi) * 0.5f;
    const float3 half = max((hi - lo) * 0.5f, float3{0.01f});
    rm.setAxisAlignedBoundingBox(rm.getInstance(chunk.entity),
                                 filament::Box{center, half});
    return true;
}

void Renderer::setTerrainTransform(const filament::math::mat4f& transform) {
    terrainTransform_ = transform;
    for (const TerrainChunk& chunk : terrainChunks_) {
        if (!chunk.entity.isNull()) setEntityTransform(*engine_, chunk.entity, transform);
    }
}

void Renderer::removeTerrain() {
    // Filament の destroy は GPU が使い終わるまで遅延するので、前フレームが
    // 参照していても構わない（地面の作り直しと同じ）。
    for (TerrainChunk& chunk : terrainChunks_) {
        if (!chunk.entity.isNull()) {
            scene_->remove(chunk.entity);
            engine_->destroy(chunk.entity);
            EntityManager::get().destroy(chunk.entity);
        }
        if (chunk.vb) engine_->destroy(chunk.vb);
        if (chunk.ib) engine_->destroy(chunk.ib);
    }
    terrainChunks_.clear();
    terrainChunksX_ = 0;
    terrainChunksZ_ = 0;
    terrainQuads_ = 0;
}

}  // namespace wizengine
