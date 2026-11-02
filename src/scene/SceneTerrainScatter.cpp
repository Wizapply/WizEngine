// テレインの散布物（木や小物）のサーバー側の操作（PHYSICS スレッド）: 原型の編集
// （edit.terrain の scatter）、散布の筆のスタンプ（🌲 / ⌫）、取り消しの書き戻し。
// 値の検査と JSON は SceneInternal.h（sanitizeScatter / scatterToJson /
// prototypePatchFromJson）、.terrain との読み書きは SceneTerrainFile.cpp、散布物の
// 差し替えそのもの（ロック・ファイルの dirty）は SceneTerrain.cpp の
// publishTerrainField に任せる。配置の計算は terrain の scatterStamp。
#include "scene/SceneInternal.h"

#include "terrain/TerrainBrush.h"
#include "terrain/TerrainUndo.h"

#include <algorithm>
#include <chrono>

using namespace scene_detail;

namespace wt = wizengine::terrain;

// ---- 原型 ---------------------------------------------------------------------

void Scene::editTerrainPrototype(int index, const nlohmann::json& patch) {
    const std::shared_ptr<const wt::HeightField> field = terrainField();
    if (!field) {
        editor_.setStatus("テレインがありません（原型を変えられません）");
        return;
    }
    const std::size_t i = std::size_t(index);
    const std::string label = terrainPrototypeLabel(i);
    const std::shared_ptr<const wt::TerrainScatter> now = terrainScatter();
    wt::TerrainScatter next = *now;
    next.prototypes[i] = prototypePatchFromJson(patch, next.prototypes[i]);
    // 検査で消されたパスは適用しない（黙って代役にしない）。
    const std::string asked = next.prototypes[i].file;
    sanitizeScatter(next, "browser");
    if (next.prototypes[i].file != asked) {
        editor_.setStatus(label + " のモデルは " + terrainPathRuleText() + "で指定してください");
        return;
    }
    if (next == *now) {
        editor_.setStatus(label + " は変わっていません");
        return;
    }
    // 原型は Undo の対象にしない（筆のストロークも閉じない）。ステータスは publish の前。
    editor_.setStatus(label + " を更新（保存すると .terrain に書きます）");
    publishTerrainScatter(field, std::move(next), false);
}

void Scene::publishTerrainScatter(const std::shared_ptr<const wt::HeightField>& field,
                                  wt::TerrainScatter&& next, bool brushPass) {
    publishTerrainField(terrain_, field, TerrainHistory::Keep, TerrainFileState::Dirty, {},
                        brushPass, nullptr, TerrainChanged::Both,
                        std::make_shared<const wt::TerrainScatter>(std::move(next)));
}

// ---- 散布の筆と取り消し -----------------------------------------------------------

void Scene::stampScatter(const std::vector<wt::BrushStamp>& stamps) {
    if (!terrainStroke_.active) return;
    const std::shared_ptr<const wt::HeightField> field = terrainField();
    if (!field) return;
    // 散布の道具のスタンプが無いパス（彫る・塗るだけ）は配置を写さない。
    const bool any = std::any_of(stamps.begin(), stamps.end(), [](const wt::BrushStamp& s) {
        return wt::brushScatters(s.tool);
    });
    if (!any) return;
    const std::shared_ptr<const wt::TerrainScatter> now = terrainScatter();
    wt::TerrainScatter next = *now;  // 配置のコピーは 1 パスに 1 回
    bool changed = false;
    bool capped = false;
    for (const wt::BrushStamp& s : stamps) {
        if (!wt::brushScatters(s.tool)) continue;
        const auto t0 = std::chrono::steady_clock::now();
        wt::ScatterStampStats stats;
        // 全部当てる（|| で途中を飛ばさない）。
        if (wt::scatterStamp(*field, s, next, &stats)) changed = true;
        const double ms = elapsedMs(t0);
        // 計測（endTerrainStroke が 1 行のログにする）。
        ++terrainStroke_.scatterStamps;
        terrainStroke_.scatterAdded += stats.added;
        terrainStroke_.scatterRemoved += stats.removed;
        terrainStroke_.scatterMs += ms;
        terrainStroke_.scatterMaxMs = std::max(terrainStroke_.scatterMaxMs, ms);
        if (stats.capped) capped = true;
    }
    // 上限の知らせはストロークに 1 回だけ（押し続けるたびに出さない）。
    if (capped && !terrainStroke_.scatterCapped) {
        terrainStroke_.scatterCapped = true;
        editor_.setStatus("散布物は 1 つのテレインに " +
                          std::to_string(wt::kMaxScatterInstances) +
                          " 個までです（それ以上は置けません）");
    }
    if (!changed) return;
    // ストロークの「変更前」の配置はストロークの最初の 1 回だけ写す（growScatter）。
    wt::growScatter(terrainStroke_.before, *now);
    // 格子は同じ = メッシュも当たり判定も作り直さない（筆の 1 パス）。
    publishTerrainScatter(field, std::move(next), true);
}

bool Scene::restoreScatter(const wt::TerrainEdit& edit) {
    if (!edit.hasScatter) return false;
    const std::shared_ptr<const wt::HeightField> field = terrainField();
    if (!field) return false;
    wt::TerrainScatter next = *terrainScatter();
    // 戻す操作そのものは記録しない（Redo は作らない = 逆操作は捨てる）。
    wt::applyScatterEdit(next, edit);
    publishTerrainScatter(field, std::move(next), false);
    return true;
}
