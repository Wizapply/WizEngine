#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <math/vec3.h>

#include "BoxController.h"
#include "CameraObject.h"
#include "EditorState.h"
#include "EditorTypes.h"
#include "GameObject.h"
#include "SceneComponent.h"
#include "SceneDocument.h"
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
    // maxCameras はカメラスロットの数（0 = SceneConfig.h の kMaxCameras）。
    // exe 引数 --max-cameras から来る。ページ・受け口はこの数だけ用意され、
    // エディタの「カメラを追加」もこの数まで（1〜16 に丸める）。
    Scene(PhysicsWorld& physics, wizengine::Renderer& renderer,
          std::size_t maxCameras = 0);

    // The scene's cameras. Each one has its own Filament view and its own
    // browser endpoint; index 0 is the default camera. Input events are routed
    // to one of these (input thread); the render loop draws each in turn.
    // スロットは kMaxCameras 個の固定プールで、エディタの追加・削除は
    // cameraActive の上げ下げ（エンドポイントは起動時に全部できている）。
    CameraObject& camera(std::size_t index = 0) { return *cameras_[index]; }
    std::size_t cameraCount() const { return cameras_.size(); }
    // 「削除」されたスロットは false。書くのは物理スレッド（camera.add/remove
    // の Op）。他のスレッドから読むときは lockObjects() を先に取る。
    bool cameraActive(std::size_t index) const {
        return index < camerasActive_.size() && camerasActive_[index] != 0;
    }

    // One grab/push component per camera, so each viewer holds and drives its
    // own object without fighting over a shared selection.
    BoxController& boxController(std::size_t cameraIndex = 0) {
        return *controllers_[cameraIndex];
    }

    // ---- ライト ------------------------------------------------------------
    // GameObject と同じ「設計値 + 実体番号」の対。書くのは物理スレッド
    // （light.* の Op とギズモのドラッグ）、実体（Filament のライト）の生成・
    // 破棄・反映は RENDER スレッド（syncLights）。構造を他スレッドから読む
    // ときは lockObjects()。ライトに物理は無いので Chrono には触らない。
    struct LightItem {
        wizengine::editor::LightDesc desc;
        std::size_t renderId = GameObject::kInvalidId;
        bool alive = true;
        bool stateDirty = true;  // 色・強さ・位置・向きの変更（実体は保つ）
        bool rebuild = false;    // 種類・影・減衰・円錐角: 実体を作り直す
        // イベントグラフのアクションが与える実行時の上書き。desc（設計値）は
        // 書き換えず、シミュレート停止で元へ戻る（オブジェクトの色と同じ）。
        bool hasRuntimeColor = false;
        wizengine::editor::Color3 runtimeColor;
        bool hasRuntimeIntensity = false;
        double runtimeIntensity = 0.0;
    };
    std::size_t lightCount() const { return lights_.size(); }
    LightItem& lightItem(std::size_t i) { return lights_[i]; }
    bool lightAlive(std::size_t i) const {
        return i < lights_.size() && lights_[i].alive;
    }

    // ---- glTF メッシュアセット（シーン文書の <asset><mesh/>）--------------
    // 文書の宣言（desc）と、実体側の遅延キャッシュ。modelId は RENDER
    // スレッドが最初に描くときに読み込む（Filament を触れるのはそのスレッド
    // だけ）。hull は PHYSICS スレッドが最初に当たり判定へ使うときに読み込む
    // （cgltf の CPU 処理だけ）。構造を他スレッドから読むときは lockObjects()。
    struct MeshAsset {
        wizengine::editor::MeshAssetDesc desc;
        std::size_t modelId = GameObject::kInvalidId;  // Renderer のモデル番号
        bool loadFailed = false;  // 読めなかった（毎フレーム試さない）
        std::vector<chrono::ChVector3d> hull;  // 凸包の点群（空 = 無し）
        bool hullTried = false;
    };

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

    // ---- ライト・カメラの編集（PHYSICS thread）----------------------------
    // ライトは設計値を書き換えるだけ（実体への反映は syncLights）。カメラは
    // CameraObject の atomic を書くので即座に効く。
    std::size_t createLight(const wizengine::editor::LightDesc& desc);
    void destroyLight(std::size_t index);
    void moveLight(std::size_t index, double x, double y, double z);
    void rotateLight(std::size_t index, double rx, double ry, double rz);
    // カメラの平行移動（視点をこの位置へ、向き・距離は保つ）と、その場での
    // 向き変え（pitch/yaw 度。orbit 表現なのでロールは無い）。
    void moveCamera(std::size_t index, double x, double y, double z);
    void rotateCamera(std::size_t index, double pitchDeg, double yawDeg);
    // カメラの編集用の姿勢表現（位置 = 視点、回転 = pitch/yaw 度、Z は常に 0）。
    // UI の数字・ギズモ・camera.set がこの 1 つの表現で揃う。CameraObject は
    // atomic なのでどのスレッドから読んでもよい。
    void cameraEditPose(std::size_t index, wizengine::editor::Vec3d& pos,
                        wizengine::editor::Vec3d& rot) const;

    // シーン文書（オブジェクト・ジョイント・ライト・カメラ・イベント・設定）。
    // これが保存される中身そのもので、XML と 1 対 1 に対応する
    // （SceneDocument.h）。オブジェクト一覧のロックを自分で取るので、
    // PHYSICS スレッド（保存）からも HTTP スレッド（/scene.xml）からも呼べる。
    wizengine::editor::SceneDocument document();
    // 同じものを XML テキストにしたもの。保存されるファイルの中身と同じ。
    std::string documentXml();

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
    // desc.mesh（アセット名）→ meshes_ の番号。-1 = 無い（球で描く）。
    int meshIndexFor(const std::string& name) const;
    // メッシュの凸包（最初に使うときに読み込む）。nullptr = 読めない。
    const std::vector<chrono::ChVector3d>* meshHull(int meshIndex);
    // 設計値から Chrono のボディを 1 個（createObject / rebuildBody 共通）。
    std::size_t createBody(const wizengine::editor::BodyDesc& desc,
                           int meshIndex);
    void destroyObject(std::size_t index);
    // 形・大きさ・質量が変わったオブジェクトの Chrono ボディを作り直す。
    // physDirty が立っているものだけが対象。
    void rebuildBody(std::size_t index);
    // 文書のジョイントを Chrono に作り直す（シミュレート開始時）。
    void buildJoints();

    // ---- イベントグラフの実行（PHYSICS スレッド）--------------------------
    // シミュレートの 1 サブステップごとに、トリガー（衝突・開始・タイマー）を
    // 判定し、ワイヤーで繋がったアクションを実行する。stepPhysics から呼ぶ。
    void runEventGraph(double dt);
    // 発火したトリガーから繋がった 1 個のアクションを実行する。
    void runGraphAction(const wizengine::editor::NodeDesc& node, double dt);
    // 実行状態（タイマー・接触の記憶・発火カウント）と、アクションが加えた
    // 実行時の上書き（色・ライト・固定）を捨てて設計値へ戻す。
    // シミュレートの開始・停止・Reset で呼ぶ。
    void resetGraphRuntime();
    // 消えた対象（オブジェクト / ライト / カメラ）を参照するノードを掃除する。
    // 対象そのものが消えたノードは削除、OnCollision の相手フィルタだけが
    // 消えた場合は「何でも」(-2) に戻す。ジョイントの掃除と同じ流儀。
    void pruneGraphForRemoved(wizengine::editor::NodeTargetKind kind, int index);

    // 置いた場所へ全部戻す（シミュレート停止時と Reset）。
    void restoreAuthoredPoses();
    // シミュレート設定を PhysicsWorld へ流し込む。
    void applySimSettings();
    // 文書の中身で今のシーンを置き換える（読み込み・起動時のシーン指定）。
    void loadDocument(const wizengine::editor::SceneDocument& doc);
    // ジョイントの端点に使う物理ボディ番号。-1 は地面。
    std::size_t jointBodyId(int objectIndex) const;

    // RENDER thread: 実体のできていないオブジェクトのレンダラブルを作り、
    // 削除済みのものを片付ける。applyToRenderer の先頭から呼ぶ。
    void syncRenderables();
    // RENDER thread: ライトも同じ流儀で（生成・破棄・状態反映）。
    void syncLights();

    // ライト・カメラを SceneConfig の初期値へ戻す（clear と、ライト/カメラを
    // 持たない古い保存文書の読み込み）。PHYSICS thread。
    void resetLightsToDefaults();
    void resetCamerasToDefaults();

    // 地面・環境光の差し替え（PHYSICS thread）。物理の床が変わるときは
    // その場で作り直し、見た目は dirty を立てて RENDER スレッドに任せる。
    void setGroundAndEnvironment(const wizengine::editor::GroundDesc& ground,
                                 const wizengine::editor::EnvironmentDesc& env);
    // ground_.half の物理の床を作り直す（初回は新規作成）。PHYSICS thread。
    void rebuildGroundBody();
    // RENDER thread: 見える地面の作り直し（applyToRenderer のロック中）。
    void syncGround();

    PhysicsWorld& physics_;
    wizengine::Renderer& renderer_;
    std::vector<std::unique_ptr<CameraObject>> cameras_;
    std::vector<std::unique_ptr<BoxController>> controllers_;
    std::vector<char> camerasActive_;  // 構造は objectsMutex_ が守る
    std::vector<LightItem> lights_;    // 同上

    std::vector<GameObject> boxes_;
    std::vector<std::unique_ptr<SceneComponent>> components_;
    EditorState editor_;
    // 地面の物理ボディ。ジョイントの「ワールド側」に使う。
    std::size_t groundPhysId_ = GameObject::kInvalidId;
    // メッシュアセットのカタログ（文書の <asset>）。構造は objectsMutex_。
    std::vector<MeshAsset> meshes_;
    // 地面と環境光（文書の <ground> / <environment>）。書くのは物理スレッド
    // （loadDocument / clear）、読むのは RENDER スレッド（dirty を見て反映）。
    // どちらも objectsMutex_ の下。物理の床の作り直しは物理スレッドが即座に
    // 行い（rebuildGroundBody）、見た目は RENDER スレッドがフレーム境界で
    // 追いつく（syncGround / 環境光は applyToRenderer 末尾のロック外）。
    wizengine::editor::GroundDesc ground_;
    wizengine::editor::EnvironmentDesc environment_;
    bool groundDirty_ = true;  // 初回の applyToRenderer が見た目を作る
    bool envDirty_ = true;
    std::mutex objectsMutex_;  // boxes_ の構造を変えるときだけ取る
    std::mutex poseMutex_;
    std::vector<BodyTransform> latestPoses_;  // one per box, in box order

    // ---- イベントグラフの実行状態（PHYSICS スレッド専用）------------------
    // グラフ本体は EditorState が持ち、ここにあるのは実行のためのキャッシュと
    // 走らせている最中にだけ意味を持つ値。resetGraphRuntime が全部捨てる。
    struct GraphRuntime {
        // 取り込んだグラフの版。EditorState::graphVersion と食い違ったら
        // 一覧をコピーし直す（毎ステップのロックを避けるための番号）。
        std::uint64_t version = ~std::uint64_t(0);
        std::vector<wizengine::editor::NodeDesc> nodes;
        std::vector<wizengine::editor::WireDesc> wires;
        // OnTimer の経過秒。ノード id で引く（グラフ編集で他のノードの
        // タイマーがリセットされないように、位置ではなく id）。
        std::map<int, double> timers;
        bool startFired = false;  // OnSimStart は最初のステップで 1 回だけ
        // 前サブステップの接触ペア（オブジェクト番号、-1 = 地面）。
        // 今回あって前回無いペアだけが「新しくぶつかった」。
        std::set<std::pair<int, int>> prevContacts;
        // 最初の収集パスは「覚えるだけ」（false の間）。開始時点で既に触れて
        // いた接触（置いた箱と地面など）を衝突として発火させないため。
        bool contactsPrimed = false;
        // SetFixed アクションが触ったオブジェクト番号。停止時にこれだけ
        // desc.fixed へ戻す（全ボディを毎回触らないための記録）。
        std::set<std::size_t> fixedTouched;
    };
    GraphRuntime graphRt_;
};
