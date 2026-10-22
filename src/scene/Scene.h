#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <math/vec3.h>

#include "scene/BoxController.h"
#include "scene/CameraObject.h"
#include "scene/EditorState.h"
#include "document/EditorTypes.h"
#include "scene/GameObject.h"
#include "scene/SceneComponent.h"
#include "document/SceneDocument.h"
#include "physics/MeshCollision.h"  // CollisionTriangles
#include "physics/PhysicsWorld.h"  // for BodyTransform

namespace wizengine {
class Renderer;
namespace terrain {
class HeightField;
struct GridRect;
class TerrainLibrary;
class TerrainUndoStack;
struct TerrainResolution;
}
}

// One scene tying physics bodies to their renderables, split for threading:
//   - build()          : set up both engines (single-threaded, before threads)
//   - stepPhysics(dt)  : PHYSICS thread - advance the sim, snapshot poses
//   - stepEditor(dt)   : PHYSICS thread - editor mode: no integration, just
//                        apply edits and keep the snapshot fresh
//   - reset()          : PHYSICS thread - put everything back where it was placed
//   - applyToRenderer(): RENDER thread  - push the latest poses to renderables
// The pose snapshot is the only shared state between the two threads.
// Engine defaults live in SceneConfig.h; scene content (objects, joints,
// events) lives in the scene document (assets/scenes/*.xml). The
// implementation is split by concern: Scene.cpp (build / step / render sync),
// SceneEdit.cpp (editor ops), SceneEvents.cpp (grab + event graphs),
// SceneSerialize.cpp (document + browser JSON), SceneTerrain.cpp (terrain
// grid / collision / render sync), SceneTerrainFile.cpp (terrain <-> document,
// .terrain files and browser JSON) and SceneTerrainEdit.cpp (edit.terrain
// ops); SceneInternal.h is shared.
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

// Which physics backend the scene wants (SceneConfig.h の kBackend。CMake が
// Multicore 無しなら Core に落ちる); main constructs the PhysicsWorld from it.
PhysicsBackend scenePhysicsBackend();

class Scene {
public:
    // maxCameras はカメラスロットの数（0 = SceneConfig.h の kMaxCameras）。
    // exe 引数 --max-cameras から来る。ページ・受け口はこの数だけ用意され、
    // エディタの「カメラを追加」もこの数まで（1〜16 に丸める）。
    Scene(PhysicsWorld& physics, wizengine::Renderer& renderer,
          std::size_t maxCameras = 0);
    // .cpp で定義する（テレインの窓口と Undo を前方宣言の型の unique_ptr で
    // 持つため。完全な型が見える所で壊す）。
    ~Scene();

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

    // ---- マウスで掴んでいる状態（PHYSICS スレッド）--------------------------
    // 「どの物を、カーソルのどの点へ」。掴んだ時点の奥行きを覚えておき、その
    // 平面上でカーソルの指す点を返すので、カメラを回しても物が寄って来ない。
    // 使い道は 2 つ: エディタモードの置き直し（BoxControlComponent）と、
    // シミュレート中の引き寄せ（イベントグラフの onGrab / grabPull）。
    // **掴んだら勝手に動く、という配線はエンジンには無い**: 動かすかどうかは
    // イベントアセット次第（既定シーンに付いている "pickup" がそれ）。
    struct PointerGrab {
        bool valid = false;
        std::size_t camera = 0;
        std::size_t index = 0;   // オブジェクト番号
        std::size_t physId = 0;
        double objX = 0.0, objY = 0.0, objZ = 0.0;  // 物のいまの位置
        double tgtX = 0.0, tgtY = 0.0, tgtZ = 0.0;  // カーソルが指す点
    };
    bool pointerGrab(std::size_t cameraIndex, PointerGrab& out);
    // 引っぱり線（カメラごと）。物理スレッドが書き、描画スレッドが読む。
    // シーンの中の線として描くので、どのカメラから見ても同じフレームに乗る。
    void setGrabLine(std::size_t cameraIndex, bool on, double ax, double ay,
                     double az, double bx, double by, double bz);
    void clearGrabLines();
    bool grabLine(std::size_t cameraIndex, double* from3, double* to3) const;

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
        // 三角メッシュそのまま（<geom collision="trimesh">）。凸包と同じく
        // 最初に使うときに読む。
        wizengine::CollisionTriangles trimesh;
        bool trimeshTried = false;
    };

    // ジョイントの計測値（直前のステップの反力と破断）。物理スレッドが
    // stepPhysics で書き、HTTP スレッド（/scene）と RENDER スレッド（線の
    // 表示）が読む。poseMutex_ の下。
    struct JointStat {
        double force = 0.0;   // 拘束の反力 (N)。ばねはばね力
        double torque = 0.0;  // 反トルク (N·m)。モータの出力もここ
        bool broken = false;  // breakForce を超えて外れた
    };
    std::vector<JointStat> jointStats();

    // ---- バックエンドの自動切替（B）------------------------------------------
    // 既定は SceneConfig.h の kBackend（Core）。Multicore が扱えない機能
    // （FEA のケーブル・ChLoad のブッシュ / 定常荷重・直接法と MINRES・
    // HHT / Newmark・モーダル解析）を文書が使っていたら Core、無くて
    // ソフトボディがあるかボディが多ければ Multicore（kMulticoreForSoftBodies /
    // kMulticoreMinBodies）、どちらでもなければ既定。接触の解き方（nsc /
    // smc）も系の種類なので同じ口。
    // 判定と切替はシミュレート開始・設定変更・読込のたび（syncBackend）。
    // いま何で動いていて、なぜかは backendNote（/scene と /stats に出す）。
    std::string backendNote();
    // 直近のモーダル解析の結果（Hz、昇順。無ければ空）。poseMutex_ の下。
    std::vector<double> modalFrequencies();
    // ケーブル（FEA）の節点位置のスナップショット（RENDER スレッド用。
    // シミュレートしていないときは空 = 設計値の直線で描く）。
    bool latestCable(std::size_t index, std::vector<float>& out);

    // ---- 地面（文書の <ground>）--------------------------------------------
    // 地面があるか（削除 = <ground enabled="false"> でないか）。PHYSICS
    // スレッドか objectsMutex_ を持って呼ぶ（ground_ と同じ約束）。
    bool groundEnabled() const { return ground_.enabled; }
    // テレインが文書上あるか（Missing も含む）。約束は groundEnabled と同じ。
    bool hasTerrain() const { return hasTerrain_; }
    // 置き場所（Transform）。読むのは groundEnabled と同じ約束。
    const wizengine::editor::Vec3d& groundPosition() const { return ground_.position; }
    const wizengine::editor::Vec3d& groundRotation() const { return ground_.rotation; }
    const wizengine::editor::Vec3d& terrainPosition() const { return terrain_.position; }
    const wizengine::editor::Vec3d& terrainRotation() const { return terrain_.rotation; }
    // ギズモの移動 / 回転（PHYSICS thread、onEditorStep から毎フレーム来うる）。
    // 地面は物理の床を置き直し（作り直さない）、テレインはフレームを差し替える
    // だけ（格子・メッシュ・当たり判定は作り直さない）。角度はオイラー角（度）。
    void moveGround(double x, double y, double z);
    void rotateGround(double xDeg, double yDeg, double zDeg);
    // テレインのギズモは「中心の地表」に出す（原点 = 中心の最低点は丘の下に
    // 埋もれて見えないため）。moveTerrain はこの点の新しい位置を受け、
    // rotateTerrain はこの点を中心に回す（どちらも terrain_.position へ換算）。
    void moveTerrain(double x, double y, double z);
    void rotateTerrain(double xDeg, double yDeg, double zDeg);
    wizengine::editor::Vec3d terrainHandlePoint() const;

    // ---- テレイン（文書の <terrain>）--------------------------------------
    // 高さ格子の実体（読み取り専用）。無ければ nullptr。格子は差し替えるたびに
    // 新しく作る（中身は書き換えない）ので、受け取った shared_ptr はそのまま
    // 別スレッドで読み続けてよい。どのスレッドから呼んでもよい（専用の
    // 末端ロック terrainFieldMutex_ だけを取る = 他のロックの中から呼べる）。
    std::shared_ptr<const wizengine::terrain::HeightField> terrainField() const;
    // 同じ。格子と同じロックの中でテレインのフレーム（置き場所 + 回転。格子は
    // 原点基準 = terrain/TerrainFrame.h）も pose に返す（null なら返さない）。
    // 格子を問い合わせる側はこの組で使う（片方だけ新しい組を見ない）。
    std::shared_ptr<const wizengine::terrain::HeightField> terrainField(
        BodyTransform* pose) const;
    // レイと地表の交差（HeightField::raycast。車輪のレイ用）。テレインが無い・
    // 当たらなければ false で、t / normal（normal は x, y, z の 3 要素）は
    // 触らない。origin / dir / normal はワールド（フレームはここで掛ける）。
    // スレッドの扱いは terrainField() と同じ。
    bool terrainRaycast(const double origin[3], const double dir[3], double maxDist,
                        double& t, double normal[3]) const;
    // テレインの当たり判定のボディ（rebuildTerrainBody）は自前の接触の相手の
    // ゴースト 1 個（PhysicsWorld::setTerrain が返す番号。GameObject ではない =
    // boxes_ に入らず、選択・掴み・イベントの対象にならない）。以下 2 つは
    // PHYSICS スレッド専用。当たり判定は edit.terrain のたびにその場で張り
    // 直す（格子を渡し直すだけ）ので、高さ格子（terrainRaycast）と常に一致している。
    // id がテレインのボディ（ゴースト）か。接触の相手を「地面」と見なしたい側
    // （イベントの衝突・車両の診断）用。
    bool isTerrainBody(std::size_t id) const {
        return terrainPhysId_ != PhysicsWorld::kInvalidId && id == terrainPhysId_;
    }
    // テレインの当たり判定があるか（作れなかった・自前の接触を張れない
    // （Multicore）・テレインが無いなら false）。
    bool hasTerrainCollision() const { return terrainPhysId_ != PhysicsWorld::kInvalidId; }

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

    // desc.mesh（アセット名）→ meshes_ の番号。-1 = 無い（球で描く）。
    // ロックは取らない（物理スレッド、または applyToRenderer の中から）。
    int meshIndexFor(const std::string& name) const;
    // RENDER スレッド用: メッシュアセットの Renderer モデル番号。最初に使う
    // ときに読み込む（syncRenderables と同じ）。読めなければ kInvalidId。
    std::size_t meshModelId(int meshIndex);
    double meshScale(int meshIndex) const;

    // ---- プレハブの部品の編集（PHYSICS thread、ギズモから）------------------
    // 対象はプレハブ編集モードのオブジェクト（EditorState::prefabEditObject）
    // に付いているプレハブの部品。ワールド座標を受け、親フレーム（車体 /
    // ソケット）のローカルへ直して書く。
    void movePart(int part, double x, double y, double z);
    // 回転はワールドの四元数（w, x, y, z）で受ける。
    void rotatePartWorld(int part, double qw, double qx, double qy, double qz);
    void resizePart(int part, double sx, double sy, double sz);

    // このフレームに描くオブジェクトの姿勢（RENDER スレッド専用。onRender の
    // 中で車輪など「オブジェクトに付いた見た目」を、描かれる車体と同じ姿勢
    // から組むための口）。シミュレート中は物理の 2 フレームの間を補間した
    // もの（buildRenderFrame）。applyToRenderer の先頭で作るので、ロックは
    // 取らない。無ければ false。
    bool latestPose(std::size_t index, BodyTransform& out) const;
    // このフレームの補間係数（RENDER スレッド専用）。0〜1 = 前回の物理
    // フレーム → 今回の間、負 = 補間していない（最新の姿勢をそのまま描く）。
    // 物理とは別に描画用の値を持つ部品（VehicleComponent の車輪）が、車体と
    // 同じ割合で補間するために読む。
    double renderAlpha() const { return renderAlpha_; }

    // Whiten strength for a grabbed object (kSelectedWhiten in SceneConfig.h).
    float selectedWhiten() const;
    // Highlight colour of the given camera's selection (cameraColors() in
    // SceneConfig.h).
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
    // PHYSICS thread: 物理の 1 パス（サブステップと取り戻しのぶん全部）を
    // 回し終えたら呼ぶ。最新の姿勢を描画補間用のフレームとして公開する
    // （前回のフレームと合わせて、描画はその間を補間する）。
    void endPhysicsPass();
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
    // copy body poses into latestPoses_ (thread-safe)。cut = true は「連続した
    // 動きではない」更新（置き直し・読込・モード切替・Reset など、物理の
    // ステップ以外から呼ぶもの全部）で、描画はそこを補間でつながない。
    void snapshot(bool cut = true);
    // RENDER thread: このフレームに描く姿勢（renderPoses_ など）を作る。
    void buildRenderFrame();

    // ---- エディタの実装（すべて PHYSICS スレッド）------------------------
    void enterMode(wizengine::editor::AppMode target);
    void applyEditorOp(const EditorState::Op& op);
    // applyEditorOp の "terrain" 分岐の中身（edit.terrain、SceneTerrainEdit.cpp）。
    // ファイルの分岐（applyTerrainFileOp）のあと、generate / undo / enabled(+name) /
    // 部分更新。
    void applyTerrainOp(const EditorState::Op& op);
    // edit.terrain のファイルを扱う分岐（refresh / rename / delete / open / new）。
    // 扱ったら true。
    bool applyTerrainFileOp(const nlohmann::json& a);
    // 設計値からオブジェクトを1個作る。番号を返す。
    std::size_t createObject(const wizengine::editor::BodyDesc& desc);
    // プレハブ編集モードのオブジェクトに付いているプレハブ名（無ければ空）。
    std::string editingPrefabName() const;

    // メッシュの凸包（最初に使うときに読み込む）。nullptr = 読めない。
    const std::vector<chrono::ChVector3d>* meshHull(int meshIndex);
    // メッシュの三角形（同上。collision="trimesh" 用）。
    const wizengine::CollisionTriangles* meshTrimesh(int meshIndex);
    // 設計値から Chrono のボディを 1 個（createObject / rebuildBody 共通）。
    // ソフトボディ（desc.hasSoft）なら粒子の格子を作り、その設計図を lattice
    // へ返す（剛体なら nullptr）。戻り値は physId（ソフトなら代表番号）。
    // 固定の持ち主の collide 部品は別の固定ボディ（children に番号）、動く
    // 持ち主のものは本体の複合形状になる。
    // hullCenter は凸包で当たるとき Chrono が寄せた体積重心（ボディ座標、m。
    // それ以外は 0）。RENDER スレッドが見た目を同じだけ逆にずらして重ねる。
    std::size_t createBody(
        const wizengine::editor::BodyDesc& desc, int meshIndex,
        std::shared_ptr<const wizengine::softlattice::Lattice>& lattice,
        std::vector<std::size_t>& children,
        wizengine::editor::Vec3d& hullCenter);
    // プレハブの collide 部品を Chrono の追加形状（複合形状）に直す（createBody 用）。
    std::vector<PhysicsWorld::ExtraShape> collisionShapes(
        const wizengine::editor::BodyDesc& desc) const;
    // プレハブの collide 部品が変わったら、それを付けている物の当たり形状を
    // 作り直す（エディタ中は physDirty だけ、シミュレート中は即 rebuildBody）。
    void markPrefabUsersDirty(const std::string& prefab);
    void destroyObject(std::size_t index);
    // 形・大きさ・質量が変わったオブジェクトの Chrono ボディを作り直す。
    // physDirty が立っているものだけが対象。
    void rebuildBody(std::size_t index);
    // 別ボディの部品（childPhysIds）を持ち主の今の姿勢へ作り直す。
    void rebuildChildren(std::size_t index);
    // 文書のジョイントを Chrono に作り直す（シミュレート開始時）。
    void buildJoints();
    // 直前のステップの反力・破断を jointStats_ へ（stepPhysics から）。
    void updateJointStats();
    // ジョイントを 1 本消したあとの後始末: それを指すノードを消し、後ろの
    // 番号を指すノードを 1 つ前へずらす（ジョイントは番号で参照するため）。
    void pruneJointNodes(int removedIndex);

    // ---- バックエンド・ケーブル・モーダル（PHYSICS スレッド）---------------
    // 文書が要求するバックエンドと接触の解き方。reason には Core を要する
    // 理由、または規模で Multicore を選んだ理由（英語、ログ用）。
    PhysicsBackend requiredBackend(ContactMethod& contact, std::string& reason);
    // 要求と今の系が違えば作り直す（rebuildPhysicsWorld）。作り直したら true
    // （そのときはテレインの当たり判定も組み直し済み）。
    bool syncBackend();
    // 系をまるごと作り直し、地面・全オブジェクト（設計値から）・シミュレート
    // 中ならジョイントとケーブルも組み直す。
    void rebuildPhysicsWorld(PhysicsBackend backend, ContactMethod contact);
    // 許容差・接触設定・転がり摩擦（SceneConfig の定数）を PhysicsWorld へ。
    // build と rebuildPhysicsWorld から。
    void configurePhysicsDefaults();
    // 文書のケーブルを Chrono の FEA に作り直す / 捨てる（ジョイントと同じ
    // タイミング: シミュレート開始と停止）。
    void buildCables();
    void removeAllCables();
    // <option modal="N">: 固有振動数を求めてログと /scene に出す。
    void runModalAnalysis();
    // RENDER thread: ケーブルの見た目（円柱の連なり）を作り、節点の
    // スナップショット（無ければ設計値の直線）に沿って置く。
    void syncCables();
    // 文書の中身を今のシーンへ**足す**（URDF などの取込。loadDocument は
    // 置き換え）。ボディ・ジョイント・ケーブル・メッシュ・プレハブが対象。
    void appendDocument(const wizengine::editor::SceneDocument& doc);

    // ---- イベントグラフの実行（PHYSICS スレッド）--------------------------
    // シミュレートの 1 サブステップごとに、**付いているイベントアセット**の
    // トリガー（衝突・開始・タイマー・掴み）を判定し、ワイヤーで繋がった
    // アクションを実行する。stepPhysics から呼ぶ。
    void runEventGraph(double dt);
    // 実行の文脈。「そのトリガーがどの物に対して起きたか」をアクションへ
    // 渡すためのもので、対象を書いていないノード（target = -1）の既定になる。
    struct GraphContext {
        int self = -1;    // アセットが付いているオブジェクト（-1 = ワールド）
        int object = -1;  // トリガーが指した物（掴んだ物・触れた物）
        std::size_t camera = 0;              // OnGrab: 掴んでいるカメラ
        bool hasPoint = false;               // OnGrab: カーソルの指す点
        double px = 0.0, py = 0.0, pz = 0.0;
    };
    // 発火したトリガーから繋がった 1 個のアクションを実行する。
    void runGraphAction(const wizengine::editor::NodeDesc& node,
                        const GraphContext& ctx, double dt);
    // オブジェクトを対象にするノードの実際の対象番号。番号を書いてあれば
    // それ、書いていなければトリガーが渡してきた物、それも無ければ付いて
    // いる相手（ワールド付けなら -1 = 対象なし）。
    static int graphTarget(const wizengine::editor::NodeDesc& node,
                           const GraphContext& ctx);
    // 付いているアセットの一覧（実行の単位）を作り直す。
    void rebuildGraphInstances();
    // イベントアセットを既定の構成（マウス操作スクリプト）へ戻す。
    // 節を持たない文書の読み込みと「全消し」で使う - ライトを 1 灯も書かない
    // 文書が初期構成で開くのと同じ扱い（掴んでも動かないシーンで始めない）。
    void resetEventsToDefaults();
    // そのアセットをオブジェクトから全部外す（アセットを消したとき）。
    void detachEventFromObjects(const std::string& name);
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
    // 文書の中身で今のシーンを置き換える（読み込み・起動時のシーン指定・XML の
    // 適用）。keepSameTerrain = 文書のテレインが今のものと同じなら手元の格子を
    // 保つ（XML の適用だけ。シーンの読込はファイルから読み直す）。
    void loadDocument(const wizengine::editor::SceneDocument& doc, bool keepSameTerrain);
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
    // 描画設定（文書の <visual>）の差し替え（PHYSICS thread）。適用は
    // RENDER スレッド（Filament を触るのはそのスレッドだけ）。
    void setRenderDesc(const wizengine::editor::RenderDesc& render);
    // 地面 / テレインをエディタ選択にする（追加・開いた直後に Inspector へ
    // 出すため）。エディタカメラのオブジェクト選択は外す（排他）。PHYSICS thread。
    void selectSceneItem(EditorState::SelKind kind);
    // 物理の床の姿勢を ground_ の置き場所へ（作り直さない）。PHYSICS thread。
    void placeGroundBody();
    // 床の箱の中心と向き（上面 = 地面の面）。
    void groundBodyPose(chrono::ChVector3d& center, chrono::ChQuaternion<>& rot) const;
    // ground_.half の物理の床を作り直す（初回は新規作成）。地面を削除して
    // あれば当たり判定の無い固定フレーム（ジョイントのワールド側の受けだけ）。
    // PHYSICS thread。
    void rebuildGroundBody();
    // RENDER thread: 見える地面の作り直し（applyToRenderer のロック中）。
    void syncGround();

    // ---- テレイン（SceneTerrain.cpp、すべて PHYSICS thread）----------------
    // publishTerrainField の指定: Undo の記録を残すか消すか。
    enum class TerrainHistory { Keep, Clear };
    // 同じく .terrain との関係（terrainFile_）: そのまま / 書くべき変更がある /
    // ファイルと一致している / ファイルが見つからない / ファイルが読めない
    // （壊れている・形が範囲外など）。Keep は引数専用（terrainFile_ には入らない）。
    // Missing / Unreadable は field = nullptr と一緒に渡す（Missing:
    // 文書上はテレインがあるが格子は無い = 描画も当たり判定もしない・編集できない・
    // 保存でファイルを作らない・XML には名前と背番号を残す）。
    enum class TerrainFileState { Keep, Dirty, Saved, Missing, Unreadable };
    // 以下 3 つは hasTerrain_ と同じく、PHYSICS スレッドか objectsMutex_ を持って呼ぶ。
    // 今のテレインに .terrain へ書いていない変更があるか（保存で書く目印）。
    bool terrainDirty() const { return terrainFile_ == TerrainFileState::Dirty; }
    // 文書上はテレインがあるが、ファイルが無い・読めないので格子が無いか。
    bool terrainMissing() const {
        const TerrainFileState f = terrainFile_;
        return f == TerrainFileState::Missing || f == TerrainFileState::Unreadable;
    }
    // Missing のうち、ファイルはあるが読めない方か（terrainUnreadableFile_ が意味を持つ間）。
    bool terrainUnreadable() const { return terrainFile_ == TerrainFileState::Unreadable; }
    // 格子のあるテレインがあるか（Missing は含まない）。
    bool terrainLoaded() const { return hasTerrain_ && !terrainMissing(); }
    // 高さ格子の差し替え。**格子・描画の dirty・テレインの有無・ファイルとの関係
    // （terrainFile_）・Undo の消去を触るのはここだけ**（保存の成功で Saved に戻す
    // saveTerrainData を除く）。
    // field = null ならテレイン無し（当たり判定も即外し、ファイルとの関係は Saved =
    // 外した物に書く変更は無い）。ただし file が Missing / Unreadable なら「文書上は
    // ある・格子は無い」テレイン（hasTerrain_ は立てたまま、その状態を覚える）。
    // desc はそのまま覚える（丸めない = 呼び手が clampTerrain 済み）。
    // 格子が変わったら描画の dirty を立て、表面だけの変更は表面の dirty だけ。
    // 最後に必ず syncBackend を呼ぶ（テレインは Core を要する）ので、読込では
    // 他の中身を作り終えてから呼ぶこと。系を作り直した場合は、その中
    // （rebuildPhysicsWorld → rebuildTerrainBody）で当たり判定も組まれる。
    // そうでなければ、格子が変わったか当たり判定が無いときにその場で張り直す
    // （ゴーストに格子を渡し直すだけ）ので、戻ったときは必ず今の格子と一致している。
    void publishTerrainField(const wizengine::editor::TerrainDesc& desc,
                             std::shared_ptr<const wizengine::terrain::HeightField> field,
                             TerrainHistory history, TerrainFileState file);
    // 形の変更（size / pos = 値を保って置き直し、resolution = 再サンプル、
    // height = 比例伸縮）。.terrain が持つ形（size / resolution / height）が
    // 変われば Undo を消してファイルの dirty を立てる。pos だけなら格子を置き
    // 直すだけ（Undo もファイルもそのまま）。形が同じなら表面だけ（テレインが
    // 無ければ値を覚えるだけ）。
    void reshapeTerrain(const wizengine::editor::TerrainDesc& next);
    // 格子の書き換えの唯一の口（コピーオンライト）: 今の格子をコピーし、record
    // なら rect（セル単位 = TerrainUndo の約束）の今の値を Undo に積んでから
    // mutate(コピー) を呼び、publishTerrainField で差し替える（Undo は残し、
    // ファイルの dirty を立てる）。mutate が false を返したら何も出さない
    // （記録も積まない）。テレインが無ければ何もせず false。
    bool editTerrainField(
        const wizengine::terrain::GridRect& rect,
        const std::function<bool(wizengine::terrain::HeightField&)>& mutate, bool record);
    // 今の格子の高さをノイズで作り直す（形・穴はそのまま。Undo は 1 段）。
    void generateTerrainNoise(const wizengine::editor::TerrainNoiseParams& n);
    // 最後の編集を 1 段取り消す（結果はステータス）。
    void undoTerrain();

    // ---- テレインの出入り（SceneTerrainFile.cpp、すべて PHYSICS thread）-------
    // 文書の <terrain> から今のテレインを作る（loadDocument から）。
    // keepSameTerrain かつ同じテレイン（sameTerrainIdentity）なら格子を保って
    // pos と表面だけ取る、旧形式ならノイズで一度生成、新形式なら .terrain を
    // 解決して読む。
    void setTerrainFromDocument(const wizengine::editor::SceneDocument& doc,
                                bool keepSameTerrain);
    // 新形式の <terrain>（doc）を解決 → 読む → publish する（setTerrainFromDocument
    // から）。一覧を取り直してから desc.data / guid を assets/terrains/ で解決し、
    // 読めれば読んだ形（data / guid / size / resolution / height）で Saved として
    // 出す。読めなければ Missing = 文書の desc をそのまま（XML の
    // data / guid を保つ）格子無しで出す:
    //   * ファイルが決まらない → Missing
    //   * ファイルはあるが使えない（readTerrainFile が断る）→ Unreadable
    //     （そのファイルは使わず上書きもしない）
    // 理由（英語）は LOGW に、注意は terrainLoadCaveat_ に入れる。
    void loadTerrainField(const wizengine::editor::TerrainDesc& doc);
    // name のファイルを読み、使えるなら desc.position に置いた格子を返して、desc の
    // data / guid / size / resolution / height を読んだ値で埋める（位置と表面は
    // そのまま）。**使えないファイル**は null と英語の reason（desc は触らない）:
    //   * 読めない（壊れている・形式が違う）
    //   * 背番号が全部 0（背番号での解決と食い違う）
    //   * 形が clampTerrain を通すと変わる（解像度 2048・size 5000 など。エディタの
    //     範囲で丸めて使うと、保存で共有ファイルが黙って劣化する）
    // 範囲の定義は clampTerrain の 1 か所で、ここは「通すと変わるか」を比べるだけ。
    // loadTerrainField と openTerrain が使う。
    std::shared_ptr<const wizengine::terrain::HeightField> readTerrainFile(
        const std::string& name, wizengine::editor::TerrainDesc& desc,
        std::string& reason) const;
    // 文書のテレインが name を参照しているか（テレインがあり、大文字・小文字を
    // 問わず一致）。Missing（格子が無い）も含む。「参照している名前か」の唯一の判定。
    bool refersToTerrain(const std::string& name) const;
    // name が読めている今のテレインの名前か（terrainLoaded() && refersToTerrain）。
    // 未保存でファイルが無いテレインも含む。Missing は含まない（同じ名前を開き
    // 直せる）。「今のテレインか」の唯一の判定。
    bool isCurrentTerrain(const std::string& name) const;
    // name が、いま参照している読めないファイル（Unreadable の terrainUnreadableFile_）か
    // （大文字・小文字を問わず一致）。改名がその名前の追従に使う。
    bool isUnreadableTerrainFile(const std::string& name) const;
    // a が今のテレインと同じものか（背番号が両方にあれば背番号、無ければ名前）。
    // Missing なら false（XML の適用で読み直す）。
    bool sameTerrainIdentity(const wizengine::editor::TerrainDesc& a) const;
    // 直前の読込で出たテレインの注意（ステータスの末尾に足す日本語 1 文）を返して
    // 消す。無ければ空。terrainCombineCaveat と同じ使い方。
    std::string terrainLoadCaveat();
    // desc の名前が空なら base から空いている名前（uniqueName）を、背番号が空なら
    // 新しい背番号を入れる（空でない方はそのまま）。新しいテレインの名前と背番号を
    // 決める唯一の口: 新規・旧形式の読込は両方を空にしてから呼び、保存は欠けて
    // いる方だけを埋める。
    void fillTerrainIdentity(wizengine::editor::TerrainDesc& desc,
                             const std::string& base) const;
    // 名前と背番号を新しく決めて（fillTerrainIdentity）、ファイルの dirty を立てて
    // 出す（Undo は消す）。
    // 付けた名前を返す。
    std::string publishNewTerrain(wizengine::editor::TerrainDesc desc,
                           std::shared_ptr<const wizengine::terrain::HeightField> field,
                           const std::string& base);
    // 保存（"save" の document() の前）。格子のあるテレインがあり、dirty か
    // ファイルが無いときだけ書く（Missing は格子が無いので書かない）。名前が無ければ
    // sceneName から採る。書いた名前を written に（書かなければ空）。失敗なら
    // false と英語の reason。
    bool saveTerrainData(const std::string& sceneName, std::string& written,
                         std::string& reason);
    // Missing の説明の文（日本語 1 文。Missing でなければ空）。読込の注意と /scene の
    // terrainMissing が同じ文になる唯一の口。Unreadable は terrainUnreadableFile_ の
    // 名前、Missing は文書の名前（無ければ背番号）で呼ぶ。PHYSICS スレッドか
    // objectsMutex_ を持って呼ぶ。
    std::string terrainMissingText() const;
    // /scene のテレイン部分（hasTerrain / terrain / terrainDirty / terrainMissing /
    // terrainOpen / terrainFiles）。
    // hierarchyJson（objectsMutex_ を持った HTTP スレッド）から呼ぶ。
    nlohmann::json terrainJson() const;
    // assets/terrains/ を走査し直す（警告は LOGW）。
    void refreshTerrainFiles();

    // ---- edit.terrain の受け口（SceneTerrainEdit.cpp、すべて PHYSICS thread）--
    // 入口の applyTerrainOp / applyTerrainFileOp は「エディタの実装」の節。
    // 新規（平ら・既定の大きさ・今の位置と表面）。名前は base（空ならシーン名）から。
    void newTerrain(const std::string& base);
    // 名前のファイルに差し替える（名前は一覧の綴り）。一覧に無い・使えない
    // （読込と同じ判定）なら何も変えない。
    void openTerrain(const std::string& name);
    // ファイルの名前の変更（背番号は変えない）。今のテレインなら参照も変える。
    // 今のテレインの名前（未保存でファイルが無くても）へ別のテレインを改名する
    // のは「既にある」と同じエラー（次の保存でそのファイルを上書きしてしまう）。
    void renameTerrain(const std::string& from, const std::string& to);
    // ファイルを OS のごみ箱へ移す（TerrainLibrary::remove）。エディタモードだけ。
    // 今のテレイン（未保存でファイルが無くても）は消さない。
    void deleteTerrain(const std::string& name);

    // テレインの当たり判定（自前の接触。PhysicsWorld::setTerrain）を今の高さ
    // 格子で張り直す。テレインが無ければ接触を止めるだけ（ゴーストは
    // PhysicsWorld が持ち続ける）。PHYSICS thread（objectsMutex_ を持たずに呼ぶこと）。
    // 例外は投げない: 失敗は中で捕まえて LOGE とステータスに出し、当たり判定の
    // 無いテレイン（terrainPhysId_ が無効、高さ格子と見た目はそのまま）で続ける。
    void rebuildTerrainBody();
    // 上の本体（例外をそのまま投げる）。rebuildTerrainBody からだけ呼ぶ。
    void rebuildTerrainBodyUnguarded();
    // テレインの接触を止めて番号を手放す（番号は先に手放す）。例外は外へ
    // 出さない（LOGE）。
    void releaseTerrainContact();
    // テレインがあるのに形の記録（proxy）を持たない動くボディ
    // （PhysicsWorld::terrainContactShape が BoundingBox = 安全網の AABB で
    // 粗く当たる物）を LOGW 1 行で名前付きで知らせる。顔ぶれが前回と同じなら
    // 黙る。PHYSICS thread。
    void warnTerrainPassThrough();
    // テレインがあり、NSC で `<option combine>` が min 以外
    // なら、LOGW を 1 回出して日本語の注意書き（ステータスの後ろに足す文）を
    // 返す。それ以外・同じ合成方式で既に知らせた後は空文字列。force = 前回の
    // 記録を捨てて必ず知らせ直す（シーン読込）。PHYSICS thread。
    std::string terrainCombineCaveat(bool force);
    // RENDER thread: テレインの見た目を反映する。applyToRenderer が
    // objectsMutex_ の中で dirty と値を取り出し、**ロックを外してから**呼ぶ
    // （全チャンクのメッシュ化とアップロード・テクスチャの読込は重い）。
    // rebuildMesh: チャンクを作り直す（enabled = false なら外す）。
    // updateSurface: 材質だけ入れ直す。
    void syncTerrain(bool rebuildMesh, bool updateSurface, bool enabled,
                     const wizengine::editor::TerrainDesc& desc,
                     const std::shared_ptr<const wizengine::terrain::HeightField>& field);

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
    // 文書のジョイント番号 → PhysicsWorld のジョイント番号（作れなかった /
    // シミュレートしていないものは kInvalidJoint）。buildJoints が作り、
    // イベントの setMotor と計測（updateJointStats）が引く。物理スレッド専用。
    std::vector<std::size_t> jointPhysIds_;
    std::vector<JointStat> jointStats_;  // poseMutex_ の下
    // 文書のケーブル番号 → PhysicsWorld のケーブル番号（buildCables が作る）。
    std::vector<std::size_t> cablePhysIds_;
    // ケーブルの節点位置（3 個で 1 節点、ケーブル番号ごと）。snapshot() が
    // 書き、RENDER スレッドが読む。poseMutex_ の下。
    std::vector<std::vector<float>> latestCables_;
    std::vector<double> cableTension_;  // 同上（N）
    std::vector<double> modalHz_;       // 同上
    std::string backendNote_;           // 同上（"core: FEA cable" など）
    // RENDER スレッド専用: ケーブルごとの円柱スロット。
    struct CableRender {
        std::vector<std::size_t> renderIds;
        wizengine::editor::Color3 color;
        bool colorSet = false;
    };
    std::vector<CableRender> cableRender_;
    // メッシュアセットのカタログ（文書の <asset>）。構造は objectsMutex_。
    std::vector<MeshAsset> meshes_;
    // 地面と環境光（文書の <ground> / <environment>）。書くのは物理スレッド
    // （loadDocument / clear）、読むのは RENDER スレッド（dirty を見て反映）。
    // どちらも objectsMutex_ の下。物理の床の作り直しは物理スレッドが即座に
    // 行い（rebuildGroundBody）、見た目は RENDER スレッドがフレーム境界で
    // 追いつく（syncGround / 環境光は applyToRenderer 末尾のロック外）。
    wizengine::editor::GroundDesc ground_;
    wizengine::editor::EnvironmentDesc environment_;
    // 描画設定（文書の <visual>）。地面・環境光と同じ扱い: 書くのは物理
    // スレッド、適用は RENDER スレッド（applyToRenderer 末尾のロック外）。
    wizengine::editor::RenderDesc render_;
    bool groundDirty_ = true;  // 初回の applyToRenderer が見た目を作る
    bool envDirty_ = true;
    bool renderDirty_ = true;
    // テレイン（文書の <terrain>）。地面と同じ扱い: 書くのは物理スレッド
    // （publishTerrainField）、desc・有無・dirty は objectsMutex_ の下。描画は RENDER
    // スレッドが applyToRenderer で dirty を拾い、重い作業（全チャンクの
    // メッシュ化とアップロード・テクスチャの読込）は**ロックを外してから**
    // 行う（環境光と同じ）。
    wizengine::editor::TerrainDesc terrain_;
    bool hasTerrain_ = false;
    bool terrainMeshDirty_ = false;     // 形が変わった = チャンクを作り直す
    bool terrainSurfaceDirty_ = false;  // 表面（テクスチャ・色・材質）だけ
    // 高さ格子の実体。作り直すたびに新しい物へ差し替える（中身は書き換え
    // ない）ので、取り出した shared_ptr は他スレッドで読み続けてよい。
    // ポインタの差し替えと取り出しだけを terrainFieldMutex_（末端のロック:
    // これを持ったまま他のロックを取らない）が守る - 車輪のレイが物理
    // スレッドから毎ステップ引くので、objectsMutex_ とは分けてある。
    std::shared_ptr<const wizengine::terrain::HeightField> terrainField_;
    // テレインのフレーム（terrain_ の pos + euler。格子は原点基準）。格子と
    // 組で読むので同じ terrainFieldMutex_ の下。書くのは publishTerrainField。
    BodyTransform terrainPose_{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0};
    // フレームが変わった = 描画のチャンクの変換を当て直す（objectsMutex_ の下。
    // 安いのでロックの中で当てる）。地面の置き場所も同じ扱い。
    bool terrainPoseDirty_ = true;
    bool groundPoseDirty_ = true;
    mutable std::mutex terrainFieldMutex_;
    // テレインの接触の相手（ゴースト）の番号。無い・張れなかったら kInvalidId。
    // ゴーストは PhysicsWorld の持ち物で、張り直しでも同じ番号が返る。
    // 物理スレッド専用。
    std::size_t terrainPhysId_ = PhysicsWorld::kInvalidId;
    // テレインの 2 つの警告の「前回知らせた内容」（繰り返さないため）。物理スレッド専用。
    std::string terrainPassThroughWarned_;
    std::string terrainCombineWarned_;
    // 系が自前の接触を扱えず張れなかった（警告済み）。この間 publish は張り直さない。
    // 張れたとき・releaseTerrainContact（外す・例外）で戻す。
    bool terrainBackendWarned_ = false;
    // 直前の読込の注意（terrainLoadCaveat が返して消す）。物理スレッド専用。
    std::string terrainLoadCaveat_;
    // 今のテレインと .terrain の関係（Keep 以外。terrainDirty() / terrainMissing()
    // で読む）。書くのは物理スレッドの publishTerrainField（と保存の成功で Saved に
    // 戻す saveTerrainData）だけ、/scene（HTTP）も読むので atomic。
    std::atomic<TerrainFileState> terrainFile_{TerrainFileState::Saved};
    // Unreadable のとき、実際に読めなかったファイルの名前（背番号で文書と違う名前に
    // 解決したときも、その名前）。terrain_ は文書の値のまま保つので別に持つ。
    // 書くのは物理スレッドの 2 か所（どちらも objectsMutex_ の下）: loadTerrainField
    // （読めなかったとき）と renameTerrain（そのファイルの名前が変わったとき）。
    // 意味があるのは Unreadable の間だけ（terrainMissingText が読む）。
    std::string terrainUnreadableFile_;
    // assets/terrains/ の窓口（一覧・解決・改名・採番）と編集の取り消しの記録。
    // どちらも Scene が持つ（コンストラクタで作る）。
    std::unique_ptr<wizengine::terrain::TerrainLibrary> terrainLibrary_;
    std::unique_ptr<wizengine::terrain::TerrainUndoStack> terrainUndo_;
    std::mutex objectsMutex_;  // boxes_ の構造を変えるときだけ取る
    std::mutex poseMutex_;
    std::vector<BodyTransform> latestPoses_;  // one per box, in box order
    // ソフトボディの粒子位置（3 個で 1 粒子、オブジェクト番号ごと。剛体は
    // 空）。latestPoses_ と同じく snapshot() が書き、RENDER スレッドが
    // 表面メッシュを組むのに読む。poseMutex_ の下。
    std::vector<std::vector<float>> latestSoft_;
    // RENDER スレッド専用の作業領域（毎フレームの再確保を避ける）。
    std::vector<float> softVerts_, softNormals_;

    // ---- 描画の補間（物理 30 Hz と描画 60 fps の間を埋める）----------------
    // 物理は描画とは別の時計の固定ステップなので、最新の姿勢をそのまま描くと
    // 1 フレームに 0 / 1 / 2 ステップ進むフレームが不規則に混ざり、動く物
    // （とそれを追うカメラ）がガタつく。物理パスの終わりの姿勢を 2 つ持ち、
    // 描画はその間を「1 パスぶん遅れて」補間する。
    struct PoseFrame {
        std::vector<BodyTransform> poses;
        std::vector<std::vector<float>> soft;    // latestSoft_ と同じ並び
        std::vector<std::vector<float>> cables;  // latestCables_ と同じ並び
        double simTime = 0.0;   // このフレームのシミュレーション時刻 (s)
        double wallTime = 0.0;  // 公開した実時間（steady_clock の秒）
        std::uint64_t cut = 0;  // 作ったときの poseCut_
        bool valid = false;
    };
    PoseFrame framePrev_, frameCur_;  // poseMutex_
    // snapshot(cut = true) のたびに進む。2 つのフレームの番号が違えば、間で
    // 置き直しなどがあった = 補間しない。poseMutex_ の下。
    std::uint64_t poseCut_ = 0;
    double simClock_ = 0.0;  // PHYSICS: stepPhysics が進めるシミュレーション時刻
    // RENDER スレッド専用: このフレームに描く姿勢（buildRenderFrame が作る）。
    std::vector<BodyTransform> renderPoses_;
    std::vector<std::vector<float>> renderSoft_;
    std::vector<std::vector<float>> renderCables_;
    double renderAlpha_ = -1.0;

    // ---- カメラのアクション（CameraLookAt / Behind / Orbit）---------------
    // イベントの「そのステップのカメラへの注文」。物理スレッドがノードの発火で
    // 書き（graphRt_.rigs → 毎ステップ cameraRigs_ へ）、RENDER スレッドが
    // applyToRenderer の中で、描画に使うのと同じ補間した姿勢からカメラを
    // 動かす（物理スレッドで動かすと、描画が別の物理ステップの物の姿勢と
    // 組み合わせて、追わせた物が画面で震える）。項目ごとに注文の時刻を持ち、
    // kCameraHoldSeconds 来なければその項目は手放す。
    struct CameraRig {
        double lookStamp = -1.0, behindStamp = -1.0, orbitStamp = -1.0;  // 実時間 s
        int lookObject = -1;
        double lookHeight = 0.0;
        int behindObject = -1;
        double behindSmooth = 0.0, behindAngleDeg = 0.0;
        double distance = 0.0, elevationDeg = 0.0;  // 0 = マウス操作のまま
    };
    std::vector<CameraRig> cameraRigs_;   // poseMutex_
    // 実行状態を捨てるたびに進む（poseMutex_）。RENDER はこれが変わったら
    // カメラを掛かる前の視点へ戻す（Reset・停止・読込）。
    std::uint64_t rigGeneration_ = 0;

    // ---- イベントグラフの実行状態（PHYSICS スレッド専用）------------------
    // グラフ本体は EditorState が持ち、ここにあるのは実行のためのキャッシュと
    // 走らせている最中にだけ意味を持つ値。resetGraphRuntime が全部捨てる。
    struct GraphRuntime {
        // 取り込んだグラフの版。EditorState::graphVersion と食い違ったら
        // 一覧をコピーし直す（毎ステップのロックを避けるための番号）。
        std::uint64_t version = ~std::uint64_t(0);
        std::vector<wizengine::editor::EventAssetDesc> assets;
        // 実行の単位 =「付いているアセット 1 個」。同じアセットを複数の
        // オブジェクトに付ければ、その数だけ実体ができる。
        struct Instance {
            std::size_t asset = 0;  // assets の位置
            int owner = -1;         // オブジェクト番号（-1 = ワールド）
        };
        std::vector<Instance> instances;
        // 実体ごとの実行状態。キーは（アセット名, owner）で、一覧を作り
        // 直しても残る - 編集のたびにタイマーが振り出しに戻らないように、
        // 位置ではなく名前で引く（ノードを id で引くのと同じ理由）。
        struct InstanceState {
            // OnTimer の経過秒（ノード id で引く）。
            std::map<int, double> timers;
        };
        std::map<std::pair<std::string, int>, InstanceState> state;
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
        // 直前のステップで破断したジョイント（文書の番号）。OnJointBreak が
        // 見て、そのパスの終わりで空にする。
        std::vector<int> brokenJoints;
        // カメラのアクションの注文（カメラ番号ごと、PHYSICS 専用）。毎ステップ
        // の終わりに cameraRigs_ へ写す。
        std::vector<CameraRig> rigs;
    };
    GraphRuntime graphRt_;
    // ステップの注文を cameraRigs_ へ写す（PHYSICS）。
    void publishCameraRigs();
    // RENDER スレッド専用: カメラごとの状態と、掛かる前のカメラの姿勢（止め
    // たらそこへ戻す = エディタの視点に戻る。姿勢が desc へ戻るのと同じ）。
    struct CameraRigState {
        bool engaged = false;
        std::uint64_t generation = 0;  // 掛けたときの rigGeneration_
        int behindObject = -1;         // 前フレームに後ろへ回った相手
        double lastTime = 0.0;         // steady_clock の秒
        double azimuth = 0.0, elevation = 0.0, radius = 0.0;
        double tx = 0.0, ty = 0.0, tz = 0.0;
    };
    std::vector<CameraRigState> rigState_;
    // 注文をこのフレームの姿勢（renderPoses_）でカメラへ当てる（RENDER）。
    void updateCameraRigs(const std::vector<BodyTransform>& poses,
                          const std::vector<CameraRig>& rigs,
                          std::uint64_t generation);

    // ---- マウスの掴み（PHYSICS スレッド）----------------------------------
    // 掴んだ時点のカメラからの奥行き。掴み直すたびに測り直す（held）ので、
    // 選択が残ったままカメラを回しても破綻しない。
    struct GrabDepth {
        bool valid = false;
        bool held = false;
        std::size_t sel = BoxController::kNone;
        double z = 0.0;
    };
    std::vector<GrabDepth> grabDepths_;
    // 引っぱり線（物理スレッドが書き、描画スレッドが読む）。
    struct GrabLine {
        std::atomic<bool> on{false};
        std::atomic<double> ax{0.0}, ay{0.0}, az{0.0};  // 物
        std::atomic<double> bx{0.0}, by{0.0}, bz{0.0};  // カーソルの指す点
    };
    std::vector<std::unique_ptr<GrabLine>> grabLines_;
    // カメラ台数ぶんの掴み用の配列を用意する（最初の 1 回）。
    void ensureGrabState();
};
