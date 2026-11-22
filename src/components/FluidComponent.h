#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "document/FluidTypes.h"
#include "fluid/FluidSurface.h"
#include "scene/SceneComponent.h"

namespace wizengine {
class Renderer;
}

// 流体の係（<worldbody> の <water> と <fluid>）。
//
//   PHYSICS : シミュレート中の毎ステップ、積分の前に、水面の下にある動く物へ
//             浮力・抗力・回転の抵抗を掛ける（fluid/WaterForces.h。力は重心へ、
//             モーメントは力の対で = PhysicsWorld::applyForceAtPoint。ソフトボディは
//             全粒子へ等しく = applyForce、モーメントは掛けない）。形は箱か球で
//             近似する（凸包のモデルは geom の半径の球、geom の無いフレームと三角
//             メッシュは対象外）。流体粒子（Multicore）は Chrono が解くのでここでは
//             何もしない。
//   RENDER  : 水面の格子（波で揺れる）と流体粒子を描く。流体粒子はシミュレート中は
//             Chrono の粒子（Scene::renderFluid = 補間済み）、エディタでは文書の配置
//             （fluid/FluidSeed.h = 開始したときと同じ並び）。描き方は 2 通り:
//               surface = true … 粒子をつないだ水の表面（fluid/FluidSurface = メタボール
//                                + Surface Nets）を、透けて屈折する水の材質
//                                （Renderer::addLiquidMesh = liquid.mat）で。表面は
//                                **別スレッド**（workerLoop）で組み、描画スレッドは
//                                「前の表面ができていれば今の粒子で次を頼む・できた表面を
//                                送る」だけ（待たない = 1〜2 フレーム遅れて追う）。
//               surface = false … 粒子ごとの玉（正八面体、不透明）。
class FluidComponent : public SceneComponent {
public:
    ~FluidComponent() override;
    void onPhysicsStep(Scene& scene, double dt) override;
    void onRender(Scene& scene) override;

private:
    // 表面の描き方（RENDER スレッド）。points は今回描く粒子（無ければ null）、live は
    // シミュレート中の粒子か（毎フレーム組み直す）。
    void renderSurface(wizengine::Renderer& r, const std::vector<float>* points, bool live,
                       bool restyle);
    // 表面を組むスレッドの本体。
    void workerLoop();

    // ---- PHYSICS スレッド専用 ----
    std::uint64_t physVersion_ = ~std::uint64_t(0);
    bool physHas_ = false;
    wizengine::editor::WaterDesc physWater_;

    // ---- RENDER スレッド専用 ----
    std::uint64_t waterVersion_ = ~std::uint64_t(0);
    bool waterHas_ = false;
    wizengine::editor::WaterDesc water_;
    std::size_t waterShape_ = std::size_t(-1);
    int waterCells_ = 0;
    double waterDrawnAt_ = -1.0;  // 最後に頂点を組んだ時刻（波が無ければ組み直さない）

    std::uint64_t fluidVersion_ = ~std::uint64_t(0);
    bool fluidHas_ = false;
    wizengine::editor::FluidDesc fluid_;
    std::vector<float> designPoints_;  // エディタで描く文書の配置
    bool designDirty_ = true;
    bool designUploaded_ = false;  // 文書の配置を今の形状へ送ったか
    std::size_t fluidShape_ = std::size_t(-1);
    std::size_t fluidShapeCount_ = 0;  // その形状が描ける粒子の数

    std::vector<float> pos_, nrm_;  // 作業領域

    // ---- 表面の描き方 ----
    // RENDER スレッド専用
    std::size_t liquidMesh_ = std::size_t(-1);
    bool liquidShown_ = false;
    bool designDirtySurface_ = true;    // 文書の配置の表面を組み直す
    bool lastLive_ = false;             // 前のフレームはシミュレート中の粒子だったか
    wizengine::fluid::SurfaceMesh uploadMesh_;
    double uploadThickness_ = 0.0;
    double styleThickness_ = 0.1;
    // 仕事の受け渡し（jobMutex_ の下）
    std::thread worker_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    bool stop_ = false;
    bool jobPending_ = false;
    bool busy_ = false;
    std::vector<float> jobPoints_;
    wizengine::fluid::SurfaceParams jobParams_;
    std::uint64_t jobSerial_ = 0;
    std::uint64_t submitSerial_ = 0;
    std::uint64_t hiddenAtSerial_ = 0;  // これ以前に頼んだ表面は捨てる（隠した後に出さない）
    bool resultReady_ = false;
    wizengine::fluid::SurfaceMesh result_;
    double resultThickness_ = 0.0;
    std::uint64_t resultSerial_ = 0;
};
