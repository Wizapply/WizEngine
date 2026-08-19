#pragma once

// Engine-level defaults in one place: cameras, lights, ground, environment,
// physics solver, grab feel. scene.cpp reads these; nothing else includes
// this header.
//
// ここにあるのは「実行環境と起動時の既定値」だけ。**シーンの中身**
// （オブジェクトの配置・glTF モデルの割り当て・ジョイント・イベント）は
// このヘッダではなく assets/scenes/*.xml が持つ（CLAUDE.md の
// 「シーン文書（XML）」の章）。起動時に読む文書は下の kStartupScene。
//
// Everything here is a compile-time value or a small inline factory - editing
// a number and rebuilding is the whole workflow. Runtime-tunable values
// (solver iterations, substeps, ...) start from these and are then driven
// from the browser via PhysicsControlComponent.

#include <vector>

#include <math/vec3.h>

#include "BoxController.h"
#include "CameraObject.h"
#include "EditorTypes.h"     // AppMode, LightDesc
#include "PhysicsWorld.h"    // PhysicsBackend
#include "scene_math.h"      // eulerDegreesFromDirection (lightConfigs)

// ---- Contact material ----------------------------------------------------
// Rolling/spinning resistance - only meaningful for spheres, which otherwise
// roll across a flat floor forever. Raise if the fruit never settles.
// (Chrono only solves these when the solver runs in spinning mode, which
// PhysicsWorld enables for the multicore backend.)
constexpr float kRollingFriction = 0.005f;
constexpr float kSpinningFriction = 0.005f;

// Contact surface. Friction is the main dial for HOW MUCH SPIN appears: it is
// what turns sliding into rolling, so a higher value spins objects up faster
// on landing. Restitution is bounciness; keep it near 0 for stacks that settle.
constexpr float kFriction = 0.6f;
constexpr float kRestitution = 0.0f;

// Velocity damping, a stand-in for air resistance: velocity decays by
// exp(-k dt) each step, so k is "how many e-foldings per second". Angular
// damping is what actually stops a sphere that is rolling on the spot; raise
// both if things drift too long, lower them for a slippery, lively scene.
constexpr double kLinearDamping = 0.15;   // 1/s
constexpr double kAngularDamping = 0.60;  // 1/s

// 地面（物理の床・見える地面・テクスチャ）と環境光（HDR の IBL）はシーンの
// 一部になった: シーン文書の <worldbody> の <ground> / <environment> が持ち、
// 節を書かない文書は GroundDesc / EnvironmentDesc（EditorTypes.h）の既定値で
// 開く。ここには定数を置かない。

// ---- Cameras -------------------------------------------------------------
// One entry per camera. Each gets its own browser page on its own port
// (kHttpPort + index, see main.cpp), its own Filament view and its own video
// stream, all looking at the same scene. Add or remove entries freely; the
// first one is the default.
inline std::vector<CameraObject::Config> cameraConfigs() {
    CameraObject::Config a;  // default three-quarter view
    a.azimuth = 0.66;
    a.elevation = 0.34;
    a.radius = 12.0;
    a.targetX = 0.0;
    a.targetY = 1.0;
    a.targetZ = 0.0;

    CameraObject::Config b = a;  // opposite side, lower
    b.azimuth = 0.66 + scenemath::kPi;  // opposite side
    b.elevation = 0.18;
    b.radius = 8.0;

    CameraObject::Config c = a;  // near top-down
    c.elevation = 1.15;
    c.radius = 10.0;

    return {a, b, c};
}

// ---- カメラの上限 -----------------------------------------------------------
// ページ（HTTP のパス）と WebRTC の受け口は起動時にこの数だけ作られる固定
// プール（どちらも軽い。エンコーダのパイプラインは従来から視聴者が来た
// 時点で作られる）。**動画ビュー（スワップチェーン + 読み戻しバッファ、
// 1 枚あたり数 MB）は予備スロットぶんを起動時には作らず**、エディタで
// 「カメラを追加」した時点（またはそのページに視聴者が来た時点）で描画
// スレッドが生成する（main.cpp の「動画ビューの遅延生成」）。一度作った
// ビューは返さない: カメラの「削除」は一覧から消すだけで、次の「追加」が
// 同じスロットとビューを再利用する。cameraConfigs() のぶんが最初から有効。
//
// ここにあるのは**既定値**。スロット数は実行環境の割り当てなので、exe 引数
// `--max-cameras N`（1〜16）で起動時に上書きできる（main.cpp が解析して
// Scene のコンストラクタへ渡す。CPU コアの固定と同じ整理）。
constexpr std::size_t kMaxCameras = 5;

// ---- エディタカメラ --------------------------------------------------------
// エディタ操作（モード切替・配置・ギズモ・ジョイント設計・シーンの保存/読込）
// を受け付けるカメラ番号。ギズモが出るのもこのカメラの選択だけ。他のカメラは
// エディタ中も今までどおり見る・選ぶはできるが、シーンは書き換えられない。
// 「複数人で同じシーンを見ながら、編集するのは 1 人」という使い方の前提。
// 範囲外の番号を書いた場合は 0 に丸められる。
constexpr std::size_t kEditorCamera = 0;

// ---- Lights --------------------------------------------------------------
// One entry per light; add or remove entries freely. These are the direct
// lights of the scene (the ambient/IBL is separate - the scene document's
// <environment> owns it).
// ここにあるのは**初期構成**: シーン文書がライトを持たないとき（新規・
// 全消し・ライト無しの手書き XML）にこの 2 灯で開く。以後はブラウザの
// エディタで編集でき、シーンの保存/読込（<worldbody> の <light>）に含まれる。
//
// 向きは方向ベクトルで書き、エディタの語彙（オイラー角・ゼロ = 真下）へ
// ここで変換する。Intensity units: lux for Directional (sun ~100k), lumens
// for Point/Spot (a 60W-ish bulb ~800 lm - point lights need surprisingly
// large values to compete with a sun-lit scene).
inline std::vector<wizengine::editor::LightDesc> lightConfigs() {
    using wizengine::editor::LightDesc;
    using wizengine::editor::LightKind;
    auto directional = [](const char* name, float r, float g, float b,
                          double intensity, const scenemath::Vec3& direction,
                          bool shadows) {
        LightDesc d;
        d.name = name;
        d.kind = LightKind::Sun;
        d.color = {r, g, b};
        d.intensity = intensity;
        const scenemath::Vec3 e = scenemath::eulerDegreesFromDirection(direction);
        d.rotation = {e.x(), e.y(), e.z()};
        d.shadows = shadows;
        return d;
    };

    // warm main light (casts the shadows) + cool fill from the other side
    // (lifts shadowed ground out of pure black).
    return {directional("key", 1.0f, 0.97f, 0.92f, 70000.0,
                        scenemath::Vec3(-0.5, -1.0, -0.35), true),
            directional("fill", 0.85f, 0.90f, 1.0f, 40000.0,
                        scenemath::Vec3(0.6, -0.5, 0.45), false)};
}

// How strongly the grabbed object is washed towards white so it stands out.
// 0 = no change, 1 = strongly whitened. Works with opaque models.
constexpr float kSelectedWhiten = 0.7f;

// ---- Grab control --------------------------------------------------------
// Grab an object with the left mouse button and drag to push it in the
// camera's screen plane.
inline BoxController::Config boxControllerConfig() {
    BoxController::Config c;
    c.stiffness = 80.0;         // pull towards the mouse target (1/s^2)
    c.damping = 12.0;           // ~2*sqrt(stiffness) = no overshoot
    c.maxAcceleration = 60.0;   // m/s^2 cap (gravity is 9.81)
    return c;
}

// Highlight colour per camera (linear RGB). Reused cyclically when there are
// more cameras than entries.
inline const std::vector<filament::math::float3>& cameraColors() {
    static const std::vector<filament::math::float3> colors = {
        {0.20f, 0.65f, 0.95f},  // camera 0: blue
        {0.30f, 0.85f, 0.35f},  // camera 1: green
        {0.95f, 0.55f, 0.20f},  // camera 2: orange
        {0.85f, 0.30f, 0.75f},  // camera 3: magenta
        {0.95f, 0.85f, 0.25f},  // camera 4: yellow
        {0.35f, 0.85f, 0.85f},  // camera 5: cyan
        {0.95f, 0.35f, 0.35f},  // camera 6: red
    };
    return colors;
}

// ---- Streaming ----------------------------------------------------------
// Codec, bitrate and GPU colour conversion are ENGINE settings, not scene
// content: they live in main.cpp (kDefaultCodec & friends) and can be
// overridden at launch with --codec / --encoder, and per camera from the
// browser's System > Stream section.

// Physics backend. Multicore parallelises the solver and collision detection
// (much faster with thousands of bodies) but does not support sleeping. It
// needs a Chrono built with the MULTICORE module and CMake -DWIZ_USE_MULTICORE=ON;
// without that this falls back to Core automatically.
constexpr PhysicsBackend kBackend = PhysicsBackend::Multicore;

// In web mode, do nothing at all while no browser holds the viewer session:
// no physics steps, no rendering, no encoding. Set false to keep simulating in
// the background.
constexpr bool kIdleWhenUnwatched = true;

// ---- モード --------------------------------------------------------------
// 起動時にどちらのモードで立ち上げるか。Simulate は従来どおり、置いてある
// 物がいきなり落ちてくる状態。Editor は物理を止めた状態で始まるので、
// 配置やジョイントの設計から入りたいときはこちら（ブラウザからいつでも
// 切り替えられるので、好みの問題）。
constexpr wizengine::editor::AppMode kStartMode =
    wizengine::editor::AppMode::Simulate;

// 起動時に読み込むシーン文書（assets/scenes の名前。拡張子は書かない）。
// シーンの中身（配置・モデル・ジョイント・イベント）はコードではなく XML が
// 持つので、既定シーンもただのファイル（assets/scenes/default.xml、リポジトリ
// 同梱）。読めなければ警告を出して空のシーン（地面のみ）で起動する。
// 空文字列にすると常に空のシーンで起動する。
constexpr const char* kStartupScene = "default";

// 重力（-Y 方向、m/s^2）。エディタのシミュレート設定の初期値でもある。
constexpr double kGravityY = -9.81;

// Physics rate, independent of the 60 fps render loop. 30 Hz halves the
// physics cost; the renderer simply draws the latest pose twice.
constexpr int kPhysicsHz = 30;

constexpr int kSubsteps = 2;
constexpr int kSolverIterations = 60;

// Contact tolerances. Chrono's default envelope (0.03 m) is huge next to a
// 0.5 m box and creates far more contacts than necessary; shrinking it is one
// of the cheapest ways to speed up big scenes.
constexpr double kCollisionEnvelope = 0.002;  // metres
constexpr double kCollisionMargin = 0.002;    // metres

// Interpenetration control. Raise kContactRecovery if boxes visibly sink into
// each other (too high makes settled stacks pop); lower kSolverTolerance to
// make the solver converge harder at the cost of speed.
constexpr double kContactRecovery = 0.2;   // m/s
constexpr double kSolverTolerance = 1e-3;
// Chrono's per-body sleeping (ChBody::SetSleepingAllowed + thresholds).
// Settled bodies leave the solver entirely, so a resting pile costs almost
// nothing. Limits are deliberately tighter than
// Chrono's defaults (0.6 s / 0.1 m/s / 0.04 rad/s): loose limits let a box that
// slows down mid-fall sleep and hang in the air once its support moves away.
// Set kSleepingEnabled false if that ever shows up.
constexpr bool kSleepingEnabled = true;
constexpr float kSleepSeconds = 1.0f;
constexpr float kSleepMinLinVel = 0.02f;  // m/s
constexpr float kSleepMinAngVel = 0.02f;  // rad/s
