// Renderer の UI のテクスチャ（字の置き場 R8・画像・世代付きの番号・材質インスタンス）。
// Canvas のレンダラブルは RendererUiMeshes.cpp。
#include "render/RendererInternal.h"

#include <algorithm>
#include <cstddef>

#include "render/ImageScale.h"
#include "render/UiTextureId.h"

using namespace filament;
using namespace filament::math;
using namespace render_detail;

namespace wizengine {
namespace {

// UI のビューは後処理（sRGB への符号化を含む）を切ってあり、主のビューは
// 符号化した値を同じ描画先に書く。UI の色はリニア値（文書の約束）なので、
// sRGB へ直して書けば主のビューと同じ明るさになる（ui.mat の srgbOut）。
// 実機で色が明るすぎる・暗すぎるときはここを false にして見比べる。
constexpr bool kUiSrgbOut = true;

// UiTexture::instances の添字（字か・板か の組ごとに 1 つ）。
std::size_t uiInstanceSlot(bool sdf, bool plate) {
    return (sdf ? 1u : 0u) + (plate ? 2u : 0u);
}

// UI のテクスチャを作れる大きさか（端末の上限を超えると Filament が止まる = abort
// なので、作る前に断る）。字の置き場と画像の両方がここを通る。だめなら警告する
// （consequence = 代わりにどう描くか、英語）。
bool uiTextureSizeOk(Engine& engine, int width, int height, const char* consequence) {
    const std::size_t maxSize = Texture::getMaxTextureSize(engine, Texture::Sampler::SAMPLER_2D);
    const bool positive = width > 0 && height > 0;
    const bool fits = std::size_t(width) <= maxSize && std::size_t(height) <= maxSize;
    if (positive && fits) return true;
    LOGW("render", "ui texture %dx%d cannot be created (limit %zu) - %s", width, height, maxSize,
         consequence);
    return false;
}

}  // namespace

void Renderer::loadUiMaterial() {
    uiMaterial_ = loadMaterialPackage(*engine_, "ui.filamat", "ui material");
}

// ---- 番号 → 実体 ---------------------------------------------------------------
Renderer::UiTexture* Renderer::uiTextureAt(int textureId) {
    if (textureId < 0) return nullptr;
    const std::size_t slot = std::size_t(uiTextureSlot(textureId));
    if (slot >= uiTextures_.size()) return nullptr;
    UiTexture& t = uiTextures_[slot];
    // 返した後に枠が使い直されていれば世代が違う = 古い番号は別のテクスチャを指さない。
    const bool current = t.used && t.generation == uiTextureGeneration(textureId);
    return current ? &t : nullptr;
}

// ---- テクスチャ ----------------------------------------------------------------
Renderer::UiTexture& Renderer::uiTextureOrWhite(int textureId) {
    if (UiTexture* t = uiTextureAt(textureId)) return *t;
    if (textureId != ui::kUiNoTexture && !uiStaleTextureWarned_) {
        uiStaleTextureWarned_ = true;
        LOGW("render", "ui mesh refers to texture %d which no longer exists - drawn white",
             textureId);
    }
    if (!uiWhite_.texture) {
        uiWhite_.texture = createTextureRGBA(*engine_, {255, 255, 255, 255}, 1, 1, false);
        uiWhite_.width = uiWhite_.height = 1;
        uiWhite_.mipmapped = true;  // 1 段だけ = ミップ付きの引き方でも同じ
        uiWhite_.used = true;
    }
    return uiWhite_;
}

MaterialInstance* Renderer::uiInstance(int textureId, int materialId,
                                       UiCanvasPlacement placement) {
    UiTexture& t = uiTextureOrWhite(textureId);
    const bool sdf = materialId == ui::kUiMaterialSdf;
    const bool plate = placement == UiCanvasPlacement::Plate;
    MaterialInstance*& mi = t.instances[uiInstanceSlot(sdf, plate)];
    if (!mi) {
        mi = uiMaterial_->createInstance();
        const TextureSampler sampler(t.mipmapped ? TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR
                                                 : TextureSampler::MinFilter::LINEAR,
                                     TextureSampler::MagFilter::LINEAR);
        mi->setParameter("tex", t.texture, sampler);
        mi->setParameter("sdf", sdf);
        // 板は主のビューに描く = 後処理の有無に合わせる（uiPlateSrgbOut）。
        mi->setParameter("srgbOut", plate ? uiPlateSrgbOut() : kUiSrgbOut);
        // 板は手前の物に隠れる（深さで比べる。書かないのは材質の既定のまま）。
        if (plate) mi->setDepthCulling(true);
        // 輪郭の値（ui/UiSdfParams.h の kSdfOnEdge、0..255 → 0..1）。
        mi->setParameter("onEdge", float(ui::kSdfOnEdge) / 255.0f);
    }
    return mi;
}

bool Renderer::uiPlateSrgbOut() const {
    // 主のビューの後処理は最後に sRGB へ符号化する。切ってあれば誰も直さないので、
    // 板が自分で直す（overlay の UI のビューと同じ = 色が揃う）。
    return !settings_.postProcess;
}

void Renderer::refreshUiPlateSrgbOut() {
    // 作り置きの板の材質インスタンス（画像用と字用）へ当て直す。
    const bool srgbOut = uiPlateSrgbOut();
    auto apply = [srgbOut](UiTexture& t) {
        for (const bool sdf : {false, true}) {
            MaterialInstance* mi = t.instances[uiInstanceSlot(sdf, true)];
            if (mi) mi->setParameter("srgbOut", srgbOut);
        }
    };
    for (auto& t : uiTextures_) apply(t);
    apply(uiWhite_);
}

int Renderer::addUiTextureR8(int width, int height) {
    if (!uiTextureSizeOk(*engine_, width, height, "text is drawn as filled boxes")) {
        return ui::kUiNoTexture;
    }
    Texture* texture = Texture::Builder()
                           .width(uint32_t(width))
                           .height(uint32_t(height))
                           .levels(1)
                           .format(Texture::InternalFormat::R8)
                           .sampler(Texture::Sampler::SAMPLER_2D)
                           .build(*engine_);
    if (!texture) {
        LOGW("render", "ui texture %dx%d could not be created - text is drawn as filled boxes",
             width, height);
        return ui::kUiNoTexture;
    }
    // 中身を 0（= 輪郭の外）で入れておく（未初期化のテクスチャを引かせない）。
    std::vector<uint8_t> zeros(std::size_t(width) * std::size_t(height), 0);
    texture->setImage(*engine_, 0, moveToPixelDescriptor(std::move(zeros), Texture::Format::R));
    return adoptUiTexture(texture, width, height, false);
}

void Renderer::updateUiTexture(int textureId, int x, int y, int w, int h,
                               const uint8_t* pixels, int rowStride) {
    const UiTexture* t = uiTextureAt(textureId);
    if (!t || !pixels) return;
    const bool inside = x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= t->width &&
                        y + h <= t->height && rowStride >= x + w;
    if (!inside) return;
    // 矩形の行だけを詰めて写す（Filament は非同期に読むので、呼び手の配列は渡せない）。
    std::vector<uint8_t> rect(std::size_t(w) * std::size_t(h));
    for (int row = 0; row < h; ++row) {
        std::memcpy(rect.data() + std::size_t(row) * std::size_t(w),
                    pixels + std::size_t(y + row) * std::size_t(rowStride) + std::size_t(x),
                    std::size_t(w));
    }
    // 行 y はデータの y 行目（ui.mat の flipUV : false と同じ向き = v = 行 / 高さ）。
    t->texture->setImage(*engine_, 0, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h),
                         moveToPixelDescriptor(std::move(rect), Texture::Format::R));
}

int Renderer::loadUiImage(const std::string& path) {
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    // 読めなければ decodeTextureRGBA が警告する。代わりの絵は作らない（白で描く）。
    if (!decodeTextureRGBA(path, "ui image", TextureFallback::None, pixels, w, h)) {
        return ui::kUiNoTexture;
    }
    if (!uiTextureSizeOk(*engine_, w, h, "the image is drawn as a white panel")) {
        return ui::kUiNoTexture;
    }
    premultiplySrgbRGBA(pixels);  // 縁の透明な画素の色がにじまないように
    Texture* texture =
        createTextureRGBA(*engine_, std::move(pixels), w, h, true, MipFilter::PremultipliedSrgb);
    return adoptUiTexture(texture, w, h, true);
}

int Renderer::adoptUiTexture(Texture* texture, int width, int height, bool mipmapped) {
    const int slot = firstFreeSlot(uiTextures_);
    if (slot >= kUiTextureSlots) {  // 番号に入らない（同時に 65536 枚 = 呼び手の誤り）
        uiTextures_.pop_back();
        engine_->destroy(texture);
        LOGW("render", "too many ui textures - this one is not created");
        return ui::kUiNoTexture;
    }
    UiTexture& t = uiTextures_[std::size_t(slot)];
    const int generation = nextUiTextureGeneration(t.generation);
    t = UiTexture{};
    t.texture = texture;
    t.width = width;
    t.height = height;
    t.mipmapped = mipmapped;
    t.generation = generation;
    t.used = true;
    return uiTextureId(slot, generation);
}

void Renderer::releaseUiTexture(int textureId) {
    UiTexture* t = uiTextureAt(textureId);
    if (!t) return;
    // 使っている Canvas × ビューは描くのをやめる（次の setUiCanvasMesh で作り直す）。
    for (auto& canvas : uiCanvases_) {
        if (!canvas.used) continue;
        for (auto& part : canvas.parts) {
            bool uses = false;
            for (const auto& r : part.mesh.runs) {
                if (r.textureId == textureId) uses = true;
            }
            if (uses) destroyUiCanvasPart(part);
        }
    }
    destroyUiTexture(*t);
}

void Renderer::destroyUiTexture(UiTexture& t) {
    // 材質インスタンス（テクスチャを参照している）→ テクスチャ の順に壊す。
    for (auto*& mi : t.instances) {
        if (mi) engine_->destroy(mi);
    }
    if (t.texture) engine_->destroy(t.texture);
    const int generation = t.generation;  // 枠の世代は残す（次に使うときに進める）
    t = UiTexture{};
    t.generation = generation;
}

}  // namespace wizengine
