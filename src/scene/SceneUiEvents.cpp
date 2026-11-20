// UI のノード（OnUiClick / SetUiProp）の実行。すべて PHYSICS スレッド（runEventGraph の中）。
// グラフの走り方（付け先・ワイヤー・発火バッジ）は SceneEvents.cpp、ここは UI の種類の中身だけ。
#include "scene/Scene.h"

#include <string>
#include <vector>

#include "core/Log.h"
#include "document/SceneDocument.h"  // uiNodeFields
#include "ui/UiProps.h"
#include "ui/UiTypes.h"

namespace ed = wizengine::editor;
namespace ui = wizengine::ui;
using UiState = wizengine::UiState;

void Scene::collectUiClicks() {
    // 確かめ（世代・参照・カメラ・受け付けるか）は UiState::takeClicks が唯一の口。
    graphRt_.ui.clicks = ui_.takeClicks(editor_.modeEpoch());
}

void Scene::collectUiTriggers(const ed::NodeDesc& node, const GraphContext& base,
                              std::vector<GraphContext>& fires) {
    if (node.kind != ed::NodeKind::OnUiClick) return;
    // 受け付ける Canvas（-1 = 何でも。規則は editor::uiTriggerCanvas = 「未設定」の印と同じ）。
    const int onlyCanvas = ed::uiTriggerCanvas(node);
    for (const UiState::Click& c : graphRt_.ui.clicks) {
        // element = 要素の名前（空 = 何でも。名前の規則は ui::sameName = 空の名前はどれとも
        // 同じでない）。
        if (onlyCanvas >= 0 && c.canvas != onlyCanvas) continue;
        if (!node.element.empty() && !ui::sameName(node.element, c.name)) continue;
        GraphContext ctx = base;
        ctx.camera = c.camera;
        ctx.uiCanvas = c.canvas;
        ctx.uiElement = c.element;
        fires.push_back(ctx);
    }
}

void Scene::compileUiNodes() {
    // graphRt_.assets のノードを指すので、assets を取り直したら必ず呼び直す（runEventGraph が
    // rebuildGraphInstances と一緒に呼ぶ）。規則は読込の警告と同じ ui::nodePropValue。
    graphRt_.ui.compiled.clear();
    for (const auto& asset : graphRt_.assets) {
        for (const auto& n : asset.nodes) {
            if (n.kind != ed::NodeKind::SetUiProp) continue;
            graphRt_.ui.compiled[&n] = ui::nodePropValue(n.prop, ed::uiNodeFields(n));
        }
    }
}

void Scene::runUiAction(const ed::NodeDesc& node, const GraphContext& ctx) {
    if (node.kind != ed::NodeKind::SetUiProp) return;
    const auto it = graphRt_.ui.compiled.find(&node);
    if (it == graphRt_.ui.compiled.end()) return;  // 取り直しの前（次のステップで解く）
    const ui::NodePropValue& got = it->second;
    // 1 回だけ警告する（同じ文は開始ごとに 1 回 = graphRt_.ui.warned）。文を組むのは警告する
    // ときだけ。
    auto warnOnce = [this, &node](const std::string& why) {
        const std::string text = "setUiProp node " + std::to_string(node.id) + " (prop '" +
                                 node.prop + "', element '" + node.element + "'): " + why +
                                 " - the node does nothing";
        if (graphRt_.ui.warned.insert(text).second) LOGW("scene", "%s", text.c_str());
    };
    // 相手（規則は editor::uiPropTarget = ノードエディタの「未設定」の印と同じ）。決まらなければ
    // 未設定（何もしない。性質を選んでいない場合も）。
    int canvas = -1, elementId = -1;
    ed::UiNodeContext uiCtx;
    uiCtx.canvas = ctx.uiCanvas;
    uiCtx.element = ctx.uiElement;
    if (ed::uiPropTarget(node, uiCtx, canvas, elementId) != ed::UiNodeUnset::None) return;
    if (got.problem != ui::NodePropValue::Problem::None) {
        warnOnce(got.message);
        return;
    }
    switch (ui_.setNodeProp(canvas, node.element, elementId, got.prop->id, got.value)) {
        case UiState::NodePropResult::NoElement:
            warnOnce("canvas " + std::to_string(canvas) + " has no such element");
            return;
        case UiState::NodePropResult::KindLacksProp:
            warnOnce("the element does not have this property");
            return;
        case UiState::NodePropResult::Ok:
        case UiState::NodePropResult::Unchanged:
        case UiState::NodePropResult::NoCanvas:  // 消えた直後（掃除の前の行き違い）
            return;
    }
}
