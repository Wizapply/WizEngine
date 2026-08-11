#pragma once

#include <cstddef>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include <math/vec3.h>

#include "BoxController.h"
#include "CameraObject.h"
#include "EditorState.h"
#include "EditorTypes.h"
#include "LightObject.h"
#include "GameObject.h"
#include "SceneComponent.h"
#include "PhysicsWorld.h"  // for BodyTransform

namespace wizengine {
class Renderer;
}

// One scene tying physics bodies to their renderables, split for threading:
//   - build()          : set up both engines (single-threaded, before threads)
//   - stepPhysics(dt)  : PHYSICS thread - advance the sim, snapshot poses
//   - stepEditor(dt)   : PHYSICS thread - editor mode: no integration, just
//                        apply edits and keep the snapshot fresh
//   - reset()          : PHYSICS thread - put everything back where it was placed
//   - applyToRenderer(): RENDER thread  - push the latest poses to renderables
// The pose snapshot is the only shared state between the two threads.
// All scene content and parameters live in scene.cpp.
//
// ---- 2つのモード ----------------------------------------------------------
// エディタモードは物理を進めない。オブジェクトを置き、大きさや質量を決め、
// ジョイントを設計する時間。シミュレートモードは今までどおり Chrono を回す。
// 切り替えは EditorState 経由で要求し、実際の遷移は物理スレッドが行う
// （PhysicsWorld を触れるのがそのスレッドだけだから）。
//
// オブジェクトの追加・削除も同じ経路を通る:
//   INPUT スレッド  … EditorState に操作を積む
//   PHYSICS スレッド … Chrono のボディを作り、boxes_ に足す
//   RENDER スレッド  … まだレンダラブルが無いオブジェクトの分を作る
// boxes_ の「構造」を触るときだけ objectsMutex_ を取る。物理スレッドは唯一の
// 書き手なので読むときは取らなくてよいが、他のスレッドは必ず lockObjects()。

// Which physics backend the scene wants. Defined in scene.cpp with the rest of
// the scene configuration; main constructs the PhysicsWorld from it.
PhysicsBackend scenePhysicsBackend();

// Streaming settings (defined in scene.cpp with the rest of the config).

class Scene {
public:
    Scene(PhysicsWorld& physics, wizengine::Renderer& renderer);

    // The scene's cameras. Each one has its own Filament view and its own
    // browser endpoint; index 0 is the default camera. Input events are routed
    // to one of these (input thread); the render loop draws each in turn.
    CameraObject& camera(std::size_t index = 0) { return *cameras_[index]; }
    std::size_t cameraCount() const { return cameras_.size(); }

    // One grab/push component per camera, so each viewer holds and drives its
    // own object without fighting over a shared selection.
    BoxController& boxController(std::size_t cameraIndex = 0) {
        return *controllers_[cameraIndex];
    }

    // The scene's lights, in lightConfigs() order (scene.cpp). Change their
    // position/direction/colour/intensity from anywhere; the render loop
    // pushes the state to the renderer once per frame.
    LightObject& light(std::size_t index = 0) { return *lights_[index]; }
    std::size_t lightCount() const { return lights_.size(); }

    // Pick the object under a screen position, given in normalised device
    // coords (x, y in [-1, 1], y up), through the given camera. Selects it (or
    // clears that camera's selection on a miss) and returns the index or
    // BoxController::kNone. Called from the INPUT thread.
    std::size_t pickBoxAt(double ndcX, double ndcY, std::size_t cameraIndex);

    // ---- Component & object access (for SceneComponent / ObjectAction) ----
    void addComponent(std::unique_ptr<SceneComponent> component);
    // INPUT thread: offers a browser command to every component; true when one
    // of them consumed it.
    bool dispatchCommand(std::size_t camIndex, const nlohmann::json& msg);

    PhysicsWorld& physics() { return physics_; }
    wizengine::Renderer& renderer() { return renderer_; }

    // オブジェクト一覧へのアクセス。ロックは取らない: 物理スレッド（唯一の
    // 書き手）と、applyToRenderer の中（既にロック済み）から呼ぶこと。
    // それ以外のスレッドは lockObjects() を先に取る。
    std::size_t objectCount() const { return boxes_.size(); }
    GameObject& object(std::size_t i) { return boxes_[i]; }
    bool objectAlive(std::size_t i) const {
        return i < boxes_.size() && boxes_[i].alive;
    }
    [[nodiscard]] std::unique_lock<std::mutex> lockObjects() {
        return std::unique_lock<std::mutex>(objectsMutex_);
    }

    // Whiten strength for a grabbed object (kSelectedWhiten in scene.cpp).
    float selectedWhiten() const;
    // Highlight colour of the given camera's selection (scene.cpp).
    filament::math::float3 cameraColor(std::size_t cameraIndex) const;

    // JSON for the sidebar hierarchy, from the point of view of one camera:
    // cameras (with colours and who selected what), the lights, the objects,
    // and - new with the editor - the mode, the joints and the sim settings.
    // Called from the HTTP thread.
    std::string hierarchyJson(std::size_t cameraIndex);

    void build();

    // ---- モードと編集 ----------------------------------------------------
    EditorState& editor() { return editor_; }
    wizengine::editor::AppMode mode() const { return editor_.mode(); }
    // エディタ操作（モード切替・配置・ギズモ・ジョイント・保存/読込）を
    // 受け付けるカメラ。SceneConfig.h の kEditorCamera（既定 0）。
    std::size_t editorCamera() const;

    // PHYSICS thread: エディタモードの1パス（積分しない）。
    void stepEditor(double dt);
    // PHYSICS thread: 溜まっている編集操作とモード切替要求を処理する。
    // 両モードの先頭で呼ぶ（エディタ中しか編集できない、では不便なので
    // シミュレート中の設定変更もここを通る）。
    void applyPendingEdits();
    // PHYSICS thread: 掴んだオブジェクトをその位置へ「置き直す」。
    // エディタモードのドラッグ操作とギズモから呼ばれる。
    void moveObject(std::size_t index, double x, double y, double z);
    // 同上、姿勢（オイラー角・度）と寸法。寸法だけは剛体の作り直しが要るので
    // physDirty を立てるだけにして、シミュレート開始時にまとめて反映する。
    void rotateObject(std::size_t index, double rx, double ry, double rz);
    void resizeObject(std::size_t index, double sx, double sy, double sz);
    // 保存用のシーン文書（オブジェクト・ジョイント・設定）。PHYSICS thread。
    nlohmann::json documentJson() const;

    void stepPhysics(double dt);
    // Re-drop the boxes (browser Reset / R key).
    // Integration substeps per physics update (perf/stability trade-off).
    int substeps() const;
    int solverIterations() const;
    // Physics updates per second - independent of the render frame rate.
    int physicsHz() const;
    double collisionEnvelope() const;
    double contactRecovery() const;
    // Stop stepping and rendering while no browser is watching (web mode only).
    bool idleWhenUnwatched() const;
    void reset();
    void applyToRenderer();

private:
    void snapshot();  // copy body poses into latestPoses_ (thread-safe)

    // ---- エディタの実装（すべて PHYSICS スレッド）------------------------
    void enterMode(wizengine::editor::AppMode target);
    void applyEditorOp(const EditorState::Op& op);
    // 設計値からオブジェクトを1個作る。番号を返す。
    std::size_t createObject(const wizengine::editor::BodyDesc& desc);
    void destroyObject(std::size_t index);
    // 形・大きさ・質量が変わったオブジェクトの Chrono ボディを作り直す。
    // physDirty が立っているものだけが対象。
    void rebuildBody(std::size_t index);
    // 文書のジョイントを Chrono に作り直す（シミュレート開始時）。
    void buildJoints();
    // 置いた場所へ全部戻す（シミュレート停止時と Reset）。
    void restoreAuthoredPoses();
    // シミュレート設定を PhysicsWorld へ流し込む。
    void applySimSettings();
    void loadDocument(const nlohmann::json& doc);
    // ジョイントの端点に使う物理ボディ番号。-1 は地面。
    std::size_t jointBodyId(int objectIndex) const;

    // RENDER thread: 実体のできていないオブジェクトのレンダラブルを作り、
    // 削除済みのものを片付ける。applyToRenderer の先頭から呼ぶ。
    void syncRenderables();

    PhysicsWorld& physics_;
    wizengine::Renderer& renderer_;
    std::vector<std::unique_ptr<CameraObject>> cameras_;
    std::vector<std::unique_ptr<BoxController>> controllers_;
    std::vector<std::unique_ptr<LightObject>> lights_;

    std::vector<GameObject> boxes_;
    std::vector<std::unique_ptr<SceneComponent>> components_;
    EditorState editor_;
    // 地面の物理ボディ。ジョイントの「ワールド側」に使う。
    std::size_t groundPhysId_ = GameObject::kInvalidId;
    // glTF インスタンスは数が固定のプール（gltfio に 1 個だけ壊す口が無い）。
    // 先頭から順に配り、使い切ったら以降は組み込みメッシュで描く。消した
    // インスタンスは潰して見えなくするだけで、番号は返ってこない。
    std::size_t modelCapacity_ = 0;
    std::size_t modelNext_ = 0;
    // ConvexHull 判定に使う点群（scene.cpp の kBodyShape 用）。空 = 使わない。
    std::vector<chrono::ChVector3d> hullPoints_;
    // True when the dynamic objects are drawn as glTF instances (see
    // kBoxModelPath in scene.cpp).
    bool useModel_ = false;
    float modelScale_ = 1.0f;  // model units -> metres (see scene.cpp)
    std::mutex objectsMutex_;  // boxes_ の構造を変えるときだけ取る
    std::mutex poseMutex_;
    std::vector<BodyTransform> latestPoses_;  // one per box, in box order
};
