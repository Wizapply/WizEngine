// The one translation unit that compiles stb_image. Keeping it alone here
// avoids pulling the (large) implementation into Renderer.cpp.
#define STB_IMAGE_IMPLEMENTATION
// STBI_ONLY_* is a whitelist: every decoder NOT listed is compiled out. HDR has
// to be here for the environment map, otherwise stbi_loadf rejects a perfectly
// valid Radiance file with the thoroughly misleading "unknown image type".
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_HDR
#include <stb_image.h>

#include "ImageLoader.h"
#include "Log.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace {

std::vector<unsigned char> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize size = in.tellg();
    in.seekg(0);
    // Braces, not parentheses: `bytes(std::size_t(size))` parses as a function
    // declaration (the most vexing parse) and compiles to nothing useful.
    const std::size_t count = std::size_t(size);
    std::vector<unsigned char> bytes(count, 0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    return bytes;
}

}  // namespace

std::string describeHeaderBytes(const std::string& path) {
    // The first bytes, printable characters as-is and everything else as \xNN.
    // When a file passes the signature check but no decoder accepts it, the
    // answer is almost always visible here: a stray BOM, CRLF, or a header
    // that is not what it claims.
    std::ifstream in(path, std::ios::binary);
    if (!in) return "(unreadable)";
    unsigned char b[48] = {0};
    in.read(reinterpret_cast<char*>(b), sizeof(b));
    const std::streamsize got = in.gcount();
    std::string out;
    for (std::streamsize i = 0; i < got; ++i) {
        if (b[i] >= 32 && b[i] < 127) {
            out += char(b[i]);
        } else {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\x%02X", b[i]);
            out += esc;
        }
    }
    return out;
}

std::string identifyImageFormat(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "unreadable";
    unsigned char b[16] = {0};
    in.read(reinterpret_cast<char*>(b), sizeof(b));
    const std::streamsize got = in.gcount();
    if (got < 4) return "too short to identify";

    auto starts = [&](const char* sig) {
        const std::size_t n = std::strlen(sig);
        return std::size_t(got) >= n && std::memcmp(b, sig, n) == 0;
    };
    if (starts("#?RADIANCE") || starts("#?RGBE")) return "Radiance HDR";
    if (b[0] == 0x76 && b[1] == 0x2F && b[2] == 0x31 && b[3] == 0x01) {
        return "OpenEXR (.exr) - convert it to Radiance .hdr";
    }
    if (b[0] == 0x89 && starts("\x89PNG")) return "PNG";
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "JPEG";
    if (starts("II*") || starts("MM\0*")) return "TIFF";
    if (starts("DDS ")) return "DDS";
    if (starts("«KTX") || (b[0] == 0xAB && b[1] == 0x4B)) return "KTX";

    char hex[64];
    std::snprintf(hex, sizeof(hex), "unrecognised (starts %02X %02X %02X %02X)",
                  b[0], b[1], b[2], b[3]);
    return hex;
}

const char* imageFailureReason() {
    const char* r = stbi_failure_reason();
    return r ? r : "unknown format";
}

bool loadImageHDR(const std::string& path, std::vector<float>& out, int& width,
                  int& height, int maxWidth) {
    int channels = 0;
    // 3 components: Filament's equirect input wants RGB; alpha is meaningless
    // for an environment.
    float* data = stbi_loadf(path.c_str(), &width, &height, &channels, 3);

    if (!data) {
        // stb matches the signature as "#?RADIANCE\n" exactly. Real files fail
        // that test for boring reasons: a UTF-8 BOM in front, or CRLF / bare-CR
        // line endings from a text-mode transfer. The pixels are fine in every
        // case, so rebuild a clean header in memory and try again.
        std::vector<unsigned char> bytes = readFileBytes(path);
        std::size_t begin = 0;
        if (bytes.size() > 3 && bytes[0] == 0xEF && bytes[1] == 0xBB &&
            bytes[2] == 0xBF) {
            begin = 3;  // skip a UTF-8 BOM
        }
        const bool isRadiance =
            bytes.size() > begin + 16 &&
            (std::memcmp(bytes.data() + begin, "#?RADIANCE", 10) == 0 ||
             std::memcmp(bytes.data() + begin, "#?RGBE", 6) == 0);

        if (isRadiance) {
            // Walk the text header line by line until the resolution line
            // (the one after the blank line), rewriting line ends to LF. `i`
            // then points at the first byte of the binary pixel data, which is
            // copied untouched - getting that index right is the whole trick,
            // since one byte of drift turns every pixel into garbage.
            std::vector<unsigned char> fixed;
            fixed.reserve(bytes.size());
            std::size_t i = begin;
            int linesAfterBlank = -1;  // -1 until the blank line is seen

            while (i < bytes.size()) {
                // Read one line, without its terminator.
                std::size_t lineStart = i;
                while (i < bytes.size() && bytes[i] != '\n' && bytes[i] != '\r') {
                    ++i;
                }
                const std::size_t lineLen = i - lineStart;
                // Step over the terminator, however it is written.
                if (i < bytes.size()) {
                    const bool cr = bytes[i] == '\r';
                    ++i;
                    if (cr && i < bytes.size() && bytes[i] == '\n') ++i;
                }
                fixed.insert(fixed.end(), bytes.begin() + std::streamoff(lineStart),
                             bytes.begin() + std::streamoff(lineStart + lineLen));
                fixed.push_back('\n');

                if (linesAfterBlank >= 0) break;   // that was the resolution line
                if (lineLen == 0) linesAfterBlank = 0;  // blank line: one more
            }

            fixed.insert(fixed.end(), bytes.begin() + std::streamoff(i),
                         bytes.end());

            data = stbi_loadf_from_memory(fixed.data(), int(fixed.size()),
                                          &width, &height, &channels, 3);
            if (data) {
                LOGW("hdr", "%s has a non-standard header (BOM or CR "
                            "line endings) - normalised and loaded",
                     path.c_str());
            }
        }
    }
    if (!data) return false;

    int factor = 1;
    while (maxWidth > 0 && width / factor > maxWidth) factor *= 2;

    if (factor == 1) {
        out.assign(data, data + std::size_t(width) * std::size_t(height) * 3);
    } else {
        // Box filter by an integer factor: averaging keeps the total energy,
        // which matters for lighting - point sampling would drop small bright
        // sources like the sun and change how the scene is lit.
        const int dw = width / factor;
        const int dh = height / factor;
        out.assign(std::size_t(dw) * std::size_t(dh) * 3, 0.0f);
        const float inv = 1.0f / float(factor * factor);
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                float acc[3] = {0.0f, 0.0f, 0.0f};
                for (int sy = 0; sy < factor; ++sy) {
                    const float* row =
                        data + (std::size_t(y * factor + sy) * width +
                                std::size_t(x * factor)) * 3;
                    for (int sx = 0; sx < factor; ++sx) {
                        acc[0] += row[sx * 3 + 0];
                        acc[1] += row[sx * 3 + 1];
                        acc[2] += row[sx * 3 + 2];
                    }
                }
                float* dst = out.data() + (std::size_t(y) * dw + x) * 3;
                dst[0] = acc[0] * inv;
                dst[1] = acc[1] * inv;
                dst[2] = acc[2] * inv;
            }
        }
        LOGI("hdr", "%dx%d downsampled to %dx%d (1/%d)", width, height, dw,
             dh, factor);
        width = dw;
        height = dh;
    }
    stbi_image_free(data);
    return true;
}

bool loadImageRGBA(const std::string& path, std::vector<uint8_t>& outPixels,
                   int& outWidth, int& outHeight) {
    int w = 0;
    int h = 0;
    int channels = 0;
    // Force 4 channels so the caller always gets RGBA8.
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        return false;
    }
    outPixels.assign(data, data + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(data);
    outWidth = w;
    outHeight = h;
    return true;
}
