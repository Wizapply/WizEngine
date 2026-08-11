#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

#include "PhysicsTuning.h"
#include "SceneComponent.h"

// ブラウザのエディタ操作を受け取る係。
//
//   mode              … エディタ / シミュレートの切り替え
//   edit.add          … ボックス・球を置く
//   edit.remove       … 消す（省略時はそのカメラの選択）
//   edit.duplicate    … 複製
//   edit.set          … 位置・回転・大きさ・質量・固定・色・名前の変更
//   edit.joint.add    … ジョイントを作る
//   edit.joint.remove … ジョイントを消す
//   edit.sim          … 重力・摩擦・レートなどシミュレート設定
//   edit.save/load/clear … assets/scenes/*.json への保存・読み込み・全消し
//
// ここでやるのは「値の正規化と検証」だけで、実体をいじる作業は EditorState の
// キューに積んで物理スレッドに任せる（Chrono を触ってよいのはそのスレッド
// だけ、という既存の約束をエディタでも崩さないため）。
//
// 例外は物理レート系（Hz・サブステップ・反復回数・エンベロープ・リカバリ）で、
// これは main が持つ PhysicsTuning の atomic に直接書く。PhysicsControlComponent
// と同じ経路なので、System タブとエディタタブのどちらから変えても同じに効く。
class EditorComponent : public SceneComponent {
public:
    explicit EditorComponent(PhysicsTuning& tuning) : tuning_(tuning) {}

    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override;

private:
    PhysicsTuning& tuning_;
};
