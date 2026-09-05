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

}  // namespace editor
}  // namespace wizengine
