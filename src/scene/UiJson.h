#pragma once

#include <nlohmann/json.hpp>

#include "scene/UiState.h"
#include "ui/UiTypes.h"

// ブラウザ API（/scene の "ui"）の UI の部分。UiState の写しから作るだけで、状態は
// 持たない（直すきっかけは「ブラウザに送る UI の形を変える」だけ）。
//
// 性質ごと・属性ごとのキーはここに書き並べない: 値は性質の表（ui::toJson）と Canvas の
// 属性の表（kCanvasAttrs）を走って作り、Inspector の欄の形は schema（同じ表から作る）で
// ブラウザへ渡す = 性質・属性・種類を足しても、ここと Inspector の変更は 0。
// 色は性質の表のとおり [r,g,b,a] のリニア値（ほかのブラウザ API の "#rrggbb" とは違う。
// 変換はブラウザの ui-inspector.js）。
//
// Chrono / Filament を知らない（tests/document で単体で検査する）。
namespace wizengine {
namespace uijson {

// Inspector の欄の形: 性質の表（props）・種類の表（kinds = 種類ごとに持つ性質の名前）・
// Canvas の属性の表（canvas）。表は実行中に変わらないので 1 回だけ組む（どのスレッドから
// 呼んでもよい）。
const nlohmann::json& schema();

// Canvas 1 枚の属性（{属性の名前: 値}。kCanvasAttrs の全行）。
nlohmann::json canvasAttrsJson(const ui::CanvasDesc& desc);

// /scene の "ui": canvases（生きている Canvas と要素の木 = 階層一覧）・selected（選択中の
// Canvas か要素の設計値。無ければ null）・schema。selCanvas / selElement はエディタの
// 選択をそのまま渡す（Canvas を選んでいなければ selCanvas = -1。selElement = -1 は Canvas
// そのもの）。選択が今もあるかはここで 1 回だけ確かめ（Snapshot::selectionExists）、
// 消えた物を指していれば selected は null（呼び手はこれで選択の有無を知る）。
nlohmann::json toJson(const UiState::Snapshot& snap, int selCanvas, int selElement);

}  // namespace uijson
}  // namespace wizengine
