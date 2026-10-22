// テレインの描画の同期（RENDER スレッド）: 高さ格子をチャンクのメッシュにして
// Renderer へ送る（全部の作り直しと、筆が触ったチャンクだけの送り直し）と、
// 表面の材質の入れ直し。dirty の取り出し（ロックの枠組み）は applyToRenderer
// （Scene.cpp）が持ち、ここはロックを外してから呼ばれる。
#include "scene/SceneInternal.h"

#include <chrono>

using namespace scene_detail;

// テレインの上限の照合。Renderer は src/terrain を知らず、src/terrain は
// Renderer を知らないので、両方を見るここで食い違いを止める。
static_assert(wizengine::terrain::kMaxChunkQuads == wizengine::Renderer::kMaxTerrainQuads,
              "terrain::kMaxChunkQuads and Renderer::kMaxTerrainQuads must match");
static_assert(kTerrainChunkQuads >= 1 && kTerrainChunkQuads <= wizengine::Renderer::kMaxTerrainQuads,
              "kTerrainChunkQuads is out of the Renderer's range");
static_assert(ed::kTerrainMaxResolution <= wizengine::terrain::kMaxResolution,
              "the document's terrain resolution limit exceeds HeightField's");
// 解像度の上限でも 1 辺のチャンク数が Renderer の上限に収まること
// （1024 / 64 = 16 <= 64）。
static_assert((ed::kTerrainMaxResolution + kTerrainChunkQuads - 1) / kTerrainChunkQuads <=
                  wizengine::Renderer::kMaxTerrainChunks,
              "kTerrainMaxResolution / kTerrainChunkQuads exceeds Renderer::kMaxTerrainChunks");

int Scene::uploadTerrainChunks(const wizengine::terrain::HeightField& field,
                               const wizengine::terrain::GridRect& chunkRect) {
    wizengine::terrain::ChunkMesh built;
    wizengine::TerrainChunkMesh upload;
    int failed = 0;
    for (int cz = chunkRect.j0; cz < chunkRect.j1; ++cz) {
        for (int cx = chunkRect.i0; cx < chunkRect.i1; ++cx) {
            const bool ok = field.buildChunkMesh(cx, cz, kTerrainChunkQuads, built);
            if (!ok) {
                ++failed;
                continue;
            }
            // 同じ並びの素の配列なので move で渡す（コピーしない）。インデックス
            // も毎回渡す: addTerrain の直後は全部 0 = 何も描かれないため。穴の
            // 変更（面積 0 の三角形）もインデックスで効く。
            upload.positions = std::move(built.positions);
            upload.normals = std::move(built.normals);
            upload.uvs = std::move(built.uvs);
            upload.indices = std::move(built.indices);
            if (!renderer_.setTerrainChunk(cx, cz, upload)) ++failed;
        }
    }
    return failed;
}

void Scene::syncTerrain(bool rebuildMesh, const wizengine::terrain::GridRect& chunks,
                        bool updateSurface, bool enabled, const ed::TerrainDesc& desc,
                        const std::shared_ptr<const wizengine::terrain::HeightField>& field) {
    const bool meshChanged = rebuildMesh || !chunks.empty();
    if (!enabled || (meshChanged && (!field || !field->valid()))) {
        renderer_.removeTerrain();  // 無ければ何もしない
        terrainRenderBuilt_ = false;
        return;
    }
    // まだ組んでいない（初回・addTerrain が断った後・外した後）のに一部だけ
    // 来たら、全部の作り直しに倒す（組んでいないチャンクへは送れない）。
    const bool full = rebuildMesh || (!chunks.empty() && !terrainRenderBuilt_);
    const auto t0 = std::chrono::steady_clock::now();
    wizengine::terrain::GridRect upload = chunks;
    if (full) {
        const int n = field->chunksPerSide(kTerrainChunkQuads);
        terrainRenderBuilt_ = false;
        if (!renderer_.addTerrain(n, n, kTerrainChunkQuads)) {
            LOGW("scene", "terrain: the renderer refused %dx%d chunks - not drawn", n, n);
            return;
        }
        terrainRenderBuilt_ = true;
        upload = wizengine::terrain::GridRect{0, 0, n, n};
    }
    if (!upload.empty()) {
        const int failed = uploadTerrainChunks(*field, upload);
        if (failed > 0) {
            LOGW("scene", "terrain: %d of %d chunks could not be built", failed,
                 (upload.i1 - upload.i0) * (upload.j1 - upload.j0));
        }
        // 一部だけの送り直し（筆のパスごと）はログに出さない（毎フレーム来る）。
        if (full) {
            LOGI("scene",
                 "terrain mesh: %dx%d chunks (%d quads each) built and uploaded in %.1f ms",
                 upload.i1, upload.j1, kTerrainChunkQuads, elapsedMs(t0));
        }
    }
    // メッシュを作り直したときは表面も必ず入れ直す。addTerrain が失敗した
    // フレームは上で return しており、そのとき消えた表面の dirty は次の
    // 作り直しがここで埋め合わせる（材質が既定の白のまま残らない）。
    // テクスチャはパスが同じなら Renderer が読み直さないので、入れ直しは安い。
    if (updateSurface || full) {
        renderer_.setTerrainSurface(
            desc.texture, float(desc.tile),
            filament::math::float4{desc.color.r, desc.color.g, desc.color.b, 1.0f},
            float(desc.roughness), float(desc.metallic));
    }
}
