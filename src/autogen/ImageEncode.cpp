// stb_image_write の実装を持つ唯一の翻訳単位。STB_IMAGE_WRITE_STATIC で関数を
// この中に閉じる - Filament の同梱ライブラリ（stb）や他の翻訳単位と名前が
// ぶつからないように（cgltf を名前を付け替えて持つのと同じ用心）。
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "autogen/ImageEncode.h"

#include <algorithm>

namespace wizengine {
namespace autogen {

namespace {

void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* p = static_cast<const uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

}  // namespace

std::vector<uint8_t> encodeJpeg(const uint8_t* rgba, int width, int height, int quality) {
    std::vector<uint8_t> out;
    if (!rgba || width <= 0 || height <= 0) return out;
    // アルファは捨てて RGB へ（JPEG は 4 成分を受けない）。
    std::vector<uint8_t> rgb(std::size_t(width) * std::size_t(height) * 3);
    for (std::size_t i = 0, n = std::size_t(width) * std::size_t(height); i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    out.reserve(rgb.size() / 8);
    const int q = std::clamp(quality, 1, 100);
    if (!stbi_write_jpg_to_func(appendBytes, &out, width, height, 3, rgb.data(), q)) {
        out.clear();
    }
    return out;
}

std::string base64(const std::vector<uint8_t>& bytes) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) |
                           uint32_t(bytes[i + 2]);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = uint32_t(bytes[i]) << 16;
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string jpegDataUrl(const std::vector<uint8_t>& jpeg) {
    return "data:image/jpeg;base64," + base64(jpeg);
}

}  // namespace autogen
}  // namespace wizengine
