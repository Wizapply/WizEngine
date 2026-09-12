#pragma once

#include "document/EditorTypes.h"

// 組み込みの「クルマらしい見た目」をプレハブとして組む。
//
// 箱の車体（BodyDesc::size）に対する比率でキャビン・傾いたフロントガラス・
// ヘッドライト・テールライトを置き、車両の各輪にはタイヤ（円柱）とスポーク
// （板 2 枚）を socket 付きで置く。プレハブを付けていない車両はこれを
// **暗黙のプレハブ**として描く（PrefabComponent）。「プレハブを編集」で
// これを実体のアセットにしてオブジェクトへ付けると、部品を個別に動かせる。
namespace wizengine {
namespace editor {

PrefabDesc builtinCarPrefab(const BodyDesc& body, const std::string& name = "");

// 階段。全段を collide 付きの箱の部品として並べたプレハブで、原点は
// **全体が収まる箱の中心**（幅 width、高さ rise*steps、奥行き run*steps。
// ローカル +Z へ上る）。付け先は geom を持たないボディ（ShapeKind::None、
// 中心を床から rise*steps/2 の高さに置く）。各段は床から段の高さまでの中実の
// 箱 = 横から見ても隙間が無い。専用のオブジェクトは持たない: 置いたあとは
// 普通のプレハブとして段を動かせる（アセットパネルの 🪜 Stairs）。
PrefabDesc builtinStairsPrefab(const std::string& name, int steps, double rise, double run,
                               double width, const Color3& color);

}  // namespace editor
}  // namespace wizengine
