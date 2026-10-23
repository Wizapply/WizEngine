#include "terrain/Heightmap.h"

#include "terrain/ByteOrder.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace wizengine {
namespace terrain {
namespace {

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
// deflate の無圧縮ブロック 1 個に入る最大のバイト数（LEN が u16）。
constexpr std::size_t kStoredBlockMax = 65535;

// RAW16（と .terrain の HGHT）の標本と deflate の LEN / NLEN はリトルエンディアン、
// PNG と zlib のそれ以外の数値はビッグエンディアン。
using byteorder::getLE;
using byteorder::putBE;
using byteorder::putLE;

// CRC-32（PNG のチャンクの検査値。多項式 0xEDB88320 の反転形、表引き）。
uint32_t crc32(const uint8_t* data, std::size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            t[n] = c;
        }
        return t;
    }();
    uint32_t c = 0xffffffffu;
    for (std::size_t k = 0; k < size; ++k) c = table[(c ^ data[k]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// Adler-32（zlib の末尾の検査値）。5552 バイトごとに剰余を取る（その間は
// 32 ビットで溢れない最大の長さ）。
uint32_t adler32(const uint8_t* data, std::size_t size) {
    constexpr uint32_t kMod = 65521;
    constexpr std::size_t kRun = 5552;
    uint32_t a = 1, b = 0;
    while (size > 0) {
        const std::size_t n = size < kRun ? size : kRun;
        for (std::size_t k = 0; k < n; ++k) {
            a += data[k];
            b += a;
        }
        a %= kMod;
        b %= kMod;
        data += n;
        size -= n;
    }
    return (b << 16) | a;
}

// PNG のチャンク（長さ・種類・中身・種類と中身の CRC）を out に足す。
void putChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    putBE<uint32_t>(out, uint32_t(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    putBE<uint32_t>(out, crc32(out.data() + start, out.size() - start));
}

// 生のバイト列 → zlib（見出し + 無圧縮の deflate ブロック列 + Adler-32）。
// 圧縮しないのは依存（zlib）を増やさないため（1025² の格子で約 2.1 MB）。
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    const std::size_t blocks =
        raw.empty() ? 1 : (raw.size() + kStoredBlockMax - 1) / kStoredBlockMax;
    out.reserve(raw.size() + blocks * 5 + 6);
    out.push_back(0x78);  // CMF: deflate、窓 32 KB
    out.push_back(0x01);  // FLG: 圧縮なし相当、(CMF·256 + FLG) が 31 の倍数
    std::size_t offset = 0;
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t n = std::min(kStoredBlockMax, raw.size() - offset);
        out.push_back(uint8_t(b + 1 == blocks ? 1 : 0));  // BFINAL と BTYPE = 00（無圧縮）
        putLE<uint16_t>(out, uint16_t(n));   // LEN
        putLE<uint16_t>(out, uint16_t(~n));  // NLEN（LEN の 1 の補数）
        out.insert(out.end(), raw.begin() + std::ptrdiff_t(offset),
                   raw.begin() + std::ptrdiff_t(offset + n));
        offset += n;
    }
    putBE<uint32_t>(out, adler32(raw.data(), raw.size()));
    return out;
}

// RAW16（と .terrain の HGHT）の k 番目の標本（u16 リトルエンディアン）。
// decodeRaw16 と decodeRaw16Into が共用する。
uint16_t raw16At(const uint8_t* data, std::size_t k) { return getLE<uint16_t>(data + 2 * k); }

// 1 辺 w の正方形の標本を格子値（dst、行優先）へ戻す。標本 k の値は at(k)。flipRows は
// 行（Z）の向きを逆にして読む。heightFieldFromSamples（標本から）と decodeRaw16Into
// （RAW16 のバイト列から）が共用する = 並びと量子化の決まりはここ 1 か所。
template <class SampleAt>
void fillDequantized(std::vector<float>& dst, std::size_t w, float height, bool flipRows,
                     SampleAt at) {
    for (std::size_t j = 0; j < w; ++j) {
        const std::size_t src = flipRows ? (w - 1 - j) : j;
        for (std::size_t i = 0; i < w; ++i) {
            dst[j * w + i] = dequantizeHeight(at(src * w + i), height);
        }
    }
}

}  // namespace

uint16_t quantizeHeight(float h, float height) {
    float t = h / height;
    if (!(t >= 0.0f)) t = 0.0f;  // NaN も 0
    if (t > 1.0f) t = 1.0f;
    return uint16_t(std::lround(t * 65535.0f));
}

float dequantizeHeight(uint16_t q, float height) { return float(q) * (height / 65535.0f); }

float quantizeClipLevel(float height) { return height + height / 65535.0f * 0.5f; }

bool decodeRaw16(const uint8_t* data, std::size_t size, std::vector<uint16_t>& samples,
                 int& side, std::string& reason) {
    const std::size_t count = size / 2;
    const std::size_t n = std::size_t(std::llround(std::sqrt(double(count))));
    if (!data || size % 2 != 0 || n * n != count || n < 2 ||
        n > std::size_t(kMaxHeightmapSide)) {
        reason = "RAW heightmap must be a square of 16-bit little-endian samples (2 * n * n "
                 "bytes, n = 2-" + std::to_string(kMaxHeightmapSide) + "); got " +
                 std::to_string(size) + " bytes (8-bit RAW is not supported)";
        return false;
    }
    std::vector<uint16_t> out(count);
    for (std::size_t k = 0; k < count; ++k) {
        out[k] = raw16At(data, k);
    }
    samples = std::move(out);
    side = int(n);
    return true;
}

bool decodeRaw16Into(const uint8_t* data, std::size_t size, float height, HeightField& field,
                     std::string& reason) {
    const std::size_t w = std::size_t(field.verticesPerSide());
    if (!field.valid() || !data || size != w * w * 2) {
        reason = "RAW16 data is " + std::to_string(size) + " bytes; the grid needs " +
                 std::to_string(w * w * 2);
        return false;
    }
    fillDequantized(field.samples(), w, height, /*flipRows=*/false,
                    [data](std::size_t k) { return raw16At(data, k); });
    return true;
}

void appendRaw16(const HeightField& field, float height, std::vector<uint8_t>& out) {
    const std::vector<float>& h = field.samples();
    out.reserve(out.size() + h.size() * 2);
    for (const float v : h) putLE<uint16_t>(out, quantizeHeight(v, height));
}

std::vector<uint8_t> encodePng16Gray(const std::vector<uint16_t>& samples, int side) {
    if (side < 2 || side > kMaxHeightmapSide ||
        samples.size() != std::size_t(side) * std::size_t(side)) {
        return {};
    }
    const std::size_t w = std::size_t(side);

    std::vector<uint8_t> ihdr;
    putBE<uint32_t>(ihdr, uint32_t(side));  // 幅
    putBE<uint32_t>(ihdr, uint32_t(side));  // 高さ
    ihdr.push_back(16);              // ビット深度
    ihdr.push_back(0);               // 色の種類: グレースケール
    ihdr.push_back(0);               // 圧縮: deflate
    ihdr.push_back(0);               // フィルタ: 標準
    ihdr.push_back(0);               // インターレースなし

    // 画像データ: 行ごとにフィルタの種類（0 = なし）+ 標本（ビッグエンディアン）。
    std::vector<uint8_t> raw;
    raw.reserve(w * (1 + 2 * w));
    for (std::size_t j = 0; j < w; ++j) {
        raw.push_back(0);
        for (std::size_t i = 0; i < w; ++i) putBE<uint16_t>(raw, samples[j * w + i]);
    }

    const std::vector<uint8_t> idat = zlibStored(raw);
    raw = {};  // 4097² で 33 MB。出力を確保する前に返す

    // チャンク 1 個 = 長さ・種類・CRC の 12 バイト + 中身。3 個ぶんを先に確保する。
    constexpr std::size_t kChunkOverhead = 12;
    std::vector<uint8_t> out;
    out.reserve(sizeof(kPngSignature) + 3 * kChunkOverhead + ihdr.size() + idat.size());
    out.insert(out.end(), kPngSignature, kPngSignature + sizeof(kPngSignature));
    putChunk(out, "IHDR", ihdr);
    putChunk(out, "IDAT", idat);
    putChunk(out, "IEND", {});
    return out;
}

std::vector<uint16_t> heightmapSamples(const HeightField& field, float height) {
    std::vector<uint16_t> out;
    out.reserve(field.samples().size());
    for (const float h : field.samples()) out.push_back(quantizeHeight(h, height));
    return out;
}

HeightField heightFieldFromSamples(const std::vector<uint16_t>& samples, int side,
                                   float size, float height, int resolution,
                                   bool flipRows, std::string& reason) {
    if (side < 2 || side > kMaxHeightmapSide ||
        samples.size() != std::size_t(side) * std::size_t(side)) {
        reason = "heightmap samples must be a square of 2-" +
                 std::to_string(kMaxHeightmapSide) + " per side";
        return {};
    }
    if (!std::isfinite(height) || !(height > 0.0f)) {
        reason = "height must be a positive finite number";
        return {};
    }
    if (resolution < 1 || resolution > kMaxResolution) {
        reason = "resolution " + std::to_string(resolution) + " is out of range (1-" +
                 std::to_string(kMaxResolution) + ")";
        return {};
    }
    HeightField field(side - 1, size, 0.0f, 0.0f, 0.0f);
    if (!field.valid()) {
        reason = "size must be a positive finite number";
        return {};
    }
    fillDequantized(field.samples(), std::size_t(side), height, flipRows,
                    [&samples](std::size_t k) { return samples[k]; });
    // resampled は同じ解像度でも丸ごと写すので、違うときだけ呼ぶ（4096 で 67 MB）。
    if (resolution == side - 1) return field;
    return field.resampled(resolution);
}

}  // namespace terrain
}  // namespace wizengine
