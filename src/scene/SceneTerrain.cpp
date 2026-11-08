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

// 表面（描画の材質だけ。層は terrainLayers_ で別に見る）が同じか。
bool sameTerrainSurface(const ed::TerrainDesc& a, const ed::TerrainDesc& b) {
    return a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b &&
           a.roughness == b.roughness && a.metallic == b.metallic;
}

// .terrain が持つ値（層・散布物）の次の値を選ぶ規則（publishTerrainField）: 渡されれば
// それ。渡されず同じファイル（sameFile）なら空 = 今のまま、それ以外（格子が無くなる・
// 別のテレイン）は既定（fallback）。P はポインタの形（生ポインタか shared_ptr）。
template <class P>
P nextFileOwned(P passed, bool sameFile, P fallback) {
    if (passed) return passed;
    return sameFile ? P{} : fallback;
}

}  // namespace

std::shared_ptr<const wizengine::terrain::TerrainScatter> Scene::defaultTerrainScatter() {
    static const std::shared_ptr<const wizengine::terrain::TerrainScatter> kDefault =
        std::make_shared<const wizengine::terrain::TerrainScatter>();
    return kDefault;
}

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
                                bool brushPass,
                                const wizengine::terrain::TerrainLayers* layers,
                                TerrainChanged changed,
                                std::shared_ptr<const wizengine::terrain::TerrainScatter> scatter) {
    // ストローク中に飛ばした張り直し（Clear はストロークを捨てるので先に読む）。
    const bool collisionPending = terrainStroke_.collisionStale;
    const bool scatterBodyPending = terrainStroke_.scatterBodyStale;
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
    // 一部だけの書き換えなら、描画は触ったところだけ: 形（Shape）は触ったチャンク、
    // 重み（Paint）はそのセルのスプラット。changedCells が空 = 全部作り直す。
    const bool partial = fieldChanged && on && !changedCells.empty();
    const bool shapeChanged = terrainChangeHas(changed, TerrainChanged::Shape);
    const bool paintChanged = terrainChangeHas(changed, TerrainChanged::Paint);
    const wizengine::terrain::GridRect chunks =
        (partial && shapeChanged) ? field->chunksTouched(changedCells, kTerrainChunkQuads)
                                  : wizengine::terrain::GridRect{};
    // テレインのフレーム（置き場所 + 回転）。格子は原点基準なので、動かす・回すは
    // ここだけ（格子・メッシュは作り直さない）。
    const BodyTransform pose = placementPose(desc.position, desc.rotation);
    bool poseChanged = false;
    const bool renamed = desc.data != terrain_.data;
    // 塗り分けの層と散布物の規則（ここだけ = nextFileOwned）: 渡されればそれ。渡されず、
    // 格子が無くなる（外す・全消し・Missing）か別のテレイン（名前か背番号が変わる =
    // 新規・旧形式の生成）になるなら既定へ戻す（どちらも .terrain が持つので前の
    // テレインのものを残さない）。それ以外（同じテレインの形・表面・位置・格子の
    // 書き換え）は今のまま。
    static const wizengine::terrain::TerrainLayers kDefaultLayers{};
    const bool sameFile = on && !renamed && desc.guid == terrain_.guid;
    const wizengine::terrain::TerrainLayers* nextLayers =
        nextFileOwned(layers, sameFile, &kDefaultLayers);
    // 散布物は中身が今と同じなら差し替えない = ポインタで変化を見る側（描画・当たり
    // 判定）に作り直させない。比べるのはロックの外（terrainScatter_ を書くのはこの
    // スレッドだけ = 取り出した物は差し替えまで今のもの）。
    std::shared_ptr<const wizengine::terrain::TerrainScatter> nextScatter =
        nextFileOwned(std::move(scatter), sameFile, defaultTerrainScatter());
    if (nextScatter && *nextScatter == *terrainScatter()) nextScatter.reset();
    const bool scatterChanged = nextScatter != nullptr;  // 下で move する
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
                if (!partial) {
                    terrainMeshDirty_ = true;  // 全体（スプラットも）。無しなら描画側が外す
                } else {
                    if (shapeChanged) terrainChunkDirty_ = terrainChunkDirty_.united(chunks);
                    if (paintChanged)
                        terrainSplatDirty_ = terrainSplatDirty_.united(changedCells);
                }
            }
            if (!samePose(pose, terrainPose_)) {
                terrainPose_ = pose;
                terrainPoseDirty_ = true;
                poseChanged = true;
            }
            if (nextScatter) terrainScatter_ = std::move(nextScatter);
        }
        if (surfaceChanged) terrainSurfaceDirty_ = true;
        if (nextLayers && *nextLayers != terrainLayers_) {
            terrainLayers_ = *nextLayers;
            terrainLayersDirty_ = true;
        }
        // ファイルとの関係（外したら Saved）。/scene が「有無と Missing」の食い違った
        // 組を見ないよう、hasTerrain_ と同じロックの中で書く。
        settleTerrainFileState(file, on, missing);
    }
    // 名前が変わったら書き出し先も変わる（ストロークごとにディスクを見ない）。
    if (renamed) refreshHeightmapFiles();
    if (history == TerrainHistory::Clear) terrainUndo_->clear();
    // 外したテレインを選んだままにしない（Inspector に欄だけ残る）。
    if (!hasTerrain_ && editor_.selKind() == EditorState::SelKind::Terrain) {
        editor_.clearSel();
    }

    // 接触の側のフレーム（作り直しの rebuildTerrainBody も当て直す）と幹のボディ。
    if (poseChanged) {
        physics_.setTerrainFrame(chronoPos(pose), toChrono(pose));
        placeScatterBody(pose);
    }

    // テレインの有無は系の種類を変えうる（kTerrainForcesCore）。作り直したら
    // テレインの当たり判定と幹もそこで組み直し済みなので、二重に張らない。
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
    // 重みだけの書き換え（Paint）は当たり判定に関わらないので張り直さない。
    const bool collisionChanged = fieldChanged && (!partial || shapeChanged);
    if (collisionChanged || retryFailed || collisionPending) {
        if (terrainStroke_.active) terrainStroke_.collisionStale = true;
        else rebuildTerrainBody();
    }
    // 幹（テレイン本体の後 = 接触の報告を今の番号に寄せる）。組み直すのは、テレインが
    // 無い・格子の全体の差し替え・散布物の差し替え・ストローク中に飛ばしたぶんと、一部の
    // 書き換えなら形（Shape）が幹の足元のセルに掛かったときだけ（幹の足元の高さと穴の
    // 有無が変わる）。筆のストローク中は印だけ立てる（endTerrainStroke が 1 回組む）。
    const bool trunkStale = !on || (fieldChanged && !partial) || scatterChanged ||
                            scatterBodyPending ||
                            (partial && shapeChanged && terrainShapeTouchesTrunks(changedCells));
    if (!trunkStale) return;
    if (terrainStroke_.active) terrainStroke_.scatterBodyStale = true;
    else rebuildScatterBody();
}

std::string Scene::reshapeTerrain(const ed::TerrainDesc& next, bool keepShape,
                                  const std::string& status) {
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
        if (!status.empty()) editor_.setStatus(status);
        publishTerrainField(next, field, TerrainHistory::Keep, TerrainFileState::Keep);
        return std::string();
    }
    // 形の変更は今の格子のコピーから作る（共有中の格子は書き換えない）。
    // 大きさ → 再サンプル → 比例伸縮（形を保つなら切り詰め）の順。
    const auto t0 = std::chrono::steady_clock::now();
    wizengine::terrain::HeightField grid =
        sized ? field->withGeometry(float(next.size), 0.0f, 0.0f, 0.0f) : *field;
    if (resampled) grid = grid.resampled(next.resolution);
    // 高さは [0, height] に保つ（.terrain の量子化の範囲）。height は
    // clampTerrain が kTerrainMinHeight 以上に保つので、倍率は有限。
    // 形を保つときは格子の高さを変えず、新しい上限を超える所だけ切る
    // （上げたときは何も変わらない）。どちらも .terrain の形の変更 = Undo は消える。
    std::size_t clipped = 0;
    if (scaled && keepShape) clipped = grid.clampHeights(float(next.height));
    else if (scaled) grid.scaleHeights(float(next.height / now.height));
    // 形の変更の知らせを status の後ろに足す（ステータスは publish の前）:
    // 再サンプルしたなら HeightField::resampled の性質を一言、切った点があればその数
    // （全部の点が上限より上だった = 全体が上限の高さの板になった、は別の文で）。
    std::string note;
    if (resampled) note = "（解像度を下げると細い起伏は消え、穴は少し太ります）";
    if (clipped > 0 && clipped == grid.samples().size()) {
        note += "（形を保つ: すべての点が新しい上限より上だったため、全体が上限の高さの"
                "平らな面になりました）";
    } else if (clipped > 0) {
        note += "（形を保つ: " + std::to_string(clipped) + " 点が新しい上限で切られました）";
    }
    if (!status.empty() || !note.empty()) editor_.setStatus(status + note);
    LOGI("scene", "terrain reshaped: %dx%d cells, %.0f m, %.0f m high (%s) in %.1f ms",
         next.resolution, next.resolution, next.size, next.height,
         !scaled ? "height kept" : keepShape ? "clamped" : "scaled", elapsedMs(t0));
    publishTerrainField(
        next, std::make_shared<const wizengine::terrain::HeightField>(std::move(grid)),
        fileChanged ? TerrainHistory::Clear : TerrainHistory::Keep,
        fileChanged ? TerrainFileState::Dirty : TerrainFileState::Keep);
    return note;
}

bool Scene::editTerrainField(
    const wizengine::terrain::GridRect& rect,
    const std::function<bool(wizengine::terrain::HeightField&)>& mutate,
    TerrainRecord record, TerrainChanged changed,
    std::shared_ptr<const wizengine::terrain::TerrainScatter> scatter) {
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
                        record == TerrainRecord::Stroke, nullptr, changed, std::move(scatter));
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
        TerrainRecord::Undo, TerrainChanged::Shape);
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
    // 1 段は格子（rect が空でない）と散布物の配置（hasScatter）の片方か両方を
    // 持つ（散布 → 彫るのストロークは両方）。持っている方を 1 回の publish で戻す
    // （格子を戻す editTerrainField に配置も渡す）。戻す操作そのものは記録しない
    // （Redo は作らない）。applyEdit が空を返す = 解像度が違う格子など、当てられない
    // 記録（取り出したまま捨てる）。格子が無ければ何も戻さない（配置も作らない =
    // 下の配置だけの publish でテレインを外さない）。
    const std::shared_ptr<const wizengine::terrain::HeightField> field = terrainField();
    std::shared_ptr<const wizengine::terrain::TerrainScatter> scatter =
        field ? undoneScatter(edit) : nullptr;
    const bool hasGrid = !edit.rect.empty();
    bool gridOk = false;
    if (field && hasGrid) {
        gridOk = editTerrainField(
            edit.rect,
            [&edit](wizengine::terrain::HeightField& f) {
                return !wizengine::terrain::applyEdit(f, edit).empty();
            },
            // 記録は高さ・穴・重みを組で戻すので、描画も当たり判定も両方。
            TerrainRecord::None, TerrainChanged::Both, scatter);
        if (!gridOk) {
            LOGW("scene", "terrain: an undo record (resolution %d) does not fit the current grid "
                          "- discarded", edit.resolution);
        }
    }
    // 格子を戻せなかった（か格子を持たない）記録の配置は、配置だけ差し替える。
    const bool scatterOnly = scatter && !gridOk;
    if (scatterOnly) publishTerrainScatter(field, std::move(scatter), false);
    // ステータスは結果が分かってから 1 回だけ（格子が無いときもここ）。
    if (!gridOk && !scatterOnly) {
        editor_.setStatus("テレインの編集を取り消せませんでした");
        return;
    }
    // 格子と配置の両方を持つ記録で格子だけ戻せなかった = 一部だけ。
    if (hasGrid && !gridOk) {
        editor_.setStatus("テレインの編集を一部だけ取り消しました（コンソール参照）");
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
        physics_.setTerrainFrame(chronoPos(pose), toChrono(pose));
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
    physics_.setAlias(scatterPhysId_, terrainPhysId_);  // 幹の接触はテレインの番号で報告
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

std::shared_ptr<const wizengine::terrain::TerrainScatter> Scene::terrainScatter() const {
    std::lock_guard<std::mutex> fl(terrainFieldMutex_);
    return terrainScatter_;
}

std::shared_ptr<const wizengine::terrain::HeightField> Scene::copyTerrainFieldAndPose(
    BodyTransform& pose,
    std::shared_ptr<const wizengine::terrain::TerrainScatter>* scatter) const {
    std::lock_guard<std::mutex> fl(terrainFieldMutex_);
    pose = terrainPose_;
    if (scatter) *scatter = terrainScatter_;
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

std::shared_ptr<const wizengine::terrain::HeightField> Scene::terrainFieldAndScatter(
    BodyTransform& pose,
    std::shared_ptr<const wizengine::terrain::TerrainScatter>& scatter) const {
    return copyTerrainFieldAndPose(pose, &scatter);
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
