// Renderer のスクリーンショット（AI の見直し用）。
//
// カメラのビュー（ページごとの映像）とは別の、専用のビュー・カメラ・スワップ
// チェーンを 1 組だけ持ち、頼まれたときに 1 枚描いて読み戻す。
//   * views_ に入れない: main はビュー番号 = カメラのページで扱っているので、
//     番号をずらさない。映像にも映らない。
//   * 同期: 描いて readPixels を積み、flushAndWait で GPU の書き終わりまで待つ。
//     renderFrame の非同期の読み戻し（2 枚の入れ物を交互）は毎フレームの映像の
//     ため。こちらは生成の後に数枚撮るだけなので、1 フレームぶん止まっても困らない。
//   * 映すのはシーンのレイヤだけ（ギズモ = エディタ専用レイヤは見せない。UI の
//     ビューも描かない）。描画設定はカメラのビューと同じ（applyViewSettings）だが、
//     TAA は前のフレームを使う仕組みなので 1 枚きりでは切る。
#include "render/RendererInternal.h"

#include <cmath>

using namespace filament;
using namespace filament::math;
using utils::EntityManager;
using namespace render_detail;

namespace wizengine {

bool Renderer::captureSnapshot(const double3& eye, const double3& target, int width,
                               int height, std::vector<uint8_t>& rgba) {
    if (!engine_ || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return false;
    }
    ViewSlot& slot = snapshot_;
    if (!slot.view) {
        slot.cameraEntity = EntityManager::get().create();
        slot.camera = engine_->createCamera(slot.cameraEntity);
        slot.view = engine_->createView();
        slot.view->setScene(scene_);
        slot.view->setCamera(slot.camera);
        // エディタ専用レイヤ（ギズモ・ライト / カメラのアイコン・グリッド）は映さない。
        slot.view->setVisibleLayers(kLayerEditorOnly, 0);
    }
    if (!slot.swapChain || slot.width != width || slot.height != height) {
        if (slot.swapChain) engine_->destroy(slot.swapChain);
        slot.width = width;
        slot.height = height;
        slot.swapChain = engine_->createSwapChain(uint32_t(width), uint32_t(height),
                                                  SwapChain::CONFIG_READABLE);
        slot.view->setViewport({0, 0, uint32_t(width), uint32_t(height)});
    }
    // 描画設定はカメラのビューと同じ（撮るたびに当て直す = 後から変わった設定も効く）。
    applyViewSettings(slot);
    TemporalAntiAliasingOptions taa;
    taa.enabled = false;
    slot.view->setTemporalAntiAliasingOptions(taa);

    // カメラ（setCamera と同じ near の決め方 = 寄っても手前が切れない）。
    slot.camera->lookAt(eye, target, {0.0, 1.0, 0.0});
    double nearPlane = length(target - eye) * kNearPerDistance;
    if (nearPlane > kNearPlane) nearPlane = kNearPlane;
    if (nearPlane < kMinNearPlane) nearPlane = kMinNearPlane;
    slot.nearPlane = nearPlane;
    applyProjection(slot);

    // beginFrame はフレームを飛ばすべきとき false を返す（GPU が遅れている）。
    // 溜まった仕事を流してからもう一度だけ試す。
    bool began = renderer_->beginFrame(slot.swapChain);
    if (!began) {
        engine_->flushAndWait();
        began = renderer_->beginFrame(slot.swapChain);
    }
    if (!began) {
        LOGW("render", "snapshot: the renderer skipped the frame");
        return false;
    }
    renderer_->render(slot.view);

    // 入れ物と完了の旗はコールバックと共有する（renderFrame と同じ考え）。万一
    // flushAndWait の後も完了が来ていなくても、遅れて来た書き込みが解放済みの
    // メモリに触れない。
    struct Readback {
        std::vector<uint8_t> pixels;
        std::atomic<bool> ready{false};
    };
    auto rb = std::make_shared<Readback>();
    rb->pixels.assign(std::size_t(width) * std::size_t(height) * 4, 0);
    {
        using namespace filament::backend;
        auto* held = new std::shared_ptr<Readback>(rb);
        PixelBufferDescriptor pbd(
            rb->pixels.data(), rb->pixels.size(), PixelDataFormat::RGBA,
            PixelDataType::UBYTE,
            [](void* /*buffer*/, size_t /*size*/, void* user) {
                auto* h = static_cast<std::shared_ptr<Readback>*>(user);
                (*h)->ready.store(true, std::memory_order_release);
                delete h;
            },
            held);
        // Vulkan は上の行から返る（renderFrame と同じ。OpenGL では上下が逆になる）。
        renderer_->readPixels(0, 0, uint32_t(width), uint32_t(height), std::move(pbd));
    }
    renderer_->endFrame();
    engine_->flushAndWait();  // ここで GPU の書き込みと読み戻しのコールバックまで済む
    // 色作りの LUT は rebuildColorGrading が views_ のぶんだけ差し替えてから古い方を
    // 壊すので、このビューには持たせたままにしない（次に撮るとき当て直す）。
    slot.view->setColorGrading(nullptr);
    if (!rb->ready.load(std::memory_order_acquire)) {
        LOGW("render", "snapshot: readback did not complete");
        return false;
    }
    rgba = std::move(rb->pixels);
    return true;
}

void Renderer::destroySnapshot() {
    ViewSlot& slot = snapshot_;
    if (slot.view) {
        engine_->destroyCameraComponent(slot.cameraEntity);
        engine_->destroy(slot.cameraEntity);
        EntityManager::get().destroy(slot.cameraEntity);
        engine_->destroy(slot.view);
        slot.view = nullptr;
        slot.camera = nullptr;
    }
    if (slot.swapChain) {
        engine_->destroy(slot.swapChain);
        slot.swapChain = nullptr;
    }
}

}  // namespace wizengine
