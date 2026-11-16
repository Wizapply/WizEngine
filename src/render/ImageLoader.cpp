// stb_image の実装をコンパイルする唯一の翻訳単位。画像を読むのは
// RendererGround.cpp（地面のテクスチャ）と EnvironmentLoader.cpp（HDR）、
// SceneTerrainFile.cpp（テレインのハイトマップ、loadImageGray16）で、
// 大きな実装をそちらへ持ち込まないためにここだけに置く。
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

#include "render/ImageLoader.h"
#include "core/FileBytes.h"
#include "core/Log.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// stb に渡す前にファイルが読めなかった理由（stb の理由と同じくスレッドごと）。
// 読めたら null に戻し、以後の理由は stb が持つ（imageFailureReason）。
thread_local const char* tReadFailure = nullptr;

// stb のメモリ版（stbi_*_from_memory）は長さを int で受ける。int に収まらない長さは
// 渡さず、理由を tReadFailure に残して false（読んだファイルも、HDR の見出しを
// 直したデータも、同じ確かめと同じ文）。
bool fitsStbLength(std::size_t size) {
    if (size <= std::size_t(INT_MAX)) return true;
    tReadFailure = "file is too large to decode (over 2 GB)";
    return false;
}

// 画像のファイル（path は UTF-8。先頭から maxBytes バイトまで、既定 = 丸ごと）を
// stb のメモリ版（stbi_*_from_memory）へ渡すために読む。UTF-8 のパスとして開くので、
// Windows でも日本語などを含むパスが開ける（stb の fopen は開けない）。
// loadImageRGBA / readImageSize / loadImageGray16 / loadImageHDR が共用する。読めない・
// 空・stb の長さ（int）に収まらないなら false（bytes は空）で、理由は
// imageFailureReason が返す。
bool readImageBytes(const std::string& path, std::vector<unsigned char>& bytes,
                    std::uintmax_t maxBytes = wizengine::kNoByteLimit) {
    std::string reason;  // 理由は下の固定の文で出す（ログの文を変えない）
    if (!wizengine::readFileHead(wizengine::utf8Path(path), maxBytes, bytes, reason)) {
        // 失敗では readFileHead が bytes を触らない。呼ぶ側は同じ配列を使い回す
        // （readImageSize の読み直し）ので、前の中身を消して「空 = 失敗」に揃える。
        bytes.clear();
    }
    if (bytes.empty()) {
        tReadFailure = "cannot open or read the file (or it is empty)";
        return false;
    }
    if (!fitsStbLength(bytes.size())) {
        bytes = {};
        return false;
    }
    tReadFailure = nullptr;
    return true;
}

// readImageSize が読む先頭の大きさ。PNG の見出しは先頭の数十バイトにある。JPEG は
// 寸法の前に大きな付帯情報（最大 64 KB の区画）が並びうるので、先頭が JPEG の印
// （FF D8）で寸法に届かなかったときだけ丸ごと読み直す（readImageSize）。
constexpr std::size_t kImageHeadBytes = 64 * 1024;

// stb が返した画素（components 成分）を out へ移して解放し、幅と高さを書く。
// data が null か大きさが 0 なら false（出力は触らない。data は解放する）。
template <typename T>
bool takePixels(T* data, int w, int h, int components, std::vector<T>& out, int& outWidth,
                int& outHeight) {
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        return false;
    }
    out.assign(data, data + std::size_t(w) * std::size_t(h) * std::size_t(components));
    stbi_image_free(data);
    outWidth = w;
    outHeight = h;
    return true;
}

}  // namespace

std::string describeHeaderBytes(const std::string& path) {
    // The first bytes, printable characters as-is and everything else as \xNN.
    // When a file passes the signature check but no decoder accepts it, the
    // answer is almost always visible here: a stray BOM, CRLF, or a header
    // that is not what it claims.
    std::vector<unsigned char> b;
    std::string reason;
    if (!wizengine::readFileHead(wizengine::utf8Path(path), 48, b, reason)) {
        return "(unreadable)";
    }
    std::string out;
    for (std::size_t i = 0; i < b.size(); ++i) {
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
    std::vector<unsigned char> b;
    std::string reason;
    if (!wizengine::readFileHead(wizengine::utf8Path(path), 16, b, reason)) {
        return "unreadable";
    }
    if (b.size() < 4) return "too short to identify";

    auto starts = [&](const char* sig) {
        const std::size_t n = std::strlen(sig);
        return b.size() >= n && std::memcmp(b.data(), sig, n) == 0;
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
    if (tReadFailure) return tReadFailure;
    const char* r = stbi_failure_reason();
    return r ? r : "unknown format";
}

bool loadImageHDR(const std::string& path, std::vector<float>& out, int& width,
                  int& height, int maxWidth) {
    int channels = 0;
    // ファイルは 1 回だけ読む（UTF-8 のパス）。読めなければ理由は tReadFailure。
    std::vector<unsigned char> bytes;
    if (!readImageBytes(path, bytes)) return false;
    // 3 components: Filament's equirect input wants RGB; alpha is meaningless
    // for an environment.
    float* data = stbi_loadf_from_memory(bytes.data(), int(bytes.size()), &width, &height,
                                         &channels, 3);

    if (!data) {
        // stb matches the signature as "#?RADIANCE\n" exactly. Real files fail
        // that test for boring reasons: a UTF-8 BOM in front, or CRLF / bare-CR
        // line endings from a text-mode transfer. The pixels are fine in every
        // case, so rebuild a clean header in memory and try again.
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
            // 直したデータは元より最大 1 バイト長い（行の終わりは LF 1 つに揃え、
            // 終わりの無い最後の見出しの行にだけ LF を足す）。写しを作る前に、
            // その長さが stb に渡せるかを確かめる。
            if (!fitsStbLength(bytes.size() + 1)) return false;
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
    std::vector<unsigned char> bytes;
    if (!readImageBytes(path, bytes)) return false;
    // Force 4 channels so the caller always gets RGBA8.
    stbi_uc* data =
        stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
    return takePixels(data, w, h, 4, outPixels, outWidth, outHeight);
}

bool readImageSize(const std::string& path, int& width, int& height) {
    int w = 0;
    int h = 0;
    int channels = 0;
    std::vector<unsigned char> bytes;
    if (!readImageBytes(path, bytes, kImageHeadBytes)) return false;
    const auto info = [&]() {
        return stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels) &&
               w > 0 && h > 0;
    };
    // 丸ごと読み直すのは、JPEG（先頭が FF D8）で、先頭 64 KB が大きな付帯情報で
    // 埋まって寸法に届かなかったときだけ（画像でない大きなファイルを丸ごと読まない）。
    const bool truncatedJpeg = bytes.size() == kImageHeadBytes && bytes[0] == 0xFF &&
                               bytes[1] == 0xD8;
    if (!info() && (!truncatedJpeg || !readImageBytes(path, bytes) || !info())) {
        return false;
    }
    width = w;
    height = h;
    return true;
}

bool loadImageGray16(const std::string& path, std::vector<uint16_t>& out, int& width,
                     int& height) {
    int w = 0;
    int h = 0;
    int channels = 0;
    std::vector<unsigned char> bytes;
    if (!readImageBytes(path, bytes)) return false;
    // 1 成分を指定: stb が輝度へ直し、8 ビットの画像も 16 ビットへ引き伸ばす。
    stbi_us* data =
        stbi_load_16_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 1);
    return takePixels(data, w, h, 1, out, width, height);
}
