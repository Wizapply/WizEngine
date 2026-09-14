#pragma once

#include <string>
#include <vector>

#include "document/SceneDocument.h"

// 外部のモデル記述（URDF / OpenSim / ADAMS）をシーン文書へ取り込む
// （Chrono::Parsers。B の 21）。パーサは Chrono の系にボディ・当たり形状・
// リンクを作るので、一時的な ChSystemNSC に読ませてから歩いて SceneDocument
// （<body> / <geom> / <prefab> / <joint>）へ直す。読めた文書は
// Scene::appendDocument で今のシーンに**足す**。
//
// 変換の約束:
//   * ボディの当たり形状が箱 / 球 1 個ならそのまま geom。それ以外（複数・
//     円柱・凸包・三角メッシュ）は geom の無いフレーム + collide 部品の
//     プレハブ（凸包・三角メッシュは外接箱に落として警告）。見た目の
//     メッシュ（dae / stl）は読まない - Renderer は glTF だけを描く。
//   * リンクは hinge / slide / ball / weld / universal / cylindrical /
//     distance / spring とモータ（speed / position / force）。可動範囲
//     （ChLinkLimit）も拾う。それ以外の種類は警告して落とす。
//   * URDF は Z-up が普通なので、既定で X 軸まわりに -90° 回して Y-up に
//     置く（OpenSim は Y-up なのでそのまま。ADAMS もそのまま）。
// ビルドに Chrono::Parsers が無ければ importAvailable() が false で、
// importModel は理由を返す。
namespace wizengine {

bool importAvailable();

// file は assets/ 相対（拡張子で形式を決める: .urdf / .osim / .adm）。
// 失敗は false と error（英語）。読めた範囲の「怪しい所」は warnings。
bool importModel(const std::string& file, editor::SceneDocument& out,
                 std::string& error, std::vector<std::string>& warnings);

}  // namespace wizengine
