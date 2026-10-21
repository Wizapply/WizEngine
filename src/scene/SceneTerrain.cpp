// テレインの実体（PHYSICS / RENDER スレッド）: 高さ格子の差し替え
// （publishTerrainField）と形の変更（reshapeTerrain）、自前の接触（当たり判定）の
// 張り直し、描画のチャンクへの同期。
// 入口（ブラウザの edit.terrain・シーン文書・保存）は SceneTerrainEdit.cpp。
// dirty の取り出し（ロックの枠組み）は applyToRenderer（Scene.cpp）が持つ。
#include "scene/SceneInternal.h"

#include <chrono>

using namespace chrono;
using namespace scene_detail;

// ---- テレイン（シーン文書の <terrain>）--------------------------------------

namespace {

// 表面（描画の材質だけ）が同じか。
bool sameTerrainSurface(const ed::TerrainDesc& a, const ed::TerrainDesc& b) {
    return a.texture == b.texture && a.tile == b.tile && a.color.r == b.color.r &&
           a.color.g == b.color.g && a.color.b == b.color.b &&
           a.roughness == b.roughness && a.metallic == b.metallic;
}

}  // namespace

void Scene::publishTerrainField(const ed::TerrainDesc& desc,
                                std::shared_ptr<const wizengine::terrain::HeightField> field,
                                TerrainHistory history, TerrainFileState file) {
    // terrain_ / hasTerrain_ / terrainField_ を書くのはこのスレッドだけなので、
    // 読むだけならロックは要らない（地面と同じ約束）。
    const bool had = hasTerrain_;
    const bool on = field != nullptr;
    const bool fieldChanged = field != terrainField_;
    const bool surfaceChanged = on && (!had || !sameTerrainSurface(desc, terrain_));
    {
        // 格子の差し替えと dirty は同じロックの中で: 描画スレッドは dirty を
        // 見たのと同じロックの中で格子を取り出すので、「新しい dirty と古い
        // 格子」の組を見ることが無い。ロック順は objects → terrainField（末端）。
        std::lock_guard<std::mutex> lk(objectsMutex_);
        terrain_ = desc;  // 無しでも値は覚えておく（/scene の欄に出す）
        hasTerrain_ = on;
        if (fieldChanged) {
            std::lock_guard<std::mutex> fl(terrainFieldMutex_);
            terrainField_ = std::move(field);
            terrainMeshDirty_ = true;  // 無しになったときは描画側が外す
        }
        if (surfaceChanged) terrainSurfaceDirty_ = true;
    }
    if (history == TerrainHistory::Clear) terrainUndo_->clear();
    // 無いテレインに書くべき変更は無い（外したら指定によらず戻す）。
    if (!on || file == TerrainFileState::Saved) {
        terrainDirty_ = false;
    } else if (file == TerrainFileState::Dirty) {
        terrainDirty_ = true;
    }

    // テレインの有無は系の種類を変えうる（kTerrainForcesCore）。作り直したら
    // テレインの当たり判定もそこで組み直し済みなので、二重に張らない。
    if (syncBackend()) return;
    // 張り直しはゴーストに格子を渡すだけ（高さ格子の生成より軽い）なので間引かない =
    // エディタ中の edit.terrain も即時に当たり判定へ映る。格子が同じでも、
    // テレインがあるのに当たり判定が無く、その理由が系の制約（Multicore =
    // terrainBackendWarned_）でない（例外で作れなかった）なら張り直す。系の制約
    // なら表面の変更のたびに張り直さない（O(res²) と全ボディの wakeAll が走る）-
    // Core へ戻ったときは syncBackend → rebuildPhysicsWorld → rebuildTerrainBody
    // が張る。無しになったときは接触を止める（rebuildTerrainBody が格子無しを見る）。
    const bool retryFailed =
        on && terrainPhysId_ == PhysicsWorld::kInvalidId && !terrainBackendWarned_;
    if (fieldChanged || retryFailed) rebuildTerrainBody();
}

void Scene::reshapeTerrain(const ed::TerrainDesc& next) {
    const std::shared_ptr<const wizengine::terrain::HeightField> field =
        hasTerrain_ ? terrainField_ : nullptr;
    const ed::TerrainDesc& now = terrain_;
    const bool sized = next.size != now.size;
    const bool moved = next.position.x != now.position.x ||
                       next.position.y != now.position.y ||
                       next.position.z != now.position.z;
    const bool resampled = next.resolution != now.resolution;
    const bool scaled = next.height != now.height;
    // .terrain が持つ形（size / resolution / height）が変わったか。pos は XML が
    // 持つので、置き直しだけならファイルは書き直さず、Undo の記録（セルの添字と
    // 相対高さ）もそのまま使える。
    const bool fileChanged = sized || resampled || scaled;
    if (!field || !(moved || fileChanged)) {
        // 形はそのまま（またはテレイン無し = 値だけ覚える）: 表面だけ。
        publishTerrainField(next, field, TerrainHistory::Keep, TerrainFileState::Keep);
        return;
    }
    // 形の変更は今の格子のコピーから作る（共有中の格子は書き換えない）。
    // 置き直し → 再サンプル → 比例伸縮の順。
    const auto t0 = std::chrono::steady_clock::now();
    wizengine::terrain::HeightField grid =
        (sized || moved)
            ? field->withGeometry(float(next.size), float(next.position.x),
                                  float(next.position.y), float(next.position.z))
            : *field;
    if (resampled) grid = grid.resampled(next.resolution);
    // 高さは [0, height] に保つ（.terrain の量子化の範囲）。height は
    // clampTerrain が kTerrainMinHeight 以上に保つので、倍率は有限。
    if (scaled) grid.scaleHeights(float(next.height / now.height));
    LOGI("scene", "terrain reshaped: %dx%d cells, %.0f m, %.0f m high in %.1f ms",
         next.resolution, next.resolution, next.size, next.height, elapsedMs(t0));
    publishTerrainField(
        next, std::make_shared<const wizengine::terrain::HeightField>(std::move(grid)),
        fileChanged ? TerrainHistory::Clear : TerrainHistory::Keep,
        fileChanged ? TerrainFileState::Dirty : TerrainFileState::Keep);
}

void Scene::rebuildTerrainBody() {
    // 例外はここで止める（stepEditor など main の guarded の外からも呼ばれうる）。
    // 失敗したら当たり判定は無いまま続ける（高さ格子と見た目はそのまま）。
    // 番号は setTerrain の前に手放すので、途中で投げると PhysicsWorld 側は
    // 格子を差し替え済みのことがある（「当たらない」と言いながら物は当たり、
    // 車輪のレイは使わない食い違い）。releaseTerrainContact が接触そのものも止める。
    try {
        rebuildTerrainBodyUnguarded();
    } catch (const std::exception& e) {
        releaseTerrainContact();
        LOGE("scene", "terrain: building the collision failed (%s) - the terrain is "
                      "visible but nothing collides with it", e.what());
        editor_.setStatus(std::string("テレインの当たり判定を作れませんでした（") +
                          e.what() + "）- 見た目だけ残します");
    } catch (...) {
        releaseTerrainContact();
        LOGE("scene", "terrain: building the collision failed (unknown exception) - the "
                      "terrain is visible but nothing collides with it");
        editor_.setStatus("テレインの当たり判定を作れませんでした（不明な例外）- "
                          "見た目だけ残します");
    }
}

void Scene::releaseTerrainContact() {
    // 番号は止める前に手放す（clearTerrain が投げても古い番号を持ち続けない）。
    // ゴーストは PhysicsWorld の持ち物なので外さない（接触だけ止める）。
    terrainPhysId_ = PhysicsWorld::kInvalidId;
    terrainBackendWarned_ = false;
    try {
        if (physics_.hasTerrain()) physics_.clearTerrain();
    } catch (const std::exception& e) {
        LOGE("scene", "terrain: clearing the custom contact failed (%s)", e.what());
    } catch (...) {
        LOGE("scene", "terrain: clearing the custom contact failed (unknown exception)");
    }
}

void Scene::rebuildTerrainBodyUnguarded() {
    // 自前の接触（PhysicsWorld::setTerrain）。terrainPhysId_ はゴーストの番号
    // （接触の報告にはこの番号が出る）。ゴーストは PhysicsWorld の持ち物で、
    // 差し替えでも同じ番号を使い回す。
    const std::shared_ptr<const wizengine::terrain::HeightField> field =
        hasTerrain_ ? terrainField_ : nullptr;
    if (!field || !field->valid()) {
        // 無し: 接触だけ止める（ゴーストは残す）。
        releaseTerrainContact();
        return;
    }
    terrainPhysId_ = PhysicsWorld::kInvalidId;  // 古い番号は手放す（同じ番号が返る）
    // 材質（案 A）: 摩擦・反発・転がりは全部シーン任せ（負）。PhysicsWorld が
    // 「球 = このまま（転がり摩擦あり）/ 箱など多点の形 = 転がり・スピン 0」
    // に分ける（BB ソルバで斜面の箱が沈むため。ベンチ B1 / B2）。
    // **`<option combine>` が average / max だと箱の転がり 0 が効かない**
    // （相手の材質との平均 / 大きい方になる）- terrainCombineCaveat が知らせる。
    // **衝突レイヤ（<geom layer nocollide>）は見ない**: 自前の接触は Chrono の
    // 衝突ファミリを通らないので、どのレイヤの物もテレインに当たる（未対応）。
    const BodyOptions terrainOpt;
    const std::size_t id = physics_.setTerrain(field, terrainOpt);
    if (id == PhysicsWorld::kInvalidId) {
        // Multicore（kTerrainForcesCore = false のとき）など。前の格子で
        // 当たり続けないように外しておく。
        if (physics_.hasTerrain()) physics_.clearTerrain();
        // 張り直しのたびに来る（スライダーで連発する）ので 1 回だけ
        // （releaseTerrainContact = テレインを外す・作り直すたびに戻す）。
        if (!terrainBackendWarned_) {
            terrainBackendWarned_ = true;
            LOGW("scene", "terrain: custom contact is not available on the %s backend - "
                          "the terrain is visible but nothing collides with it",
                 physics_.backendName());
        }
        return;
    }
    terrainPhysId_ = id;
    terrainBackendWarned_ = false;  // 張れた = 次に張れなくなったら知らせ直す
}

void Scene::warnTerrainPassThrough() {
    if (!hasTerrainCollision()) {
        terrainPassThroughWarned_.clear();
        return;
    }
    // 判定は PhysicsWorld::terrainContactShape の 1 か所（形の記録の有無と、今の
    // 固定・当たり判定の状態）。記録の無い動く物も安全網（衝突モデルの AABB の
    // 8 角）で当たるので素通りはしないが、当たり方が粗い（傾いた物は外接箱で
    // 浮いて見える）ので名前を知らせる。ソフトボディは代表番号（粒子 1 個）で
    // 聞く（粒子は全部同じ作り）。
    std::string list;
    std::size_t count = 0;
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        const GameObject& obj = boxes_[i];
        if (!obj.alive || obj.physId == GameObject::kInvalidId) continue;
        if (physics_.terrainContactShape(obj.physId) !=
            PhysicsWorld::TerrainContactShape::BoundingBox)
            continue;
        if (count < 8) list += (count ? ", " : "") + std::string("'") + obj.desc.name + "'";
        ++count;
    }
    if (count > 8) list += ", ... (" + std::to_string(count) + " in total)";
    if (list == terrainPassThroughWarned_) return;
    terrainPassThroughWarned_ = list;
    if (count == 0) return;
    LOGW("scene", "terrain: %zu moving bodies have no terrain shape record and touch the "
                  "terrain with their bounding box (coarse): %s", count, list.c_str());
}

std::string Scene::terrainCombineCaveat(bool force) {
    if (force) terrainCombineWarned_.clear();
    const ed::SimSettings s = editor_.sim();
    // 転がり摩擦は NSC だけの欄（SMC には無い = 何も変わらない）。
    const bool affected = hasTerrain_ && s.contact != "smc" &&
                          (s.combine == "average" || s.combine == "max");
    if (!affected) {
        terrainCombineWarned_.clear();
        return std::string();
    }
    if (s.combine == terrainCombineWarned_) return std::string();
    terrainCombineWarned_ = s.combine;
    LOGW("scene", "terrain: <option combine=\"%s\"> mixes the terrain's zero rolling friction "
                  "with the body's - boxes on slopes may sink into the terrain (use \"min\")",
         s.combine.c_str());
    return "（注意: 合成方式が " + s.combine +
           " なので、テレインの斜面で箱が沈むことがあります - min を推奨）";
}

std::shared_ptr<const wizengine::terrain::HeightField> Scene::terrainField() const {
    std::lock_guard<std::mutex> fl(terrainFieldMutex_);
    return terrainField_;
}

bool Scene::terrainRaycast(const double origin[3], const double dir[3], double maxDist,
                           double& t, double normal[3]) const {
    // 格子は共有ポインタで受け取ってからロックの外で辿る（差し替えられても
    // 手元の格子は生きている）。
    const std::shared_ptr<const wizengine::terrain::HeightField> field = terrainField();
    if (!field) return false;
    const wizengine::terrain::Vec3 o{origin[0], origin[1], origin[2]};
    const wizengine::terrain::Vec3 d{dir[0], dir[1], dir[2]};
    double hitT = 0.0;
    wizengine::terrain::Vec3 n = wizengine::terrain::Vec3::Zero();
    // 判定の戻り値は一度変数に受けてから使う（出力引数と同じ式に並べない規約）。
    const bool hit = field->raycast(o, d, maxDist, hitT, n);
    if (!hit) return false;
    t = hitT;
    normal[0] = n.x();
    normal[1] = n.y();
    normal[2] = n.z();
    return true;
}

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

// RENDER スレッド（applyToRenderer がロックを外してから呼ぶ）。
void Scene::syncTerrain(bool rebuildMesh, bool updateSurface, bool enabled,
                        const ed::TerrainDesc& desc,
                        const std::shared_ptr<const wizengine::terrain::HeightField>& field) {
    if (!enabled || (rebuildMesh && (!field || !field->valid()))) {
        renderer_.removeTerrain();  // 無ければ何もしない
        return;
    }
    if (rebuildMesh) {
        const auto t0 = std::chrono::steady_clock::now();
        const int chunks = field->chunksPerSide(kTerrainChunkQuads);
        if (!renderer_.addTerrain(chunks, chunks, kTerrainChunkQuads)) {
            LOGW("scene", "terrain: the renderer refused %dx%d chunks - not drawn", chunks,
                 chunks);
            return;
        }
        wizengine::terrain::ChunkMesh built;
        wizengine::TerrainChunkMesh upload;
        int failed = 0;
        for (int cz = 0; cz < chunks; ++cz) {
            for (int cx = 0; cx < chunks; ++cx) {
                const bool ok = field->buildChunkMesh(cx, cz, kTerrainChunkQuads, built);
                if (!ok) {
                    ++failed;
                    continue;
                }
                // 同じ並びの素の配列なので move で渡す（コピーしない）。インデックス
                // も毎回渡す: addTerrain の直後は全部 0 = 何も描かれないため。
                upload.positions = std::move(built.positions);
                upload.normals = std::move(built.normals);
                upload.uvs = std::move(built.uvs);
                upload.indices = std::move(built.indices);
                if (!renderer_.setTerrainChunk(cx, cz, upload)) ++failed;
            }
        }
        if (failed > 0) {
            LOGW("scene", "terrain: %d of %d chunks could not be built", failed,
                 chunks * chunks);
        }
        LOGI("scene", "terrain mesh: %dx%d chunks (%d quads each) built and uploaded in %.1f ms",
             chunks, chunks, kTerrainChunkQuads, elapsedMs(t0));
    }
    // メッシュを作り直したときは表面も必ず入れ直す。addTerrain が失敗した
    // フレームは上で return しており、そのとき消えた表面の dirty は次の
    // 作り直しがここで埋め合わせる（材質が既定の白のまま残らない）。
    // テクスチャはパスが同じなら Renderer が読み直さないので、入れ直しは安い。
    if (updateSurface || rebuildMesh) {
        renderer_.setTerrainSurface(
            desc.texture, float(desc.tile),
            filament::math::float4{desc.color.r, desc.color.g, desc.color.b, 1.0f},
            float(desc.roughness), float(desc.metallic));
    }
}
