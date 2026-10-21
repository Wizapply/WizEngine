// テレインの実体（PHYSICS / RENDER スレッド）: 文書の <terrain> から高さ格子を
// 作り、自前の接触（当たり判定）を張り直し、描画のチャンクへ同期する。
// ブラウザからの入口は applyEditorOp の "terrain" 分岐（applyTerrainOp）。
// dirty の取り出し（ロックの枠組み）は applyToRenderer（Scene.cpp）が持つ。
#include "scene/SceneInternal.h"

#include <chrono>

using namespace chrono;
using namespace scene_detail;

// ---- テレイン（シーン文書の <terrain>）--------------------------------------

namespace {

// 文書の値 → 高さ格子の生成パラメータ（src/terrain は文書の型を知らない）。
wizengine::terrain::TerrainParams toTerrainParams(const ed::TerrainDesc& d) {
    wizengine::terrain::TerrainParams p;
    p.size = float(d.size);
    p.resolution = d.resolution;
    p.height = float(d.height);
    p.posX = float(d.position.x);
    p.posY = float(d.position.y);
    p.posZ = float(d.position.z);
    p.seed = d.seed;
    p.octaves = d.octaves;
    p.gain = float(d.gain);
    p.lacunarity = float(d.lacunarity);
    p.scale = float(d.scale);
    return p;
}

// 形（高さ格子と当たり判定）を決める値が同じか。
bool sameTerrainShape(const ed::TerrainDesc& a, const ed::TerrainDesc& b) {
    return a.size == b.size && a.resolution == b.resolution && a.height == b.height &&
           a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.seed == b.seed && a.octaves == b.octaves &&
           a.gain == b.gain && a.lacunarity == b.lacunarity && a.scale == b.scale;
}

// 表面（描画の材質だけ）が同じか。
bool sameTerrainSurface(const ed::TerrainDesc& a, const ed::TerrainDesc& b) {
    return a.texture == b.texture && a.tile == b.tile && a.color.r == b.color.r &&
           a.color.g == b.color.g && a.color.b == b.color.b &&
           a.roughness == b.roughness && a.metallic == b.metallic;
}

}  // namespace

void Scene::setTerrain(bool hasTerrain, const ed::TerrainDesc& descIn) {
    const ed::TerrainDesc desc = ed::clampTerrain(descIn);
    // terrain_ / hasTerrain_ / terrainField_ を書くのはこのスレッドだけなので、
    // 読むだけならロックは要らない（地面と同じ約束）。
    const bool had = hasTerrain_;
    const bool shapeChanged =
        hasTerrain && (!had || !terrainField_ || !sameTerrainShape(desc, terrain_));
    const bool surfaceChanged =
        hasTerrain && (!had || !sameTerrainSurface(desc, terrain_));

    // 高さ格子の生成はロックの外で（解像度 1024 で数十 ms〜。描画スレッドを
    // 待たせない）。
    std::shared_ptr<const wizengine::terrain::HeightField> field;
    if (shapeChanged) {
        const auto t0 = std::chrono::steady_clock::now();
        field = std::make_shared<const wizengine::terrain::HeightField>(
            wizengine::terrain::generateHeightField(toTerrainParams(desc)));
        LOGI("scene", "terrain height field: %dx%d cells, %.0f m, generated in %.1f ms",
             desc.resolution, desc.resolution, desc.size, elapsedMs(t0));
    }
    {
        // 格子の差し替えと dirty は同じロックの中で: 描画スレッドは dirty を
        // 見たのと同じロックの中で格子を取り出すので、「新しい dirty と古い
        // 格子」の組を見ることが無い。ロック順は objects → terrainField（末端）。
        std::lock_guard<std::mutex> lk(objectsMutex_);
        terrain_ = desc;  // 無しでも値は覚えておく（/scene の欄に出す）
        hasTerrain_ = hasTerrain;
        if (shapeChanged || (had && !hasTerrain)) {
            std::lock_guard<std::mutex> fl(terrainFieldMutex_);
            terrainField_ = hasTerrain ? field : nullptr;
            terrainMeshDirty_ = true;  // 無しになったときは描画側が外す
        }
        if (surfaceChanged) terrainSurfaceDirty_ = true;
    }

    // テレインの有無は系の種類を変えうる（kTerrainForcesCore）。作り直したら
    // テレインの当たり判定もそこで組み直し済みなので、二重に張らない。
    const bool rebuilt = syncBackend();
    if (rebuilt) return;
    if (!hasTerrain) {
        if (had) rebuildTerrainBody();  // 接触を止める
        return;
    }
    // 張り直しはゴーストに格子を渡すだけ（高さ格子の生成より軽い）なので間引かない =
    // エディタ中の edit.terrain も即時に当たり判定へ映る。
    if (shapeChanged) rebuildTerrainBody();
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
        // 張り直しのたびに来る（スライダーで連発する）ので 1 回だけ。
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGW("scene", "terrain: custom contact is not available on the %s backend - "
                          "the terrain is visible but nothing collides with it",
                 physics_.backendName());
        }
        return;
    }
    terrainPhysId_ = id;
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

// edit.terrain（PHYSICS スレッド）。applyEditorOp の "terrain" 分岐から呼ぶ。
void Scene::applyTerrainOp(const EditorState::Op& op) {
    const nlohmann::json& a = op.args;
    // 送られてきたキーだけ上書き（edit.ground と同じ部分更新）。テレインが
    // 無い間も値は覚えている（terrain_）ので、enabled = true だけを送れば
    // 直前の設定（一度も無ければ既定値）で作られる。enabled を送らなければ
    // 有無はそのまま（無い間に送った値は次に有効にしたときに効く）。
    const ed::TerrainDesc t = ed::clampTerrain(ed::terrainFromJson(a, terrain_));
    bool on = hasTerrain_;
    if (a.contains("enabled") && a["enabled"].is_boolean()) on = a["enabled"].get<bool>();
    // 当たり判定はモードによらずその場で張り直す（setTerrain）。
    // ステータスは setTerrain の前に置く: 有無の切替で系が変わったときは
    // syncBackend の「物理を切り替えました」が上書きして残る（clear と同じ順序）。
    if (on == hasTerrain_) {
        editor_.setStatus(on ? "テレインを更新" : "テレインの設定を更新（無効のまま）");
    } else {
        editor_.setStatus(on ? "テレインを有効にしました" : "テレインを無効にしました");
    }
    setTerrain(on, t);
    return;
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
