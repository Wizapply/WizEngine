// Renderer のテクスチャ読込（地面とテレインで共用）。
#include "render/RendererInternal.h"
#include "render/ImageLoader.h"

using namespace filament;

namespace render_detail {

filament::Texture* loadTextureRGBA(filament::Engine& engine,
                                   const std::string& texturePath,
                                   const char* label,
                                   TextureFallback fallback) {
    // An image file (PNG/JPEG/TGA/BMP) when the scene names one. 読めない・
    // 無いときは代わりの絵に落として警告する（テクスチャはシーン文書の内容 =
    // 手で書けるので、glTF メッシュと同じく止めずに降格する）。Image
    // colours are sRGB; the fallbacks are linear multipliers on baseColor.
    std::vector<uint8_t> pixels;
    int texW = 0;
    int texH = 0;
    const bool fromFile =
        !texturePath.empty() &&
        loadImageRGBA(wizengine::assetPath(texturePath), pixels, texW, texH);
    if (fromFile) {
        LOGI("render", "%s texture: %s (%dx%d)", label,
             wizengine::assetPath(texturePath).c_str(), texW, texH);
    } else if (fallback == TextureFallback::Checker) {
        if (!texturePath.empty()) {
            LOGW("render", "%s texture '%s' not found, using generated checker",
                 label, texturePath.c_str());
        }
        texW = texH = 256;  // 2 x 2 squares, 128 px each
        pixels.resize(size_t(texW) * size_t(texH) * 4);
        for (int y = 0; y < texH; ++y) {
            for (int x = 0; x < texW; ++x) {
                const bool dark = (x < texW / 2) != (y < texH / 2);
                const uint8_t v = dark ? 105 : 255;
                uint8_t* p = pixels.data() + (size_t(y) * texW + size_t(x)) * 4;
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
        texW = texH = 1;
        pixels.assign(4, uint8_t(255));
    }

    // Mip level count from the larger dimension.
    uint8_t levels = 1;
    for (int m = (texW > texH ? texW : texH); m > 1; m >>= 1) ++levels;

    filament::Texture* texture =
        filament::Texture::Builder()
            .width(uint32_t(texW))
            .height(uint32_t(texH))
            .levels(levels)
            .format(fromFile ? filament::Texture::InternalFormat::SRGB8_A8
                             : filament::Texture::InternalFormat::RGBA8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .build(engine);

    // Upload every mip level ourselves (simple box filter). Filament's
    // generateMipmaps() needs a texture usage flag whose name varies between
    // versions, so building the chain by hand keeps this version-proof.
    std::vector<uint8_t> mip = pixels;
    int w = texW;
    int h = texH;
    for (int level = 0; level < int(levels); ++level) {
        const size_t bytes = size_t(w) * size_t(h) * 4;
        auto* buf = new uint8_t[bytes];
        std::memcpy(buf, mip.data(), bytes);
        filament::Texture::PixelBufferDescriptor pb(
            buf, bytes, filament::Texture::Format::RGBA,
            filament::Texture::Type::UBYTE,
            [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); });
        texture->setImage(engine, uint8_t(level), std::move(pb));

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
    return texture;
}

}  // namespace render_detail
