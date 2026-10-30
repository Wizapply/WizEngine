// Renderer のテレインの表面。層のテクスチャ配列（色・法線・マスク）・塗り分けの
// 重み（スプラット）・材質パラメータを Filament へ入れる。チャンクのメッシュと
// エンティティは RendererTerrain.cpp。
//
// 表面は層のテクスチャ配列 3 本（4 層）と塗り分けの重み（スプラット）を terrain.mat
// が混ぜる。重みの中身（塗り方・正規化）は知らない: 呼び出し側が矩形の
// テクセルを setTerrainSplat で渡す。
#include "render/RendererInternal.h"
#include "terrain/HeightField.h"  // kDefaultSplatTexel（既定の重みの唯一の定義）
#include "terrain/TerrainFile.h"  // layerImageName（層の画像の欄の英語名）

#include <optional>

using namespace filament;
using namespace filament::math;
using namespace render_detail;

namespace {

using Texel = std::array<uint8_t, 4>;

// 画像の無い層の単色（配列ごとの「画像無し」の唯一の定義。decodeTextureRGBA は
// None で代わりの絵を作らない）。
//   色     … 白（255 は sRGB でもリニアでも 1.0 = baseColor がそのまま色）
//   法線   … (0, 0, 1) = 平ら（128 / 255 * 2 - 1 ≒ 0。法線マップの無い層は
//            hasNormal = 0 でシェーダが正確な (0, 0, 1) を使うので見た目には出ない）
//   マスク … 遮蔽なし・粗さ 1・金属 0。マスクの無い層は hasMask = 0 で数値に
//            倒れるので見た目には出ない（念のため中立の値を入れておく）
constexpr Texel kWhiteTexel{255, 255, 255, 255};
constexpr Texel kFlatNormalTexel{128, 128, 255, 255};
constexpr Texel kNeutralMaskTexel{255, 255, 0, 255};

// rgba 1 色で埋めた size 四方の画素（層の単色と、既定のスプラットが使う）。
std::vector<uint8_t> solidPixels(int size, const Texel& rgba) {
    const std::size_t texels = std::size_t(size) * std::size_t(size);
    std::vector<uint8_t> pixels(texels * 4);
    for (std::size_t k = 0; k < texels; ++k) {
        std::copy(rgba.begin(), rgba.end(), pixels.begin() + std::ptrdiff_t(k * 4));
    }
    return pixels;
}

// 層の配列 1 本（kTerrainLayerTexSize 四方 × kLayers 層、ミップ込み）を作り、
// 全層を solid で埋める。format は画像の色空間（色 = SRGB8_A8、法線・マスク =
// RGBA8 = リニア）で、配列は全層が同じ形式。
filament::Texture* createLayerArray(filament::Engine& engine,
                                    filament::Texture::InternalFormat format,
                                    const Texel& solid) {
    const int size = wizengine::Renderer::kTerrainLayerTexSize;
    filament::Texture* array =
        filament::Texture::Builder()
            .width(uint32_t(size))
            .height(uint32_t(size))
            .depth(uint32_t(wizengine::TerrainSurface::kLayers))
            .levels(mipLevelCount(size, size))
            .format(format)
            .sampler(filament::Texture::Sampler::SAMPLER_2D_ARRAY)
            .build(engine);
    for (int i = 0; i < wizengine::TerrainSurface::kLayers; ++i) {
        uploadMipChain(engine, array, uint32_t(i), solidPixels(size, solid), size, size);
    }
    return array;
}

// 配列 array の層 layer へ path の画像を入れる（読めない・空なら solid）。
// 違う大きさ・縦横比の画像は kTerrainLayerTexSize 四方へ引き伸ばす（配列は全層が
// 同じ大きさ）。kind はログの画像の種類（terrain::layerImageName）。
// 戻り値 = ファイルから読めたか。
bool uploadLayerImage(filament::Engine& engine, filament::Texture* array, int layer,
                      const std::string& path, const char* kind, const Texel& solid) {
    char label[64];
    std::snprintf(label, sizeof(label), "terrain layer %d (%s)", layer, kind);
    std::vector<uint8_t> pixels;
    int w = 0;
    int h = 0;
    const bool fromFile = decodeTextureRGBA(path, label, TextureFallback::None, pixels, w, h);
    const int size = wizengine::Renderer::kTerrainLayerTexSize;
    if (fromFile && (w != size || h != size)) {
        LOGI("render", "%s texture resized from %dx%d to %dx%d%s", label, w, h, size, size,
             w != h ? " (not square - stretched)" : "");
    }
    // 同じ大きさの画像はそのまま渡す。
    std::vector<uint8_t> resized;
    if (!fromFile) {
        resized = solidPixels(size, solid);
    } else if (w == size && h == size) {
        resized = std::move(pixels);
    } else {
        resizeRGBA(pixels.data(), w, h, size, size, resized);
    }
    uploadMipChain(engine, array, uint32_t(layer), std::move(resized), size, size);
    return fromFile;
}

}  // namespace

namespace wizengine {

void Renderer::createTerrainTextures() {
    // 層の配列 3 本: 全層を既定の単色で埋める（= 画像無しの層）。色はファイルの
    // 画像（sRGB）に合わせて SRGB8_A8、法線とマスクはデータなのでリニアの RGBA8
    // （sRGB として読むと 0.5 が約 0.21 になり、法線が傾く）。
    terrainLayerTextures_ =
        createLayerArray(*engine_, filament::Texture::InternalFormat::SRGB8_A8, kWhiteTexel);
    terrainNormalTextures_ =
        createLayerArray(*engine_, filament::Texture::InternalFormat::RGBA8, kFlatNormalTexel);
    terrainMaskTextures_ =
        createLayerArray(*engine_, filament::Texture::InternalFormat::RGBA8, kNeutralMaskTexel);
    for (TerrainLayerImages& images : terrainLayerImages_) images = TerrainLayerImages{};
    // タイルさせる（REPEAT）。斜めから見る面なので異方性を効かせる。3 本とも同じ
    // タイルで引くので同じサンプラー。
    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    sampler.setAnisotropy(8.0f);
    terrainMatInstance_->setParameter("albedo", terrainLayerTextures_, sampler);
    terrainMatInstance_->setParameter("normalMaps", terrainNormalTextures_, sampler);
    terrainMatInstance_->setParameter("masks", terrainMaskTextures_, sampler);
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
        const std::vector<uint8_t> texelsRgba =
            solidPixels(resolution, terrain::kDefaultSplatTexel);
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
    if (!terrainMatInstance_ || !terrainLayerTextures_ || !terrainNormalTextures_ ||
        !terrainMaskTextures_) {
        return;
    }

    // 層の画像はパスが変わったものだけ読み直す（色や粗さのスライダー・タイルを
    // 動かすたびに画像をデコードしない）。配列の中身の入れ替えなので、
    // マテリアルへの付け替えは要らない。層の数値は float4（成分 = 層）で渡し、
    // シェーダには分岐を置かない。
    using terrain::LayerImage;
    float4 uvScale{1.0f};
    float4 roughness{0.0f};
    float4 metallic{0.0f};
    float4 normalScale{0.0f};
    float4 hasNormal{0.0f};
    float4 hasMask{0.0f};
    for (int i = 0; i < TerrainSurface::kLayers; ++i) {
        const TerrainSurface::Layer& layer = surface.layers[i];
        TerrainLayerImages& loaded = terrainLayerImages_[i];
        // パスが前回と違えば読み直して「ファイルから読めたか」を返す。同じなら
        // 読まずに空を返す（前回の結果のまま）。
        auto refresh = [&](filament::Texture* array, const std::string& want,
                           std::string& have, LayerImage image,
                           const Texel& solid) -> std::optional<bool> {
            if (want == have) return std::nullopt;
            have = want;
            return uploadLayerImage(*engine_, array, i, want, terrain::layerImageName(image),
                                    solid);
        };
        // 画像の種類（terrain::kLayerImages の順）ごとの配列と既定の単色。
        refresh(terrainLayerTextures_, layer.texture, loaded.texture, LayerImage::Color,
                kWhiteTexel);
        if (const std::optional<bool> ok = refresh(terrainNormalTextures_, layer.normal,
                                                   loaded.normal, LayerImage::Normal,
                                                   kFlatNormalTexel)) {
            loaded.hasNormal = *ok;
        }
        if (const std::optional<bool> ok = refresh(terrainMaskTextures_, layer.mask,
                                                   loaded.mask, LayerImage::Mask,
                                                   kNeutralMaskTexel)) {
            loaded.hasMask = *ok;
        }
        // UV はメートル単位なので、1 / タイルのメートル数でタイルする（3 枚とも同じ）。
        uvScale[i] = (layer.tileMeters > 1e-4f) ? 1.0f / layer.tileMeters : 1.0f;
        // 数値は呼ぶ側で検査済み（TerrainSurface::Layer の説明）。ここでは範囲を決めない。
        roughness[i] = layer.roughness;
        metallic[i] = layer.metallic;
        normalScale[i] = layer.normalScale;
        // 法線マップが読めた層だけマップの法線を使う（無い層は正確に平ら = (0, 0, 1)。
        // 既定の単色 128 は 0 ちょうどにならず、少し傾くため）。
        hasNormal[i] = loaded.hasNormal ? 1.0f : 0.0f;
        // マスクが読めた層は粗さ・金属をマスクから（数値は使わない）。
        hasMask[i] = loaded.hasMask ? 1.0f : 0.0f;
    }
    terrainMatInstance_->setParameter("uvScale", uvScale);
    terrainMatInstance_->setParameter("baseColor", RgbType::LINEAR, surface.color.xyz);
    terrainMatInstance_->setParameter("roughness", roughness);
    terrainMatInstance_->setParameter("metallic", metallic);
    terrainMatInstance_->setParameter("normalScale", normalScale);
    terrainMatInstance_->setParameter("hasNormal", hasNormal);
    terrainMatInstance_->setParameter("hasMask", hasMask);
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
