// Renderer のテレインの表面。層のテクスチャ配列・塗り分けの重み（スプラット）・
// 材質パラメータを Filament へ入れる。チャンクのメッシュとエンティティは
// RendererTerrain.cpp。
//
// 表面は層のテクスチャ配列（4 層）と塗り分けの重み（スプラット）を terrain.mat
// が混ぜる。重みの中身（塗り方・正規化）は知らない: 呼び出し側が矩形の
// テクセルを setTerrainSplat で渡す。
#include "render/RendererInternal.h"
#include "terrain/HeightField.h"  // kDefaultSplatTexel（既定の重みの唯一の定義）

using namespace filament;
using namespace filament::math;
using namespace render_detail;

namespace {

// テクスチャの無い層の画素: 白で埋めた kTerrainLayerTexSize 四方（白 255 は
// sRGB でもリニアでも 1.0 = 色がそのまま出る）。「テクスチャの無い層は白」の
// 唯一の定義（decodeTextureRGBA は None で代わりの絵を作らない）。
std::vector<uint8_t> whiteLayerPixels(int size) {
    return std::vector<uint8_t>(std::size_t(size) * std::size_t(size) * 4, uint8_t(255));
}

}  // namespace

namespace wizengine {

void Renderer::createTerrainTextures() {
    // 層の配列: 全層を白で埋める（= テクスチャ無しの層。色がそのまま出る）。
    // 配列は全層が同じ大きさ・同じ形式なので、ファイルの画像（sRGB）に合わせて
    // SRGB8_A8 にする（白は whiteLayerPixels）。
    const int size = kTerrainLayerTexSize;
    terrainLayerTextures_ =
        filament::Texture::Builder()
            .width(uint32_t(size))
            .height(uint32_t(size))
            .depth(uint32_t(TerrainSurface::kLayers))
            .levels(mipLevelCount(size, size))
            .format(filament::Texture::InternalFormat::SRGB8_A8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D_ARRAY)
            .build(*engine_);
    for (int i = 0; i < TerrainSurface::kLayers; ++i) {
        uploadMipChain(*engine_, terrainLayerTextures_, uint32_t(i), whiteLayerPixels(size),
                       size, size);
        terrainLayerPaths_[i].clear();
    }
    // タイルさせる（REPEAT）。斜めから見る面なので異方性を効かせる。
    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    sampler.setAnisotropy(8.0f);
    terrainMatInstance_->setParameter("albedo", terrainLayerTextures_, sampler);
    // 重みは 1×1 の既定（層 0 だけ）= 塗っていないテレインは層 0 の絵。
    // 換算の幅は仮の 1 m（1×1 は CLAMP でどこを引いても同じテクセル）。
    recreateTerrainSplat(1, true);
    terrainMatInstance_->setParameter("splatInvSize", 1.0f);
}

void Renderer::recreateTerrainSplat(int resolution, bool fill) {
    filament::Texture* next =
        filament::Texture::Builder()
            .width(uint32_t(resolution))
            .height(uint32_t(resolution))
            .levels(1)
            .format(filament::Texture::InternalFormat::RGBA8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .build(*engine_);
    if (fill) {
        // 既定のテクセルは terrain::kDefaultSplatTexel（数字をここに書かない）。
        const std::size_t texels = std::size_t(resolution) * std::size_t(resolution);
        std::vector<uint8_t> texelsRgba(texels * 4);
        for (std::size_t k = 0; k < texels; ++k) {
            std::copy(terrain::kDefaultSplatTexel.begin(), terrain::kDefaultSplatTexel.end(),
                      texelsRgba.begin() + std::ptrdiff_t(k * 4));
        }
        next->setImage(*engine_, 0, copyToPixelDescriptor(texelsRgba.data(), texelsRgba.size()));
    }
    // 重みはセルの間で滑らかに混ぜる（LINEAR）。端は伸ばす（CLAMP = 反対側の
    // 縁の重みを混ぜない）。ミップは持たない（遠くでも境目の位置を保つ）。
    filament::TextureSampler sampler(filament::TextureSampler::MinFilter::LINEAR,
                                     filament::TextureSampler::MagFilter::LINEAR,
                                     filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    // 新しい方を付けてから古い方を壊す（地面のテクスチャと同じ順）。
    terrainMatInstance_->setParameter("splat", next, sampler);
    if (terrainSplat_) engine_->destroy(terrainSplat_);
    terrainSplat_ = next;
    terrainSplatRes_ = resolution;
}

void Renderer::setTerrainSurface(const TerrainSurface& surface) {
    if (!terrainMatInstance_ || !terrainLayerTextures_) return;

    // 層のテクスチャはパスが変わった層だけ読み直す（色や粗さのスライダー・
    // タイルを動かすたびに画像をデコードしない）。配列の中身の入れ替えなので、
    // マテリアルへの付け替えは要らない。
    float4 uvScale{1.0f};
    for (int i = 0; i < TerrainSurface::kLayers; ++i) {
        const TerrainSurface::Layer& layer = surface.layers[i];
        if (layer.texture != terrainLayerPaths_[i]) {
            char label[32];
            std::snprintf(label, sizeof(label), "terrain layer %d", i);
            std::vector<uint8_t> pixels;
            int w = 0;
            int h = 0;
            const bool fromFile = decodeTextureRGBA(layer.texture, label, TextureFallback::None,
                                                    pixels, w, h);
            // 配列は全層が同じ大きさ: 違う大きさ・縦横比の画像は引き伸ばす。
            const int size = kTerrainLayerTexSize;
            if (fromFile && (w != size || h != size)) {
                LOGI("render", "%s texture resized from %dx%d to %dx%d%s", label, w, h, size,
                     size, w != h ? " (not square - stretched)" : "");
            }
            // 読めない・空の層は白（whiteLayerPixels）。同じ大きさの画像はそのまま渡す。
            std::vector<uint8_t> resized;
            if (!fromFile) {
                resized = whiteLayerPixels(size);
            } else if (w == size && h == size) {
                resized = std::move(pixels);
            } else {
                resizeRGBA(pixels.data(), w, h, size, size, resized);
            }
            uploadMipChain(*engine_, terrainLayerTextures_, uint32_t(i), std::move(resized),
                           size, size);
            terrainLayerPaths_[i] = layer.texture;
        }
        // UV はメートル単位なので、1 / タイルのメートル数でタイルする。
        uvScale[i] = (layer.tileMeters > 1e-4f) ? 1.0f / layer.tileMeters : 1.0f;
    }
    terrainMatInstance_->setParameter("uvScale", uvScale);
    terrainMatInstance_->setParameter("baseColor", RgbType::LINEAR, surface.color.xyz);
    terrainMatInstance_->setParameter("roughness", std::clamp(surface.roughness, 0.0f, 1.0f));
    terrainMatInstance_->setParameter("metallic", std::clamp(surface.metallic, 0.0f, 1.0f));
}

bool Renderer::setTerrainSplat(int resolution, float sizeMeters, int x0, int z0, int w,
                               int h, const uint8_t* rgba) {
    if (!terrainMatInstance_) return false;
    if (resolution < 1 || !rgba || w < 1 || h < 1 || x0 < 0 || z0 < 0 ||
        x0 + w > resolution || z0 + h > resolution || !std::isfinite(sizeMeters) ||
        !(sizeMeters > 0.0f)) {
        LOGW("render",
             "setTerrainSplat: invalid rect (%d, %d) %dx%d for resolution %d, size %.3f m",
             x0, z0, w, h, resolution, double(sizeMeters));
        return false;
    }
    // 解像度が違う・未作成なら作り直す。矩形が全体なら直後に全部書くので埋めない。
    if (!terrainSplat_ || resolution != terrainSplatRes_) {
        const bool whole = x0 == 0 && z0 == 0 && w == resolution && h == resolution;
        recreateTerrainSplat(resolution, !whole);
    }
    // UV はローカルのメートル（格子は原点中心）なので、uv / size + 0.5 で 0〜1。
    terrainMatInstance_->setParameter("splatInvSize", 1.0f / sizeMeters);
    // 矩形だけの部分更新（行 = +Z がテクスチャの行）。
    const std::size_t bytes = std::size_t(w) * std::size_t(h) * 4;
    terrainSplat_->setImage(*engine_, 0, uint32_t(x0), uint32_t(z0), uint32_t(w),
                            uint32_t(h), copyToPixelDescriptor(rgba, bytes));
    return true;
}

}  // namespace wizengine
