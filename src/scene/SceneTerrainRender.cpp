// テレインの描画の同期（RENDER スレッド）: 高さ格子をチャンクのメッシュにして
// Renderer へ送る（全部の作り直しと、筆が触ったチャンクだけの送り直し）、塗り
// 分けの重み（スプラット）の全体 / 矩形の送り直し、表面の材質と層の入れ直し。dirty の取り出し（ロックの枠組み）は applyToRenderer
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
// 塗り分けの層の数（重みの RGBA・TerrainLayers・層のテクスチャ配列の枚数）。
static_assert(wizengine::terrain::kSplatChannels == wizengine::TerrainSurface::kLayers,
              "terrain::kSplatChannels and TerrainSurface::kLayers must match");
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

bool Scene::uploadTerrainSplat(const wizengine::terrain::HeightField& field,
                               const wizengine::terrain::GridRect& cells) {
    const int res = field.resolution();
    // 塗っていない（重みの配列が無い）テレインは全セルが既定なので、どの矩形でも
    // 1×1 の既定で足りる = 従来の絵のまま、解像度ぶんのテクスチャを持たない
    // （取り消しの矩形もここに来る）。後で一部を塗れば、Renderer が解像度に
    // 合わせて既定で作り直してからその矩形を書く。
    if (!field.hasSplat()) {
        const auto& texel = wizengine::terrain::kDefaultSplatTexel;
        return renderer_.setTerrainSplat(1, field.size(), 0, 0, 1, 1, texel.data());
    }
    const int w = cells.i1 - cells.i0;
    const int h = cells.j1 - cells.j0;
    std::vector<uint8_t> rgba(std::size_t(w) * std::size_t(h) *
                              std::size_t(wizengine::terrain::kSplatChannels));
    if (!field.copySplat(cells, rgba.data())) return false;
    return renderer_.setTerrainSplat(res, field.size(), cells.i0, cells.j0, w, h, rgba.data());
}

void Scene::syncTerrain(bool rebuildMesh, const wizengine::terrain::GridRect& chunks,
                        const wizengine::terrain::GridRect& splatCells,
                        bool updateSurface, bool enabled, const ed::TerrainDesc& desc,
                        const wizengine::terrain::TerrainLayers& layers,
                        const std::shared_ptr<const wizengine::terrain::HeightField>& field) {
    const bool gridChanged = rebuildMesh || !chunks.empty() || !splatCells.empty();
    if (!enabled || (gridChanged && (!field || !field->valid()))) {
        renderer_.removeTerrain();  // 無ければ何もしない
        terrainRenderBuilt_ = false;
        return;
    }
    // まだ組んでいない（初回・addTerrain が断った後・外した後）のに一部だけ
    // 来たら、全部の作り直しに倒す（組んでいないチャンクへは送れない）。
    const bool full =
        rebuildMesh || ((!chunks.empty() || !splatCells.empty()) && !terrainRenderBuilt_);
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
    // 塗り分けの重み: 全部の作り直しなら全体、そうでなければ塗ったセルの矩形だけ
    // （どちらも格子がある = 上の検査を通っている）。
    if (full || !splatCells.empty()) {
        const int res = field->resolution();
        wizengine::terrain::GridRect splat =
            full ? wizengine::terrain::GridRect{0, 0, res, res} : splatCells;
        // 退路: Vulkan で矩形だけの部分更新（setImage の部分版）の絵が崩れたら、
        // 次の 1 行を有効にして毎回全体を送る（1024 で 4 MB / 回）。
        // splat = wizengine::terrain::GridRect{0, 0, res, res};
        if (!uploadTerrainSplat(*field, splat)) {
            LOGW("scene", "terrain: splat cells (%d, %d)-(%d, %d) could not be uploaded "
                          "(resolution %d)", splat.i0, splat.j0, splat.i1, splat.j1, res);
        }
    }
    // メッシュを作り直したときは表面も必ず入れ直す。addTerrain が失敗した
    // フレームは上で return しており、そのとき消えた表面の dirty は次の
    // 作り直しがここで埋め合わせる（材質が既定の白のまま残らない）。
    // 層のテクスチャはパスが同じなら Renderer が読み直さないので、入れ直しは安い。
    if (updateSurface || full) {
        renderer_.setTerrainSurface(wizengine::toTerrainSurface(desc, layers));
    }
}
