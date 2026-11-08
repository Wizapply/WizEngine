#pragma once

#include <map>
#include <string>
#include <vector>

#include "document/SceneDocument.h"

// 生成した配置の後始末（決定的な計算。LLM は呼ばない）。
//
// LLM は数値の幾何が苦手で、「机の上のコップ」が机にめり込む・箱どうしが
// 重なる・床の下に沈む、を普通にやる。Chrono はそれを「重なりを解く大きな
// 力」で押し返すので、開始した瞬間に物が弾け飛ぶ。そこで**動く物だけ**を
// 真上へ持ち上げて、下の物の上面（外接箱）に載せ直す。落ちて落ち着くので
// 物理として正しい状態から始まる。
//
// やらないこと: 固定の物（壁・建物は重ねて置くのが普通）とジョイントで
// つながった物（つなぎ方の幾何が崩れる）は動かさない。横へずらすことも
// しない（どちらへずらすのが意図に沿うかは分からない - 上に載せるのは
// 少なくとも「置いた場所」を保つ）。外接箱で見るので、斜めの物や生成
// モデルの凹みでは安全側（少し浮く）に倒れる。
namespace wizengine {
namespace autogen {

// メッシュ名 → 倍率を掛けたあとの外接箱（モデル単位 × scale、m）の
// 半寸法と中心のずれ。生成したモデルと、ファイルの読める既存のモデル。
struct MeshExtent {
    double half[3] = {0.5, 0.5, 0.5};
};
using MeshExtents = std::map<std::string, MeshExtent>;

struct LayoutReport {
    int lifted = 0;         // 他の物の上へ載せ直した数
    int raisedToFloor = 0;  // 床の上へ持ち上げた数
    std::vector<std::string> notes;  // 英語（ログ）
};

// doc.bodies のうち movable[i] が true の物を直す。obstacles は動かさない
// 既存の物（追加モードの今のシーン）。床は doc.ground（enabled かつ
// テレインが無いとき y = ground.position.y の平面）。
LayoutReport resolveOverlaps(editor::SceneDocument& doc, const std::vector<bool>& movable,
                             const std::vector<editor::BodyDesc>& obstacles,
                             const MeshExtents& meshes);

}  // namespace autogen
}  // namespace wizengine
