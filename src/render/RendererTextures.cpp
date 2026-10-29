// Renderer のテクスチャ読込（地面とテレインで共用）: 画像のデコード・大きさの
// 揃え直し（純粋な計算は render/ImageScale.h）・ミップ列のアップロードを別々の
// 関数にして、地面の 2D テクスチャとテレインの層のテクスチャ配列が同じ部品を通る。
#include "render/RendererInternal.h"
#include "render/ImageLoader.h"
#include "render/ImageScale.h"

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
    } else if (!texturePath.empty()) {
        // None: 画素は空のまま（代わりの絵は呼び手が持つ）。
        LOGW("render", "%s texture '%s' not found, using solid colour",
             label, texturePath.c_str());
    }
    return fromFile;
}

void uploadMipChain(filament::Engine& engine, filament::Texture* texture,
                    uint32_t layer, std::vector<uint8_t> pixels, int width,
                    int height) {
    // Upload every mip level ourselves (simple box filter). Filament's
    // generateMipmaps() needs a texture usage flag whose name varies between
    // versions, so building the chain by hand keeps this version-proof.
    // 2D テクスチャは layer = 0（zoffset 0・depth 1 の setImage は 2D 版と同じ）、
    // 配列は層の番号。段数はテクスチャを作ったときの levels に合わせる。
    // 各段は次の段を作ってから記述子へ移す（写さない。渡した後は Filament が
    // 読み終えるまで持つので、こちらからは触らない）。
    const int levels = int(texture->getLevels());
    std::vector<uint8_t> mip = std::move(pixels);
    std::vector<uint8_t> next;
    int w = width;
    int h = height;
    for (int level = 0; level < levels; ++level) {
        const bool last = level + 1 == levels || (w == 1 && h == 1);
        int nw = w;
        int nh = h;
        if (!last) halveRGBA(mip.data(), w, h, true, true, next, nw, nh);
        texture->setImage(engine, size_t(level), 0, 0, layer, uint32_t(w),
                          uint32_t(h), 1, moveToPixelDescriptor(std::move(mip)));
        if (last) break;
        mip = std::move(next);
        next = std::vector<uint8_t>();
        w = nw;
        h = nh;
    }
}

filament::Texture* loadTextureRGBA(filament::Engine& engine,
                                   const std::string& texturePath,
                                   const char* label) {
    std::vector<uint8_t> pixels;
    int texW = 0;
    int texH = 0;
    const bool fromFile = decodeTextureRGBA(texturePath, label, TextureFallback::Checker,
                                            pixels, texW, texH);
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
