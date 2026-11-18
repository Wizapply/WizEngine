// Renderer の UI のビュー（overlay の Canvas を重ねる、各ビューの 2 枚目のビュー）。
//
// 主のビューを描いた直後に、同じ描画先へこのビューを描く（renderFrame）。
//   * BlendMode::TRANSLUCENT: 描かなかった画素は透明のまま = 主のビューの絵が残り、
//     描いた画素は主のビューの絵の上に重なる（Filament の ClearOptions の説明。
//     消去は beginFrame の 1 回だけなので、2 枚目のビューは消さない）。
//   * 後処理なし: トーンマップ・ブルーム・色作り・FXAA が UI に掛からない
//     （View::setPostProcessingEnabled の説明が「UI の重ね描き用」と書く使い方）。
//     その代わり sRGB への符号化も無いので、ui.mat の srgbOut が受け持つ。
//   * 影なし・自分専用のシーン（主のシーンの物は映らない）。
//   * 直交投影: 「ビューの高さ = 1」の単位で、左上が原点・y 下向き（Q3）。
//     ORTHO(0, 縦横比, 1, 0)。Canvas ごとの 1 / 基準の高さ は Canvas の変換が持つ。
#include "render/RendererInternal.h"

using namespace filament;
using namespace filament::math;
using utils::EntityManager;
using namespace render_detail;

namespace wizengine {

void Renderer::ensureUiView(ViewSlot& slot) {
    if (slot.uiView) return;
    slot.uiScene = engine_->createScene();
    slot.uiCameraEntity = EntityManager::get().create();
    slot.uiCamera = engine_->createCamera(slot.uiCameraEntity);
    slot.uiView = engine_->createView();
    slot.uiView->setScene(slot.uiScene);
    slot.uiView->setCamera(slot.uiCamera);
    slot.uiView->setBlendMode(BlendMode::TRANSLUCENT);
    slot.uiView->setPostProcessingEnabled(false);
    slot.uiView->setShadowingEnabled(false);
    applyUiProjection(slot);
}

void Renderer::applyUiProjection(ViewSlot& slot) {
    if (!slot.uiView) return;
    slot.uiView->setViewport({0, 0, uint32_t(slot.width), uint32_t(slot.height)});
    const double aspect = slot.height > 0 ? double(slot.width) / double(slot.height) : 1.0;
    // 左 0・右 縦横比・下 1・上 0（y 下向き）。深さは使わない（ui.mat）。
    slot.uiCamera->setProjection(Camera::Projection::ORTHO, 0.0, aspect, 1.0, 0.0, -1.0,
                                 1.0);
}

int2 Renderer::viewSize(std::size_t viewIndex) const {
    if (viewIndex >= views_.size()) return {0, 0};
    return {views_[viewIndex].width, views_[viewIndex].height};
}

void Renderer::destroyUi() {
    for (auto& canvas : uiCanvases_) {
        for (auto& part : canvas.parts) destroyUiCanvasPart(part);
    }
    uiCanvases_.clear();
    for (auto& t : uiTextures_) destroyUiTexture(t);
    uiTextures_.clear();
    destroyUiTexture(uiWhite_);
    if (uiMaterial_) engine_->destroy(uiMaterial_);
    uiMaterial_ = nullptr;
    for (auto& slot : views_) {
        if (!slot.uiView) continue;
        engine_->destroy(slot.uiView);
        engine_->destroyCameraComponent(slot.uiCameraEntity);
        EntityManager::get().destroy(slot.uiCameraEntity);
        engine_->destroy(slot.uiScene);
        slot.uiView = nullptr;
        slot.uiCamera = nullptr;
        slot.uiScene = nullptr;
    }
}

}  // namespace wizengine
