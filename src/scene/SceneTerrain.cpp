// テレインの実体（PHYSICS スレッド）: 高さ格子の差し替え
// （publishTerrainField）と形の変更（reshapeTerrain）、格子の書き換え（コピー
// オンライトの editTerrainField = ノイズ生成・取り消し・筆）、自前の接触（当たり
// 判定）の張り直し。
// 入口はブラウザの edit.terrain が SceneTerrainEdit.cpp、シーン文書・保存が
// SceneTerrainFile.cpp、筆のストロークが SceneTerrainSculpt.cpp。描画への同期は
// SceneTerrainRender.cpp（dirty の取り出し = ロックの枠組みは applyToRenderer）。
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

void Scene::settleTerrainFileState(TerrainFileState file, bool on, bool missing) {
    switch (file) {
        case TerrainFileState::Keep:
            if (!on && !missing) terrainFile_ = TerrainFileState::Saved;
            break;
        case TerrainFileState::Dirty:
            terrainFile_ = on ? TerrainFileState::Dirty : TerrainFileState::Saved;
            break;
        case TerrainFileState::Saved:
            terrainFile_ = TerrainFileState::Saved;
            break;
        case TerrainFileState::Missing:
        case TerrainFileState::Unreadable:
            terrainFile_ = on ? TerrainFileState::Saved : file;
            break;
    }
}

void Scene::publishTerrainField(const ed::TerrainDesc& desc,
                                std::shared_ptr<const wizengine::terrain::HeightField> field,
                                TerrainHistory history, TerrainFileState file,
                                const wizengine::terrain::GridRect& changedCells,
                                bool brushPass) {
    // ストローク中に飛ばした張り直し（Clear はストロークを捨てるので先に読む）。
    const bool collisionPending = terrainStroke_.collisionStale;
    if (history == TerrainHistory::Clear) abortTerrainStroke();
    // terrain_ / hasTerrain_ / terrainField_ を書くのはこのスレッドだけ（読むのにロック不要）。
    const bool had = hasTerrain_;
    const bool on = field != nullptr;
    // 文書上はある・格子は無い（Missing）。描画・当たり判定は格子 null の
    // ときの処理のまま（外す）。Keep で格子無しが来たときは今の Missing を保つ
    // （黙って「テレイン無し」に落とさない）。
    const bool missing =
        !on && (file == TerrainFileState::Missing || file == TerrainFileState::Unreadable ||
                (file == TerrainFileState::Keep && terrainMissing()));
    const bool fieldChanged = field != terrainField_;
    const bool surfaceChanged = on && (!had || !sameTerrainSurface(desc, terrain_));
    // 一部だけの書き換えなら、描画は触ったチャンクだけ（空 = 全部作り直す）。
    const wizengine::terrain::GridRect chunks =
        (fieldChanged && on && !changedCells.empty())
            ? field->chunksTouched(changedCells, kTerrainChunkQuads)
            : wizengine::terrain::GridRect{};
    // テレインのフレーム（置き場所 + 回転）。格子は原点基準なので、動かす・回すは
    // ここだけ（格子・メッシュは作り直さない）。
    const BodyTransform pose = placementPose(desc.position, desc.rotation);
    bool poseChanged = false;
    {
        // 格子の差し替えと dirty は同じロックの中で: 描画スレッドは dirty を
        // 見たのと同じロックの中で格子を取り出すので、「新しい dirty と古い
        // 格子」の組を見ることが無い。ロック順は objects → terrainField（末端）。
        std::lock_guard<std::mutex> lk(objectsMutex_);
        terrain_ = desc;  // 無しでも値は覚えておく（/scene の欄に出す）
        hasTerrain_ = on || missing;
        {
            std::lock_guard<std::mutex> fl(terrainFieldMutex_);
            if (fieldChanged) {
                terrainField_ = std::move(field);
                if (chunks.empty()) terrainMeshDirty_ = true;  // 無しなら描画側が外す
                else terrainChunkDirty_ = terrainChunkDirty_.united(chunks);
            }
            if (!samePose(pose, terrainPose_)) {
                terrainPose_ = pose;
                terrainPoseDirty_ = true;
                poseChanged = true;
            }
        }
        if (surfaceChanged) terrainSurfaceDirty_ = true;
        // ファイルとの関係（外したら Saved）。/scene が「有無と Missing」の食い違った
        // 組を見ないよう、hasTerrain_ と同じロックの中で書く。
        settleTerrainFileState(file, on, missing);
    }
    if (history == TerrainHistory::Clear) terrainUndo_->clear();
    // 外したテレインを選んだままにしない（Inspector に欄だけ残る）。
    if (!hasTerrain_ && editor_.selKind() == EditorState::SelKind::Terrain) {
        editor_.clearSel();
    }

    // 接触の側のフレーム（作り直しの rebuildTerrainBody も当て直す）。
    if (poseChanged) physics_.setTerrainFrame(ChVector3d(pose.px, pose.py, pose.pz),
                                              ChQuaternion<>(pose.qw, pose.qx, pose.qy,
                                                             pose.qz));

    // テレインの有無は系の種類を変えうる（kTerrainForcesCore）。作り直したら
    // テレインの当たり判定もそこで組み直し済みなので、二重に張らない。
    // 省くのは筆の 1 パス（brushPass）だけ: 同じ形の格子の中身を書き換えるだけで
    // テレインの有無は変わらず、1 パスごとにここを通る。それ以外は必ず判定する
    // （全消しなど、テレインの有無が変わらなくてもこの判定で系を選び直す呼び手がいる）。
    if (!brushPass && syncBackend()) return;
    // 張り直しはゴーストに格子を渡すだけ（高さ格子の生成より軽い）なので間引かない =
    // エディタ中の edit.terrain も即時に当たり判定へ映る。格子が同じでも、
    // テレインがあるのに当たり判定が無く、その理由が系の制約（Multicore =
    // terrainBackendWarned_）でない（例外で作れなかった）なら張り直す。系の制約
    // なら表面の変更のたびに張り直さない（O(res²) と全ボディの wakeAll が走る）-
    // Core へ戻ったときは syncBackend → rebuildPhysicsWorld → rebuildTerrainBody
    // が張る。無しになったときは接触を止める（rebuildTerrainBody が格子無しを見る）。
    // 筆のストローク中は張らずに印だけ立てる（endTerrainStroke が 1 回張る）。
    const bool retryFailed =
        on && terrainPhysId_ == PhysicsWorld::kInvalidId && !terrainBackendWarned_;
    if (!(fieldChanged || retryFailed || collisionPending)) return;
    if (terrainStroke_.active) terrainStroke_.collisionStale = true;
    else rebuildTerrainBody();
}

void Scene::reshapeTerrain(const ed::TerrainDesc& next) {
    const std::shared_ptr<const wizengine::terrain::HeightField> field = terrainField();
    const ed::TerrainDesc& now = terrain_;
    const bool sized = next.size != now.size;
    const bool resampled = next.resolution != now.resolution;
    const bool scaled = next.height != now.height;
    // .terrain が持つ形（size / resolution / height）が変わったか。pos / euler は
    // XML が持ち、格子は原点基準なので、置き直し・回転は格子に触らない（フレーム
    // だけ = publishTerrainField が描画と接触へ渡す）。ファイルも Undo の記録
    // （セルの添字と相対高さ）もそのまま。
    const bool fileChanged = sized || resampled || scaled;
    if (!field || !fileChanged) {
        // 形はそのまま（またはテレイン無し = 値だけ覚える）: 表面と置き場所だけ。
        publishTerrainField(next, field, TerrainHistory::Keep, TerrainFileState::Keep);
        return;
    }
    // 形の変更は今の格子のコピーから作る（共有中の格子は書き換えない）。
    // 大きさ → 再サンプル → 比例伸縮の順。
    const auto t0 = std::chrono::steady_clock::now();
    wizengine::terrain::HeightField grid =
        sized ? field->withGeometry(float(next.size), 0.0f, 0.0f, 0.0f) : *field;
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

bool Scene::editTerrainField(
    const wizengine::terrain::GridRect& rect,
    const std::function<bool(wizengine::terrain::HeightField&)>& mutate,
    TerrainRecord record) {
    // 筆以外の書き換え（取り消し・生成）はストロークを先に終える: ストロークの
    // 記録の後ろに積む / ストロークごと取り消す、の順序にする。
    if (record != TerrainRecord::Stroke) endTerrainStroke();
    const std::shared_ptr<const wizengine::terrain::HeightField> field = terrainField();
    if (!field) return false;
    // 共有中の格子（描画・当たり判定が持っている）は書き換えず、コピーを差し替える。
    // *field は書き換え前のまま残る（ストロークの「変更前」に使う）。
    wizengine::terrain::HeightField copy = *field;
    wizengine::terrain::TerrainEdit before;
    if (record == TerrainRecord::Undo) before = wizengine::terrain::captureRegion(copy, rect);
    if (!mutate(copy)) return false;
    // 書き換えが成功してから記録する（失敗した操作の記録を残さない）。
    if (record == TerrainRecord::Undo) {
        terrainUndo_->push(std::move(before));
    } else if (record == TerrainRecord::Stroke) {
        terrainStroke_.before = wizengine::terrain::growEdit(*field, terrainStroke_.before, rect);
        terrainStroke_.cells = terrainStroke_.cells.united(rect);
    }
    publishTerrainField(terrain_,
                        std::make_shared<const wizengine::terrain::HeightField>(std::move(copy)),
                        TerrainHistory::Keep, TerrainFileState::Dirty, rect,
                        record == TerrainRecord::Stroke);
    return true;
}

bool Scene::replaceTerrainHeights(wizengine::terrain::HeightField&& gen, const char* what) {
    const std::shared_ptr<const wizengine::terrain::HeightField> field = terrainField();
    if (!field) return false;
    const int res = field->resolution();
    const bool ok = editTerrainField(
        wizengine::terrain::GridRect{0, 0, res, res},
        [&gen](wizengine::terrain::HeightField& f) {
            // 形は terrain_ と同じはずだが、食い違えば書き換えない。
            if (gen.samples().size() != f.samples().size()) return false;
            f.samples() = std::move(gen.samples());  // gen はここで使い終わり
            return true;
        },
        TerrainRecord::Undo);
    if (!ok) {
        LOGW("scene", "terrain: the %s grid does not match the current grid - not applied",
             what);
    }
    return ok;
}

void Scene::generateTerrainNoise(const ed::TerrainNoiseParams& n) {
    const std::shared_ptr<const wizengine::terrain::HeightField> field = terrainField();
    if (!field) {
        editor_.setStatus("テレインがありません（ノイズで生成できません）");
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    // 今の形（大きさ・解像度・高さ・位置）でノイズの格子を作り、その高さだけを
    // 今の格子へ入れる（穴は今のまま残る）。
    const int res = field->resolution();
    // ステータスは結果が分かってから 1 回だけ。
    if (!replaceTerrainHeights(
            wizengine::terrain::generateHeightField(toTerrainParams(terrain_, n)),
            "generated")) {
        editor_.setStatus("テレインをノイズで生成できませんでした（コンソール参照）");
        return;
    }
    editor_.setStatus("テレインをノイズで生成しました（種 " + std::to_string(n.seed) +
                      "、取り消しできます）");
    LOGI("scene", "terrain generated from noise (seed %u, %d octaves): %dx%d cells in %.1f ms",
         n.seed, n.octaves, res, res, elapsedMs(t0));
}

void Scene::undoTerrain() {
    // 筆のストローク中なら先に終えて積む（取り出す前に。= そのストロークを戻す）。
    endTerrainStroke();
    const wizengine::terrain::TerrainEdit edit = terrainUndo_->pop();
    if (edit.empty()) {
        editor_.setStatus("取り消すものがありません");
        return;
    }
    // 戻す操作そのものは記録しない（Redo は作らない）。applyEdit が空を返す =
    // 解像度が違う格子など、当てられない記録（取り出したまま捨てる）。
    const bool ok = editTerrainField(
        edit.rect,
        [&edit](wizengine::terrain::HeightField& f) {
            return !wizengine::terrain::applyEdit(f, edit).empty();
        },
        TerrainRecord::None);
    // ステータスは結果が分かってから 1 回だけ。
    if (!ok) {
        LOGW("scene", "terrain: an undo record (resolution %d) does not fit the current grid "
                      "- discarded", edit.resolution);
        editor_.setStatus("テレインの編集を取り消せませんでした");
        return;
    }
    editor_.setStatus("テレインの編集を取り消しました");
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
    const std::shared_ptr<const wizengine::terrain::HeightField> field = terrainField();
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
    {
        // フレームは格子と組（PhysicsWorld が作り直されても当て直す）。
        BodyTransform pose;
        copyTerrainFieldAndPose(pose);
        physics_.setTerrainFrame(ChVector3d(pose.px, pose.py, pose.pz),
                                 ChQuaternion<>(pose.qw, pose.qx, pose.qy, pose.qz));
    }
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
    const bool affected = terrainLoaded() && s.contact != "smc" &&
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

std::shared_ptr<const wizengine::terrain::HeightField> Scene::copyTerrainFieldAndPose(
    BodyTransform& pose) const {
    std::lock_guard<std::mutex> fl(terrainFieldMutex_);
    pose = terrainPose_;
    return terrainField_;
}

std::shared_ptr<const wizengine::terrain::HeightField> Scene::terrainField(
    wizengine::terrain::TerrainFrame* frame) const {
    // ロックの中では写すだけ（描画スレッドも同じロックを取るので持つ時間を延ばさない）。
    BodyTransform p;
    std::shared_ptr<const wizengine::terrain::HeightField> field = copyTerrainFieldAndPose(p);
    if (frame) {
        *frame = wizengine::terrain::TerrainFrame::fromPose(p.px, p.py, p.pz, p.qw, p.qx,
                                                            p.qy, p.qz);
    }
    return field;
}

// ---- ギズモの移動 / 回転（PHYSICS thread）----------------------------------
// フレームだけを変える（reshapeTerrain は形が同じなら格子に触らない）。
// Missing（格子が無い）でも値は覚える（ファイルを戻したらそこに出る）。
// 掴み点 = 原点 + R·(0, 中心の地表の高さ, 0)。

namespace {
// 格子の中心の地表の高さ（ローカル = 最低点からの高さ）。格子が無ければ 0。
double centerHeight(const std::shared_ptr<const wizengine::terrain::HeightField>& field) {
    if (!field || !field->valid()) return 0.0;
    const double h = field->heightAt(0.0, 0.0);
    return std::isfinite(h) ? h : 0.0;
}
// R·(0, h, 0)（オイラー角は度）。
ed::Vec3d liftedBy(const ed::Vec3d& eulerDeg, double h) {
    const scenemath::Vec3 v =
        scenemath::quatFromEulerDegrees(eulerDeg.x, eulerDeg.y, eulerDeg.z) *
        scenemath::Vec3(0.0, h, 0.0);
    return {v.x(), v.y(), v.z()};
}
}  // namespace

ed::Vec3d Scene::terrainHandlePoint() const {
    const ed::Vec3d up = liftedBy(terrain_.rotation, centerHeight(terrainField()));
    return {terrain_.position.x + up.x, terrain_.position.y + up.y,
            terrain_.position.z + up.z};
}

void Scene::moveTerrain(double x, double y, double z) {
    if (!hasTerrain_) return;
    ed::TerrainDesc next = terrain_;
    const ed::Vec3d up = liftedBy(terrain_.rotation, centerHeight(terrainField()));
    next.position = {x - up.x, y - up.y, z - up.z};
    reshapeTerrain(ed::clampTerrain(next));
}

void Scene::rotateTerrain(double xDeg, double yDeg, double zDeg) {
    if (!hasTerrain_) return;
    // 掴み点を動かさずに回す = 新しい回転で持ち上げた分だけ原点をずらす。
    const ed::Vec3d handle = terrainHandlePoint();
    ed::TerrainDesc next = terrain_;
    next.rotation = {xDeg, yDeg, zDeg};
    const ed::Vec3d up = liftedBy(next.rotation, centerHeight(terrainField()));
    next.position = {handle.x - up.x, handle.y - up.y, handle.z - up.z};
    reshapeTerrain(ed::clampTerrain(next));
}
