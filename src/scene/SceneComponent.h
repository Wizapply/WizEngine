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
