#pragma once

#include <string>
#include <vector>

#include "formula/Formula.h"

// 計算式の雛形（ブラウザの「新しい計算式」の選択肢）。車両などの特定の用途に
// 依らない汎用のもので、中身は**数式のテキスト**（formula/FormulaText.h）で
// 持つ = 読めばそのまま式の説明になり、起こしたグラフは変換器が検査する。
// 雛形から作った計算式はそのまま制御器として付ける（シミュレート中に回って
// スコープに出る）。UI は id を送るだけで、中身はここにしか無い。
namespace wizengine {
namespace control {

struct FormulaTemplate {
    std::string id;
    std::string label;  // 選択肢の表示（日本語）
    std::string hint;   // 一言の説明
    // 雛形が指す相手: "" = 無し、"joint" = ジョイントの名前、"body" = オブジェクトの名前
    // （テキストの {joint} / {body} を置き換える）。
    std::string needs;
};
const std::vector<FormulaTemplate>& formulaTemplates();

// 雛形の数式テキスト（{joint} / {body} を target で置き換えたもの）。
// 知らない id は空。
std::string templateText(const std::string& id, const std::string& target);

// 雛形からグラフを起こす。知らない id・起こせない（雛形の書き損じ）なら false。
bool templateGraph(const std::string& id, const std::string& name, const std::string& target,
                   wizengine::formula::FormulaGraphDesc& out, std::string& error);

}  // namespace control
}  // namespace wizengine
