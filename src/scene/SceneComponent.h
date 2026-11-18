#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

class Scene;

// A scene-level behaviour with lifecycle hooks, in the spirit of a Unity
// MonoBehaviour. Derive, override the hooks you need, and register the
// instance with Scene::addComponent() - the Scene calls every component's
// hooks at the right points, so adding behaviour never means editing
// Scene::stepPhysics() or the input dispatch again.
//
// THREADING CONTRACT - the hook name says which thread calls it:
//   onPhysicsStep : PHYSICS thread, every step, before integration.
//   onEditorStep  : PHYSICS thread, every pass while in EDITOR mode.
//   onPhysicsPassEnd : PHYSICS thread, after each SIMULATE pass that stepped
//                   (all substeps + catch-up). Publish render-side state here
//                   so it lines up with the pose frames the renderer
//                   interpolates between (Scene::renderAlpha).
//   onRender      : RENDER thread, once per frame, before poses are drawn.
//   onRenderUnlocked : （宣言の説明を参照）
//   onCommand     : INPUT thread, for each browser JSON command.
// A component that keeps state shared between hooks must make that state
// thread-safe itself (atomics, like CameraObject/BoxController do).
class SceneComponent {
public:
    virtual ~SceneComponent() = default;

    virtual void onPhysicsStep(Scene& scene, double dt) {
        (void)scene;
        (void)dt;
    }

    // エディタモードのあいだ、物理を進めない代わりに毎パス呼ばれる。物理
    // スレッドから来るので PhysicsWorld を触ってよい（掴んだ物を「押す」
    // のではなく「置き直す」のがここ）。シミュレート中は呼ばれない。
    virtual void onEditorStep(Scene& scene, double dt) {
        (void)scene;
        (void)dt;
    }

    // シミュレート中、物理の 1 パス（サブステップと取り戻しの全部）を回し
    // 終えたあと。描画が補間する姿勢のフレームはここで切り替わるので、自前の
    // 描画用の値（車輪の姿勢など）を同じ区切りで「前回 / 今回」に回す。
    virtual void onPhysicsPassEnd(Scene& scene) { (void)scene; }

    virtual void onRender(Scene& scene) { (void)scene; }

    // RENDER スレッド、毎フレーム 1 回。applyToRenderer がオブジェクトのロックを
    // 外した後（描画設定・環境光を当てるのと同じ区切り）に、登録の順に呼ぶ。
    // 重い描画側の仕事（ファイルの読み込み・デコード・メッシュの組み立て）はここ。
    // Renderer は使ってよい（RENDER スレッド）。Scene のオブジェクト一覧は読まない
    // （要るものは onRender = ロックの中で写しておく。onRender は短く保つ）。
    virtual void onRenderUnlocked(Scene& scene) { (void)scene; }

    // camIndex identifies which camera's browser page sent the command.
    // Return true when the command was consumed (stops the search).
    virtual bool onCommand(Scene& scene, std::size_t camIndex,
                           const nlohmann::json& msg) {
        (void)scene;
        (void)camIndex;
        (void)msg;
        return false;
    }
};
