#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include <math/vec3.h>
#include <nlohmann/json.hpp>

#include "EditorTypes.h"
#include "Renderer.h"  // BatchShape
#include "SceneComponent.h"

// 選択中のオブジェクトに出す Unity 風のギズモ（移動 / 回転 / 拡縮）。
//
// ---- なぜサーバー側で描くのか ----------------------------------------------
// ブラウザに届くのは映像なので、ギズモを HTML/SVG で重ねると、カメラを回した
// 瞬間に「オーバーレイだけ先に動いて映像が 1〜2 フレーム遅れて追いつく」ずれが
// 見える。ギズモをシーンの中の線として描けば映像と必ず同じフレームに乗るし、
// 手前の物に隠れる・複数カメラから同じように見える、という当たり前の挙動も
// ただで手に入る。グラブ線が既に同じ理由で 3D 幾何になっている。
//
// ---- スレッド ---------------------------------------------------------------
//   onCommand    : INPUT  … pick でハンドルを掴み、drag で位置を受け、hover を記録
//   onEditorStep : PHYSICS… 掴んでいるハンドルに応じて desc を書き換える
//   onRender     : RENDER … ギズモの線を組み立てて Renderer へ送る
// ハンドル番号とカーソル位置だけが atomic でスレッドをまたぐ。
class GizmoComponent : public SceneComponent {
public:
    GizmoComponent();
    ~GizmoComponent() override;

    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override;
    void onEditorStep(Scene& scene, double dt) override;
    void onRender(Scene& scene) override;

private:
    struct Cam;                          // 実装は .cpp（atomic を含むので）
    std::vector<std::unique_ptr<Cam>> cams_;
    void ensure(Scene& scene);

    // 描画のたびに組み直す図形の列（色バッチごと）。太線と塗りつぶしの面が
    // 混ざる。メンバに持つのは、毎フレーム vector を確保し直さないため。
    std::vector<std::vector<wizengine::BatchShape>> batches_;

    // グリッドの直近の表示状態。細線セットの頂点を作り直すのは、表示の
    // ON/OFF か間隔が変わったときだけ（RENDER スレッドのみが触る）。
    bool gridShown_ = false;
    double gridStep_ = 0.0;
};

namespace gizmo {

// 線分バッチ（太線・塗りつぶし）の色。0-2 が X/Y/Z 軸、3 が中立（灰）、
// 4 が掴んでいるハンドル。Renderer::configureLineBatches に渡す（Scene::build）。
const std::vector<filament::math::float3>& batchColors();
// 1 バッチに詰められる図形の数。いちばん食うのは回転リング（48 本）と
// 矢じりの円錐（24 面 ×3 軸）。グリッドはポリゴンではなく細線セットで
// 描くので、ここの容量には入らない。
inline constexpr std::size_t kMaxSegments = 256;

// Y=0 グリッド用の細線セット。番号は Scene::build が gridColors() の順に
// Renderer::addLineSet した並びと一致する。
inline constexpr std::size_t kGridSetGray = 0;  // 格子（灰）
inline constexpr std::size_t kGridSetX = 1;     // 原点を通る X 軸（赤）
inline constexpr std::size_t kGridSetZ = 2;     // 原点を通る Z 軸（青）
const std::vector<filament::math::float3>& gridColors();

}  // namespace gizmo
