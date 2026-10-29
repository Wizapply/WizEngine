// テレインの塗り分けの道具（PHYSICS スレッド）: 自動塗り（edit.terrain の
// autopaint）の規則の JSON と、規則 → 重みの書き直し（editTerrainField）。
// 重みの計算そのものは terrain/TerrainAutoPaint、格子の差し替え（コピー
// オンライト・Undo・dirty）は SceneTerrain.cpp の editTerrainField に任せる。
// 彫る道具（筆のストローク = SceneTerrainSculpt.cpp）とは直すきっかけが別。
#include "scene/SceneInternal.h"

using namespace scene_detail;

namespace wt = wizengine::terrain;

namespace scene_detail {

nlohmann::json autoPaintRulesToJson(const wt::AutoPaintRules& rules) {
    nlohmann::json layers = nlohmann::json::array();
    for (const wt::AutoPaintBand& b : rules.layer) {
        layers.push_back(nlohmann::json{{"enabled", b.enabled},
                                        {"slopeMin", b.slopeMinDeg},
                                        {"slopeMax", b.slopeMaxDeg},
                                        {"heightMin", b.heightMin},
                                        {"heightMax", b.heightMax}});
    }
    return nlohmann::json{{"softness", rules.softness}, {"layers", std::move(layers)}};
}

wt::AutoPaintRules autoPaintRulesFromJson(const nlohmann::json& j,
                                          const wt::AutoPaintRules& base) {
    wt::AutoPaintRules r = base;
    r.softness = ed::jsonNumber(j, "softness", r.softness);
    const auto it = j.is_object() ? j.find("layers") : j.end();
    if (it != j.end() && it->is_array()) {
        const std::size_t n = std::min(it->size(), r.layer.size());
        for (std::size_t k = 0; k < n; ++k) {
            const nlohmann::json& e = (*it)[k];
            wt::AutoPaintBand& b = r.layer[k];
            if (const auto en = e.is_object() ? e.find("enabled") : e.end();
                en != e.end() && en->is_boolean())
                b.enabled = en->get<bool>();
            b.slopeMinDeg = ed::jsonNumber(e, "slopeMin", b.slopeMinDeg);
            b.slopeMaxDeg = ed::jsonNumber(e, "slopeMax", b.slopeMaxDeg);
            b.heightMin = ed::jsonNumber(e, "heightMin", b.heightMin);
            b.heightMax = ed::jsonNumber(e, "heightMax", b.heightMax);
        }
    }
    return wt::clampAutoPaintRules(r);
}

}  // namespace scene_detail

void Scene::autoPaintTerrain(const nlohmann::json& rulesJson, const std::string& notes) {
    const std::shared_ptr<const wt::HeightField> field = terrainField();
    if (!field) {
        editor_.setStatus("テレインがありません（塗り分けできません）");
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const wt::AutoPaintRules rules =
        autoPaintRulesFromJson(rulesJson, wt::defaultAutoPaintRules());
    const int res = field->resolution();
    // 全セルを書き直す（取り消しは 1 段 = 高さ・穴・重みを組で記録）。重みが 1 つも
    // 変わらなければ editTerrainField は記録も差し替えもしない。
    const bool changed = editTerrainField(
        wt::GridRect{0, 0, res, res},
        [&rules](wt::HeightField& f) { return wt::applyAutoPaint(f, rules); },
        TerrainRecord::Undo, TerrainChanged::Paint);
    // ステータスは結果が分かってから 1 回だけ。
    if (!changed) {
        editor_.setStatus("塗り分けは変わりませんでした（今の重みが同じ規則のとおりです）" + notes);
        return;
    }
    editor_.setStatus("テレインを傾きと高さで塗り分けました（取り消しできます）" + notes);
    LOGI("scene", "terrain auto-painted by slope and height (softness %.2f): %dx%d cells "
                  "in %.1f ms",
         rules.softness, res, res, elapsedMs(t0));
}
