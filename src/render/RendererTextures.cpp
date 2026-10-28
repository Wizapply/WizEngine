// Renderer のテクスチャ読込（地面とテレインで共用）: 画像のデコード・大きさの
// 揃え直し・ミップ列のアップロードを別々の関数にして、地面の 2D テクスチャと
// テレインの層のテクスチャ配列が同じ部品を通る。
#include "render/RendererInternal.h"
#include "render/ImageLoader.h"

using namespace filament;

namespace render_detail {

bool decodeTextureRGBA(const std::string& texturePath, const char* label,
                       TextureFallback fallback, std::vector<uint8_t>& pixels,
                       int& width, int& height) {
    // An image file (PNG/JPEG/TGA/BMP) when the scene names one. 読めない・
    // 無いときは代わりの絵に落として警告する（テクスチャはシーン文書の内容 =
    // 手で書けるので、glTF メッシュと同じく止めずに降格する）。Image
    // colours are sRGB; the fallbacks are linear multipliers on baseColor.
    pixels.clear();
    width = 0;
    height = 0;
    const bool fromFile =
        !texturePath.empty() &&
        loadImageRGBA(wizengine::assetPath(texturePath), pixels, width, height);
    if (fromFile) {
        LOGI("render", "%s texture: %s (%dx%d)", label,
             wizengine::assetPath(texturePath).c_str(), width, height);
    } else if (fallback == TextureFallback::Checker) {
        if (!texturePath.empty()) {
            LOGW("render", "%s texture '%s' not found, using generated checker",
                 label, texturePath.c_str());
        }
        width = height = 256;  // 2 x 2 squares, 128 px each
        pixels.resize(size_t(width) * size_t(height) * 4);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool dark = (x < width / 2) != (y < height / 2);
                const uint8_t v = dark ? 105 : 255;
                uint8_t* p = pixels.data() + (size_t(y) * width + size_t(x)) * 4;
                p[0] = v;
                p[1] = v;
                p[2] = v;
                p[3] = 255;
            }
        }
    } else {
        // 単色: 白 1 画素（baseColor がそのまま色になる）。
        if (!texturePath.empty()) {
            LOGW("render", "%s texture '%s' not found, using solid colour",
                 label, texturePath.c_str());
        }
        width = height = 1;
        pixels.assign(4, uint8_t(255));
    }
    return fromFile;
}

void resizeRGBA(const uint8_t* src, int srcW, int srcH, int dstW, int dstH,
                std::vector<uint8_t>& dst) {
    dst.resize(size_t(dstW) * size_t(dstH) * 4);
    // 双一次補間。出力の画素の中心を入力の座標へ写し、近い 4 画素を混ぜる
    // （端は端の画素を伸ばす）。大きさが同じなら写すだけ。
    if (srcW == dstW && srcH == dstH) {
        std::memcpy(dst.data(), src, dst.size());
        return;
    }
    const float sx = float(srcW) / float(dstW);
    const float sy = float(srcH) / float(dstH);
    for (int y = 0; y < dstH; ++y) {
        const float fy = std::clamp((float(y) + 0.5f) * sy - 0.5f, 0.0f, float(srcH - 1));
        const int y0 = int(fy);
        const int y1 = std::min(y0 + 1, srcH - 1);
        const float ty = fy - float(y0);
        for (int x = 0; x < dstW; ++x) {
            const float fx =
                std::clamp((float(x) + 0.5f) * sx - 0.5f, 0.0f, float(srcW - 1));
            const int x0 = int(fx);
            const int x1 = std::min(x0 + 1, srcW - 1);
            const float tx = fx - float(x0);
            const uint8_t* p00 = src + (size_t(y0) * srcW + size_t(x0)) * 4;
            const uint8_t* p10 = src + (size_t(y0) * srcW + size_t(x1)) * 4;
            const uint8_t* p01 = src + (size_t(y1) * srcW + size_t(x0)) * 4;
            const uint8_t* p11 = src + (size_t(y1) * srcW + size_t(x1)) * 4;
            uint8_t* out = dst.data() + (size_t(y) * dstW + size_t(x)) * 4;
            for (int c = 0; c < 4; ++c) {
                const float top = float(p00[c]) + (float(p10[c]) - float(p00[c])) * tx;
                const float bottom = float(p01[c]) + (float(p11[c]) - float(p01[c])) * tx;
                out[c] = uint8_t(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
}

uint8_t mipLevelCount(int width, int height) {
    // Mip level count from the larger dimension.
    uint8_t levels = 1;
    for (int m = (width > height ? width : height); m > 1; m >>= 1) ++levels;
    return levels;
}

void uploadMipChain(filament::Engine& engine, filament::Texture* texture,
                    uint32_t layer, std::vector<uint8_t> pixels, int width,
                    int height) {
    // Upload every mip level ourselves (simple box filter). Filament's
    // generateMipmaps() needs a texture usage flag whose name varies between
    // versions, so building the chain by hand keeps this version-proof.
    // 2D テクスチャは layer = 0（zoffset 0・depth 1 の setImage は 2D 版と同じ）、
    // 配列は層の番号。段数はテクスチャを作ったときの levels に合わせる。
    const int levels = int(texture->getLevels());
    std::vector<uint8_t>& mip = pixels;
    int w = width;
    int h = height;
    for (int level = 0; level < levels; ++level) {
        const size_t bytes = size_t(w) * size_t(h) * 4;
        texture->setImage(engine, size_t(level), 0, 0, layer, uint32_t(w),
                          uint32_t(h), 1, copyToPixelDescriptor(mip.data(), bytes));

        if (w == 1 && h == 1) break;
        const int nw = (w > 1) ? w / 2 : 1;
        const int nh = (h > 1) ? h / 2 : 1;
        std::vector<uint8_t> next(size_t(nw) * size_t(nh) * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                const int x0 = (w > 1) ? 2 * x : 0;
                const int y0 = (h > 1) ? 2 * y : 0;
                const int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
                const int y1 = (y0 + 1 < h) ? y0 + 1 : y0;
                for (int c = 0; c < 4; ++c) {
                    const int sum = mip[(size_t(y0) * w + x0) * 4 + c] +
                                    mip[(size_t(y0) * w + x1) * 4 + c] +
                                    mip[(size_t(y1) * w + x0) * 4 + c] +
                                    mip[(size_t(y1) * w + x1) * 4 + c];
                    next[(size_t(y) * nw + size_t(x)) * 4 + c] = uint8_t(sum / 4);
                }
            }
        }
        mip.swap(next);
        w = nw;
        h = nh;
    }
}

filament::Texture* loadTextureRGBA(filament::Engine& engine,
                                   const std::string& texturePath,
                                   const char* label,
                                   TextureFallback fallback) {
    std::vector<uint8_t> pixels;
    int texW = 0;
    int texH = 0;
    const bool fromFile =
        decodeTextureRGBA(texturePath, label, fallback, pixels, texW, texH);
    filament::Texture* texture =
        filament::Texture::Builder()
            .width(uint32_t(texW))
            .height(uint32_t(texH))
            .levels(mipLevelCount(texW, texH))
            .format(fromFile ? filament::Texture::InternalFormat::SRGB8_A8
                             : filament::Texture::InternalFormat::RGBA8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .build(engine);
    uploadMipChain(engine, texture, 0, std::move(pixels), texW, texH);
    return texture;
}

}  // namespace render_detail
