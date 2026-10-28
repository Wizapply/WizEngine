// Renderer のテレイン（チャンク分割の高さメッシュ）。
//
// 高さの作り方は知らない: 呼び出し側がチャンクごとの頂点（TerrainChunkMesh）を
// 作って setTerrainChunk で渡す。ここが持つのは Filament の実体だけ。
//
// チャンクの頂点数とインデックス数は addTerrain の時点で固定し、実行中に
// プリミティブ数を変えない（版によって名前が違う API を避ける - ギズモの
// 太線バッチ・細線セットと同じ方針）。穴は面積 0 の三角形で表す。
//
// 表面は層のテクスチャ配列（4 層）と塗り分けの重み（スプラット）を terrain.mat
// が混ぜる。重みの中身（塗り方・正規化）は知らない: 呼び出し側が矩形の
// テクセルを setTerrainSplat で渡す。
#include "render/RendererInternal.h"
#include "terrain/HeightField.h"  // kDefaultSplatTexel（既定の重みの唯一の定義）

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

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

    // 法線 → 接空間の四元数（lit マテリアルは TANGENTS で法線を受ける）。
    std::vector<float3> nrm(n);
    for (std::size_t i = 0; i < n; ++i) {
        nrm[i] = float3{mesh.normals[i * 3], mesh.normals[i * 3 + 1],
                        mesh.normals[i * 3 + 2]};
    }
    auto* tan = new quatf[n];
    auto* orient = filament::geometry::SurfaceOrientation::Builder()
                       .vertexCount(n)
                       .normals(nrm.data())
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

void Renderer::createTerrainTextures() {
    // 層の配列: 全層を白で埋める（= テクスチャ無しの層。色がそのまま出る）。
    // 配列は全層が同じ大きさ・同じ形式なので、ファイルの画像（sRGB）に合わせて
    // SRGB8_A8 にする（白 255 は sRGB でもリニアでも 1.0）。
    const int size = kTerrainLayerTexSize;
    terrainLayerTextures_ =
        filament::Texture::Builder()
            .width(uint32_t(size))
            .height(uint32_t(size))
            .depth(uint32_t(TerrainSurface::kLayers))
            .levels(mipLevelCount(size, size))
            .format(filament::Texture::InternalFormat::SRGB8_A8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D_ARRAY)
            .build(*engine_);
    const std::vector<uint8_t> white(std::size_t(size) * std::size_t(size) * 4, uint8_t(255));
    for (int i = 0; i < TerrainSurface::kLayers; ++i) {
        uploadMipChain(*engine_, terrainLayerTextures_, uint32_t(i), white, size, size);
        terrainLayerPaths_[i].clear();
    }
    // タイルさせる（REPEAT）。斜めから見る面なので異方性を効かせる。
    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    sampler.setAnisotropy(8.0f);
    terrainMatInstance_->setParameter("albedo", terrainLayerTextures_, sampler);
    // 重みは 1×1 の既定（層 0 だけ）= 塗っていないテレインは層 0 の絵。
    // 換算の幅は仮の 1 m（1×1 は CLAMP でどこを引いても同じテクセル）。
    recreateTerrainSplat(1, true);
    terrainMatInstance_->setParameter("splatInvSize", 1.0f);
}

void Renderer::recreateTerrainSplat(int resolution, bool fill) {
    filament::Texture* next =
        filament::Texture::Builder()
            .width(uint32_t(resolution))
            .height(uint32_t(resolution))
            .levels(1)
            .format(filament::Texture::InternalFormat::RGBA8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .build(*engine_);
    if (fill) {
        // 既定のテクセルは terrain::kDefaultSplatTexel（数字をここに書かない）。
        const std::size_t texels = std::size_t(resolution) * std::size_t(resolution);
        std::vector<uint8_t> texelsRgba(texels * 4);
        for (std::size_t k = 0; k < texels; ++k) {
            std::copy(terrain::kDefaultSplatTexel.begin(), terrain::kDefaultSplatTexel.end(),
                      texelsRgba.begin() + std::ptrdiff_t(k * 4));
        }
        next->setImage(*engine_, 0, copyToPixelDescriptor(texelsRgba.data(), texelsRgba.size()));
    }
    // 重みはセルの間で滑らかに混ぜる（LINEAR）。端は伸ばす（CLAMP = 反対側の
    // 縁の重みを混ぜない）。ミップは持たない（遠くでも境目の位置を保つ）。
    filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR,
                                     filament::TextureSampler::MagFilter::LINEAR,
                                     filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    // 新しい方を付けてから古い方を壊す（地面のテクスチャと同じ順）。
    terrainMatInstance_->setParameter("splat", next, sampler);
    if (terrainSplat_) engine_->destroy(terrainSplat_);
    terrainSplat_ = next;
    terrainSplatRes_ = resolution;
}

void Renderer::setTerrainSurface(const TerrainSurface& surface) {
    if (!terrainMatInstance_ || !terrainLayerTextures_) return;

    // 層のテクスチャはパスが変わった層だけ読み直す（色や粗さのスライダー・
    // タイルを動かすたびに画像をデコードしない）。配列の中身の入れ替えなので、
    // マテリアルへの付け替えは要らない。
    float4 uvScale{1.0f};
    for (int i = 0; i < TerrainSurface::kLayers; ++i) {
        const TerrainSurface::Layer& layer = surface.layers[i];
        if (layer.texture != terrainLayerPaths_[i]) {
            char label[32];
            std::snprintf(label, sizeof(label), "terrain layer %d", i);
            std::vector<uint8_t> pixels;
            int w = 0;
            int h = 0;
            const bool fromFile = decodeTextureRGBA(layer.texture, label, TextureFallback::White,
                                                    pixels, w, h);
            // 配列は全層が同じ大きさ: 違う大きさ・縦横比の画像は引き伸ばす。
            const int size = kTerrainLayerTexSize;
            if (fromFile && (w != size || h != size)) {
                LOGI("render", "%s texture resized from %dx%d to %dx%d%s", label, w, h, size,
                     size, w != h ? " (not square - stretched)" : "");
            }
            std::vector<uint8_t> resized;
            resizeRGBA(pixels.data(), w, h, size, size, resized);
            uploadMipChain(*engine_, terrainLayerTextures_, uint32_t(i), std::move(resized),
                           size, size);
            terrainLayerPaths_[i] = layer.texture;
        }
        // UV はメートル単位なので、1 / タイルのメートル数でタイルする。
        uvScale[i] = (layer.tileMeters > 1e-4f) ? 1.0f / layer.tileMeters : 1.0f;
    }
    terrainMatInstance_->setParameter("uvScale", uvScale);
    terrainMatInstance_->setParameter("baseColor", RgbType::LINEAR, surface.color.xyz);
    terrainMatInstance_->setParameter("roughness", std::clamp(surface.roughness, 0.0f, 1.0f));
    terrainMatInstance_->setParameter("metallic", std::clamp(surface.metallic, 0.0f, 1.0f));
}

bool Renderer::setTerrainSplat(int resolution, float sizeMeters, int x0, int z0, int w,
                               int h, const uint8_t* rgba) {
    if (!terrainMatInstance_) return false;
    if (resolution < 1 || !rgba || w < 1 || h < 1 || x0 < 0 || z0 < 0 ||
        x0 + w > resolution || z0 + h > resolution || !std::isfinite(sizeMeters) ||
        !(sizeMeters > 0.0f)) {
        LOGW("render",
             "setTerrainSplat: invalid rect (%d, %d) %dx%d for resolution %d, size %.3f m",
             x0, z0, w, h, resolution, double(sizeMeters));
        return false;
    }
    // 解像度が違う・未作成なら作り直す。矩形が全体なら直後に全部書くので埋めない。
    if (!terrainSplat_ || resolution != terrainSplatRes_) {
        const bool whole = x0 == 0 && z0 == 0 && w == resolution && h == resolution;
        recreateTerrainSplat(resolution, !whole);
    }
    // UV はローカルのメートル（格子は原点中心）なので、uv / size + 0.5 で 0〜1。
    terrainMatInstance_->setParameter("splatInvSize", 1.0f / sizeMeters);
    // 矩形だけの部分更新（行 = +Z がテクスチャの行）。
    const std::size_t bytes = std::size_t(w) * std::size_t(h) * 4;
    terrainSplat_->setImage(*engine_, 0, uint32_t(x0), uint32_t(z0), uint32_t(w),
                            uint32_t(h), copyToPixelDescriptor(rgba, bytes));
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
