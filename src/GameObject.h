#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "EditorTypes.h"

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
    // 今の renderId が glTF インスタンスプールの番号か（true）、Renderer の
    // 形状スロットの番号か（false）。形を変えたときにどちらを片付ければ
    // よいかは、これを見ないと分からない。
    bool modelDraw = false;
    // 描画側に色を送り直す必要があるか（RENDER スレッドが見て落とす）。
    bool colorDirty = true;
    // 形や大きさが変わったのでレンダラブルを作り直す（同上）。
    bool renderDirty = false;
    // 形・大きさ・質量が変わったので Chrono のボディを作り直す必要がある。
    // エディタ中は物理を回していないので、シミュレート開始のときにまとめて
    // 作り直す（スライダーを動かすたびにボディを捨てないための遅延）。
    bool physDirty = false;
    // イベントグラフのアクション（SetColor）が与える実行時の色。設計値
    // （desc.color）は書き換えない: シミュレートを止めると desc へ戻る、
    // という姿勢と同じ原則で色も戻す（Scene::resetGraphRuntime が落とす）。
    bool hasRuntimeColor = false;
    wizengine::editor::Color3 runtimeColor;
    std::vector<std::unique_ptr<ObjectAction>> actions;
};
