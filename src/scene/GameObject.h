#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "document/EditorTypes.h"
#include "scene/SoftLattice.h"

class PhysicsWorld;
struct GameObject;

// A behaviour attached to ONE object, called from the PHYSICS thread every
// step before integration. This is the per-object counterpart of
// SceneComponent: use a SceneComponent for scene-wide logic, an ObjectAction
// for "this object spins" / "this object pulses" kinds of behaviour.
class ObjectAction {
public:
    virtual ~ObjectAction() = default;
    virtual void onPhysicsStep(GameObject& object, PhysicsWorld& physics,
                               double dt) = 0;
};

// One dynamic object in the scene: a physics body, its renderable, and any
// actions attached to it.
//
// エディタモードが入ったので、オブジェクトは「設計値（desc）」と「実体
// （physId / renderId）」の両方を持つ。実体は作るスレッドが違う:
//   physId   … PHYSICS スレッドが Chrono のボディを作った時点で決まる
//   renderId … RENDER スレッドが Filament のレンダラブルを作った時点で決まる
// どちらも決まるまで kInvalidId。つまり「置いた直後の 1 フレームだけ
// まだ描かれていない」状態がありうるが、生成をそれぞれのスレッドに任せる
// ことでロックなしにエンジンを触れる。
//
// alive=false は削除済み。番号は詰めない（詰めると他のオブジェクトの番号が
// ずれ、ブラウザ側の選択やジョイントの参照が別物を指してしまう）。
struct GameObject {
    static constexpr std::size_t kInvalidId = static_cast<std::size_t>(-1);

    std::size_t physId = kInvalidId;
    std::size_t renderId = kInvalidId;
    wizengine::editor::BodyDesc desc;
    bool alive = true;
    // 今の renderId が glTF モデル実体の番号か（true）、Renderer の
    // 形状スロットの番号か（false）。形を変えたときにどちらを片付ければ
    // よいかは、これを見ないと分からない。
    bool modelDraw = false;
    // desc.mesh（アセット名）を引いた結果（Scene::meshes_ の番号、-1 = 無い/
    // 見つからない）。名前引きを毎フレームやらないためのキャッシュで、
    // シーンの読込はオブジェクトを作り直すのでズレない。
    int meshIndex = -1;
    // 凸包で当たるとき、Chrono がボディの原点に据えた凸包の体積重心（モデル
    // 座標・m。それ以外は 0）。見た目（glTF の原点）はこの分だけ逆へずらして
    // 描く - でないと原点が底にあるモデルは当たり判定から浮いて見える。
    // 書くのは物理スレッド（createBody）、読むのは RENDER スレッド。
    wizengine::editor::Vec3d hullCenter{0.0, 0.0, 0.0};
    // 描画側に色と材質を送り直す必要があるか（RENDER スレッドが見て落とす）。
    bool colorDirty = true;
    // 形や大きさが変わったのでレンダラブルを作り直す（同上）。
    bool renderDirty = false;
    // 形・大きさ・質量が変わったので Chrono のボディを作り直す必要がある。
    // エディタ中は物理を回していないので、シミュレート開始のときにまとめて
    // 作り直す（スライダーを動かすたびにボディを捨てないための遅延）。
    bool physDirty = false;
    // 固定の持ち主に付いたプレハブの collide 部品を、複合形状ではなく別の
    // 固定ボディとして作ったときの番号（Scene::createBody）。持ち主と一緒に
    // 退場させ、持ち主を動かしたら physDirty で作り直す。動く持ち主の部品は
    // 複合形状（本体に足す）なのでここには入らない。
    std::vector<std::size_t> childPhysIds;
    // イベントグラフのアクション（SetColor）が与える実行時の色。設計値
    // （desc.color）は書き換えない: シミュレートを止めると desc へ戻る、
    // という姿勢と同じ原則で色も戻す（Scene::resetGraphRuntime が落とす）。
    bool hasRuntimeColor = false;
    wizengine::editor::Color3 runtimeColor;
    std::vector<std::unique_ptr<ObjectAction>> actions;
    // ソフトボディの格子（desc.hasSoft のときだけ）。physId はその「代表」
    // （最初の粒子）の番号で、PhysicsWorld はその番号への操作を粒子全体に
    // 広げる（姿勢の当てはめ・置き直し・固定・力）。描画側は latestSoft_
    // （粒子位置のスナップショット）とこの格子から表面メッシュを組む。
    // 物理スレッドが作り、objectsMutex_ の下で差し替える（不変なので
    // shared_ptr<const> で持つ）。
    std::shared_ptr<const wizengine::softlattice::Lattice> lattice;
    // ギズモの拡縮ドラッグ中に粒子を作り直し続けないための遅延（秒）。
    // resizeObject が積み、stepEditor が減らして 0 になったら作り直す。
    double softRebuildTimer = 0.0;
};
