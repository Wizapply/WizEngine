#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "scene/UiState.h"
#include "ui/UiTypes.h"

// ブラウザ API（/scene の "ui"）の UI の部分。UiState の写しから作るだけで、設計値は
// 持たない（直すきっかけは「ブラウザに送る UI の形を変える」だけ）。
//
// 性質ごと・属性ごとのキーはここに書き並べない: 値は性質の表（ui::toJson）と Canvas の
// 属性の表（kCanvasAttrs）を走って作り、Inspector の欄の形は schema（同じ表から作る）で
// ブラウザへ渡す = 性質・属性・種類を足しても、ここと Inspector の変更は 0。
// 一覧（canvases の要素の行・assets）は要約なので、決まったキー（id・kind・name・label
// など）を書き並べる（性質の値は送らない）。
// 名前の無い物の表示名（label）もここで入れる（呼び方の唯一の定義は ui/UiTypes.h の
// canvasLabel / elementLabel。ブラウザは受けた label を出すだけ）。
// 色は性質の表のとおり [r,g,b,a] のリニア値（ほかのブラウザ API の "#rrggbb" とは違う。
// 変換はブラウザの ui-inspector.js）。
//
// Chrono / Filament を知らない（tests/document で単体で検査する）。
namespace wizengine {
namespace uijson {

// Inspector の欄の形: 性質の表（props。既定が要素ごとに決まる性質の def は null）・種類の表
// （kinds = 種類ごとの一覧のアイコンと持つ性質の名前）・Canvas の属性の表（canvas）。表は実行中に
// 変わらないので 1 回だけ組む（どのスレッドから呼んでもよい）。
const nlohmann::json& schema();

// Canvas 1 枚の属性（{属性の名前: 値}。kCanvasAttrs の全行）。
nlohmann::json canvasAttrsJson(const ui::CanvasDesc& desc);

// /scene の "ui": canvases（生きている Canvas と要素の木 = 階層一覧。editable = 要素を
// 編集してよいか）・assets（宣言した UI = アセットパネルのタイル）・selected（選択中の
// Canvas か要素の設計値。Canvas なら中身の XML（xml）も。無ければ null）・schema。
// selCanvas / selElement はエディタの選択をそのまま渡す（Canvas を選んでいなければ
// selCanvas = -1。selElement = -1 は Canvas そのもの）。選択が今もあるかはここで 1 回だけ
// 確かめ（Snapshot::selectionExists）、消えた物を指していれば selected は null（呼び手は
// これで選択の有無を知る）。
nlohmann::json toJson(const UiState::Snapshot& snap, int selCanvas, int selElement);

// /scene のポーリングごとに組み直さないための写し: UiState の版と選択が前回と同じなら、
// 前に組んだ結果をそのまま返す（全体の写しも、選択中の Canvas の XML も、文字列への書き出しも
// やり直さない）。返すのは共有の読み取り専用の結果（ポーリングのたびに丸ごと写さない）。
// 写しは変わった物だけを写し直す（UiState::copyIfChanged）。どのスレッドから呼んでもよい
// （中のロックを取ってから UiState の末端のロックを取る）。
class Cache {
public:
    struct Entry {
        std::string text;       // toJson の結果を書き出した文字列（/scene へそのまま埋める）
        bool selected = false;  // 選択が今もある（json の selected が null でない）
    };
    std::shared_ptr<const Entry> get(const UiState& state, int selCanvas, int selElement);

private:
    std::mutex mutex_;
    std::uint64_t known_ = 0;
    UiState::Snapshot snap_;
    int canvas_ = -1, element_ = -1;
    std::shared_ptr<const Entry> entry_;
};

}  // namespace uijson
}  // namespace wizengine
