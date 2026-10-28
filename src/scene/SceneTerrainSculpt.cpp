// テレインの筆のストローク（PHYSICS スレッド）: 開始・1 パスぶんのスタンプの
// 適用・終了（Undo 1 段 + 当たり判定の張り直し 1 回）。
// 格子の書き換えは editTerrainField（コピーオンライト）の 1 か所を通り、
// ストロークの「変更前」もそこで育てる（TerrainRecord::Stroke）。入力（レイ →
// ローカルのスタンプ）は呼び手が組む。
#include "scene/SceneInternal.h"

#include "terrain/TerrainBrush.h"

#include <chrono>

using namespace scene_detail;

void Scene::beginTerrainStroke() {
    if (terrainStroke_.active) endTerrainStroke();
    // シミュレート中は塗らない（当たり判定の張り直しを end まで止めると、車輪の
    // レイ = 新しい格子と接触 = 古い格子が食い違う）。モードを見るのはここだけ:
    // ストロークはエディタでしか始まらず、enterMode が切り替える前に終える。
    // 格子の無いテレイン（Missing）・テレイン無しは編集できない（terrainLoaded は
    // 格子があるときだけ true）。
    if (!editor_.isEditor()) return;
    if (!terrainLoaded()) return;
    terrainStroke_ = TerrainStroke{};
    terrainStroke_.active = true;
}

void Scene::stampTerrain(std::vector<wizengine::terrain::BrushStamp>& stamps) {
    if (!terrainStroke_.active || stamps.empty()) return;
    const std::shared_ptr<const wizengine::terrain::HeightField> field = terrainField();
    if (!field) return;
    const auto t0 = std::chrono::steady_clock::now();
    // このパスで書き換えうる範囲（全スタンプの和）。格子のコピーは 1 パスに 1 回。
    // 高さの上限は今のテレインの値をここで入れる（呼び手に渡させない = 渡し忘れが無い。
    // terrain_ を書くのはこのスレッドだけなのでロック不要）。
    wizengine::terrain::GridRect rect;
    for (wizengine::terrain::BrushStamp& s : stamps) {
        s.maxHeight = terrain_.height;
        rect = rect.united(wizengine::terrain::stampRect(*field, s));
    }
    if (rect.empty()) return;  // 格子の外・壊れた値
    const bool changed = editTerrainField(
        rect,
        [&stamps](wizengine::terrain::HeightField& f) {
            bool any = false;
            for (const wizengine::terrain::BrushStamp& s : stamps) {
                // 全部当てる（|| で途中を飛ばさない）。
                if (wizengine::terrain::applyStamp(f, s)) any = true;
            }
            return any;
        },
        TerrainRecord::Stroke, TerrainChanged::Shape);
    // 計測は格子を実際に書き換えたパスだけ（回数・スタンプ数・時間を揃える）。
    if (!changed) return;
    ++terrainStroke_.passes;
    terrainStroke_.stamps += int(stamps.size());
    terrainStroke_.passMs += elapsedMs(t0);
}

void Scene::endTerrainStroke() {
    if (!terrainStroke_.active) return;
    // 先に閉じる（下の張り直しがストローク中と見なされないように）。
    TerrainStroke done = std::move(terrainStroke_);
    terrainStroke_ = TerrainStroke{};
    const wizengine::terrain::GridRect cells = done.cells;
    terrainUndo_->push(std::move(done.before));  // 空（何も変わらなかった）は積まれない
    // ストローク中に飛ばした張り直しがあれば張る（系の作り直しで張られていても、
    // 印が立っていればもう一度張る = 安全側）。
    char collision[48] = "";
    if (done.collisionStale) {
        const auto t0 = std::chrono::steady_clock::now();
        rebuildTerrainBody();
        std::snprintf(collision, sizeof(collision), ", collision %.1f ms", elapsedMs(t0));
    }
    // ログは格子を書き換えたストロークだけ（1 パスの時間を見るため）。
    if (cells.empty()) return;
    LOGI("scene", "terrain stroke: %d passes, %d stamps, cells %dx%d at (%d, %d), "
                  "avg pass %.2f ms%s",
         done.passes, done.stamps, cells.i1 - cells.i0, cells.j1 - cells.j0, cells.i0, cells.j0,
         done.passes > 0 ? done.passMs / done.passes : 0.0, collision);
}

void Scene::abortTerrainStroke() { terrainStroke_ = TerrainStroke{}; }
