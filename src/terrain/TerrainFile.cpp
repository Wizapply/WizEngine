#include "terrain/TerrainFile.h"

#include "core/Guid.h"
#include "terrain/ByteOrder.h"
#include "terrain/Heightmap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>

namespace wizengine {
namespace terrain {
namespace {

constexpr char kMagic[4] = {'W', 'Z', 'T', 'R'};
constexpr char kTagHeights[4] = {'H', 'G', 'H', 'T'};
constexpr char kTagHoles[4] = {'H', 'O', 'L', 'E'};
constexpr char kTagSplat[4] = {'S', 'P', 'L', 'T'};
constexpr char kTagLayers[4] = {'L', 'A', 'Y', 'R'};
constexpr uint32_t kHeightsVersion = 1;
constexpr uint32_t kHolesVersion = 1;
constexpr uint32_t kSplatVersion = 1;
constexpr uint32_t kLayersVersion = 1;
constexpr std::size_t kSectionHeaderBytes = 16;
constexpr uint8_t kHoleByte = 0;     // HOLE 節: 穴
constexpr uint8_t kSolidByte = 255;  // HOLE 節: 中身
// SPLT 節の長さ（1 辺 res セル、1 セル kSplatChannels バイト）。
constexpr std::size_t splatSectionBytes(std::size_t res) {
    return res * res * std::size_t(kSplatChannels);
}
// readFile が読む大きさの上限（壊れた・別のファイルを丸ごと読まない安全弁）。
// 4096 の格子でも HGHT は約 34 MB。後の節（SPLT など）の余地を見て広めに。
constexpr uintmax_t kMaxFileBytes = 1024ull * 1024ull * 1024ull;

using byteorder::getLE;
using byteorder::putLE;

// ---- float のリトルエンディアンの読み書き（整数は ByteOrder.h）----
void putF32(std::vector<uint8_t>& out, float f) {
    uint32_t v = 0;
    std::memcpy(&v, &f, 4);
    putLE<uint32_t>(out, v);
}
float getF32(const uint8_t* p) {
    const uint32_t v = getLE<uint32_t>(p);
    float f = 0.0f;
    std::memcpy(&f, &v, 4);
    return f;
}

// ---- 文字列（u16 の長さ + UTF-8。長さの上限は kMaxLayerPathBytes）----
using StringLength = uint16_t;                         // 長さの欄の型
constexpr std::size_t kStringLengthBytes = sizeof(StringLength);  // 長さの欄の大きさ
void putString(std::vector<uint8_t>& out, const std::string& s) {
    putLE<StringLength>(out, StringLength(s.size()));  // 長さは呼び手が上限内に保つ
    out.insert(out.end(), s.begin(), s.end());
}
// putString が書く（getString が読む）バイト数。
std::size_t stringBytes(std::size_t length) { return kStringLengthBytes + length; }
std::size_t stringBytes(const std::string& s) { return stringBytes(s.size()); }
// p から avail バイトの中の文字列を読む。読んだバイト数を used に書く。
bool getString(const uint8_t* p, std::size_t avail, std::string& out, std::size_t& used,
               std::string& reason) {
    if (avail < kStringLengthBytes) {
        reason = "string length runs past end of record";
        return false;
    }
    const std::size_t len = getLE<StringLength>(p);
    if (len > kMaxLayerPathBytes) {
        reason = "string length " + std::to_string(len) + " exceeds " +
                 std::to_string(kMaxLayerPathBytes);
        return false;
    }
    if (stringBytes(len) > avail) {
        reason = "string runs past end of record";
        return false;
    }
    out.assign(reinterpret_cast<const char*>(p + kStringLengthBytes), len);
    used = stringBytes(len);
    return true;
}

std::size_t alignUp8(std::size_t n) { return (n + 7) & ~std::size_t(7); }

void padTo8(std::vector<uint8_t>& out) { out.resize(alignUp8(out.size()), 0); }

// 節の見出し（tag・版・payload の長さ）を書く。payload は呼び手が続けて足し、
// padTo8 で次の節の境界へ揃える。
void beginSection(std::vector<uint8_t>& out, const char tag[4], uint32_t version,
                  uint64_t byteLength) {
    out.insert(out.end(), tag, tag + 4);
    putLE<uint32_t>(out, version);
    putLE<uint64_t>(out, byteLength);
}

bool sameTag(const uint8_t* p, const char tag[4]) { return std::memcmp(p, tag, 4) == 0; }

// splitmix64（背番号の代わりの作り方で、種を散らすためだけに使う）。
uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// random_device が使えないときの背番号: 時刻（2 種類）・スタックと関数の
// アドレス・呼び出し回数を混ぜる。
TerrainGuid fallbackGuid() {
    static std::atomic<uint64_t> counter{0};  // 複数スレッドから呼ばれても競合しない
    int local = 0;
    uint64_t state =
        uint64_t(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    state ^= uint64_t(std::chrono::system_clock::now().time_since_epoch().count()) << 1;
    state ^= uint64_t(reinterpret_cast<uintptr_t>(&local));
    state ^= uint64_t(reinterpret_cast<uintptr_t>(&fallbackGuid)) << 7;
    state ^= (counter.fetch_add(1) + 1) * 0xd1b54a32d192ed03ull;
    TerrainGuid g;
    for (int half = 0; half < 2; ++half) {
        const uint64_t v = splitmix64(state);
        for (int k = 0; k < 8; ++k) g.bytes[std::size_t(half * 8 + k)] = uint8_t(v >> (8 * k));
    }
    return g;
}

// 節の位置（payload の先頭と長さ）。
struct SectionRef {
    const uint8_t* payload = nullptr;
    uint64_t length = 0;
    bool found = false;
};

// 知っている節。
struct Sections {
    SectionRef heights, holes, splat, layers;
};

// 節を辿って知っている節を拾う。未知の節は読み飛ばす。
bool scanSections(const uint8_t* data, std::size_t size, uint32_t count, Sections& out,
                  std::string& reason) {
    const struct {
        const char* tag;
        uint32_t version;
        SectionRef* ref;
    } known[] = {{kTagHeights, kHeightsVersion, &out.heights},
                 {kTagHoles, kHolesVersion, &out.holes},
                 {kTagSplat, kSplatVersion, &out.splat},
                 {kTagLayers, kLayersVersion, &out.layers}};
    std::size_t offset = kTerrainHeaderBytes;
    for (uint32_t s = 0; s < count; ++s) {
        if (size - offset < kSectionHeaderBytes) {
            reason = "section " + std::to_string(s) + " header runs past end of data";
            return false;
        }
        const uint8_t* p = data + offset;
        const uint32_t version = getLE<uint32_t>(p + 4);
        const uint64_t length = getLE<uint64_t>(p + 8);
        const std::size_t start = offset + kSectionHeaderBytes;
        if (length > uint64_t(size - start)) {
            reason = "section '" + std::string(reinterpret_cast<const char*>(p), 4) +
                     "' length " + std::to_string(length) + " runs past end of data";
            return false;
        }
        SectionRef* hit = nullptr;
        uint32_t expected = 0;
        for (const auto& k : known) {
            if (sameTag(p, k.tag)) {
                hit = k.ref;
                expected = k.version;
            }
        }
        if (hit) {
            const std::string tag(reinterpret_cast<const char*>(p), 4);
            if (hit->found) {
                reason = "duplicate " + tag + " section";
                return false;
            }
            if (version != expected) {
                reason = "unsupported " + tag + " section version " + std::to_string(version);
                return false;
            }
            hit->payload = data + start;
            hit->length = length;
            hit->found = true;
        }
        // 最後の節の後ろの詰め物は無くてもよい（size で頭打ち）。
        offset = std::min(alignUp8(start + std::size_t(length)), size);
    }
    return true;
}

// LAYR 節の中身 → 層（形式は TerrainFile.h）。
bool decodeLayers(const SectionRef& sec, TerrainLayers& out, std::string& reason) {
    const uint8_t* p = sec.payload;
    std::size_t left = std::size_t(sec.length);
    if (left < 4) {
        reason = "LAYR section is too short";
        return false;
    }
    const uint32_t count = getLE<uint32_t>(p);
    p += 4;
    left -= 4;
    TerrainLayers layers;
    for (uint32_t k = 0; k < count; ++k) {
        const std::string what = "LAYR record " + std::to_string(k);
        const uint32_t recordBytes = left >= 4 ? getLE<uint32_t>(p) : 0;
        if (left < 4 || recordBytes > left - 4) {
            reason = what + " runs past end of section";
            return false;
        }
        p += 4;
        left -= 4;
        if (k < uint32_t(kSplatChannels)) {
            TerrainLayer& layer = layers[k];
            std::size_t used = 0;
            if (!getString(p, recordBytes, layer.texture, used, reason)) {
                reason = what + ": " + reason;
                return false;
            }
            if (recordBytes - used < 4) {
                reason = what + " is too short (" + std::to_string(recordBytes) + " bytes)";
                return false;
            }
            layer.tile = getF32(p + used);
        }
        p += recordBytes;  // 知らない欄・知らない層は飛ばす
        left -= recordBytes;
    }
    out = std::move(layers);
    return true;
}

}  // namespace

bool TerrainGuid::empty() const { return guidBytesEmpty(bytes); }

// hex の規則は core/Guid.h の 1 か所（document 層の TerrainDesc::guid と共通）。
std::string guidToHex(const TerrainGuid& guid) { return guidBytesToHex(guid.bytes); }

bool guidFromHex(const std::string& hex, TerrainGuid& out) {
    return guidBytesFromHex(hex, out.bytes);
}

TerrainGuid makeRandomGuid() {
    TerrainGuid g;
    try {
        std::random_device rd;
        for (std::size_t k = 0; k < 16; k += 4) {
            const uint32_t v = rd();
            for (std::size_t b = 0; b < 4; ++b) g.bytes[k + b] = uint8_t(v >> (8 * b));
        }
    } catch (...) {
        g = fallbackGuid();
    }
    if (g.empty()) g.bytes[0] = 1;  // 空（= 背番号なし）にはしない
    return g;
}

bool readHeader(const uint8_t* data, std::size_t size, TerrainFileHeader& out,
                std::string& reason) {
    if (!data || size < kTerrainHeaderBytes) {
        reason = "data is shorter than the " + std::to_string(kTerrainHeaderBytes) +
                 "-byte header";
        return false;
    }
    if (!sameTag(data, kMagic)) {
        reason = "not a terrain file (bad magic)";
        return false;
    }
    TerrainFileHeader h;
    h.version = getLE<uint32_t>(data + 4);
    const uint32_t res = getLE<uint32_t>(data + 8);
    h.size = getF32(data + 12);
    h.height = getF32(data + 16);
    std::memcpy(h.guid.bytes.data(), data + 20, 16);
    h.sectionCount = getLE<uint32_t>(data + 36);
    if (h.version != kTerrainFileVersion) {
        reason = "unsupported terrain file version " + std::to_string(h.version);
        return false;
    }
    if (res < 1 || res > uint32_t(kMaxResolution)) {
        reason = "resolution " + std::to_string(res) + " is out of range (1-" +
                 std::to_string(kMaxResolution) + ")";
        return false;
    }
    if (!std::isfinite(h.size) || !(h.size > 0.0f)) {
        reason = "size must be a positive finite number";
        return false;
    }
    if (!std::isfinite(h.height) || !(h.height > 0.0f)) {
        reason = "height must be a positive finite number";
        return false;
    }
    h.resolution = int(res);
    out = h;
    return true;
}

bool readHeader(const std::filesystem::path& path, TerrainFileHeader& out,
                std::string& reason) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        reason = "cannot open " + path.u8string();
        return false;
    }
    uint8_t buf[kTerrainHeaderBytes] = {};
    in.read(reinterpret_cast<char*>(buf), std::streamsize(sizeof(buf)));
    if (!readHeader(buf, std::size_t(in.gcount()), out, reason)) {
        reason = path.u8string() + ": " + reason;
        return false;
    }
    return true;
}

bool encode(const HeightField& field, float height, const TerrainGuid& guid,
            std::vector<uint8_t>& out, std::string& reason, const TerrainLayers& layers) {
    if (!field.valid()) {
        reason = "height field is empty";
        return false;
    }
    if (!std::isfinite(height) || !(height > 0.0f)) {
        reason = "height must be a positive finite number";
        return false;
    }
    for (const TerrainLayer& l : layers) {
        if (l.texture.size() > kMaxLayerPathBytes) {
            reason = "layer texture path is longer than " +
                     std::to_string(kMaxLayerPathBytes) + " bytes";
            return false;
        }
    }
    const std::size_t res = std::size_t(field.resolution());
    const bool holes = field.anyHoles();
    const bool splat = field.anySplat();  // 全部既定（塗って全部取り消した）なら書かない
    const bool layered = std::any_of(layers.begin(), layers.end(),
                                     [](const TerrainLayer& l) { return l != TerrainLayer{}; });
    const std::size_t splatBytes = splatSectionBytes(res);
    // HGHT の中身は RAW16 のハイトマップと同じ並び・同じ量子化（appendRaw16 が
    // buf の末尾へ直接書く = 標本・バイトの中間の配列を作らない）。
    const std::size_t heightBytes = field.samples().size() * 2;

    // LAYR の中身: u32 count + 層ごとに u32 recordBytes + レコード
    // （u16 pathLen + パス + f32 tile。欄は末尾へ足す = 古い版は recordBytes で飛ばす）。
    auto layerRecordBytes = [](const TerrainLayer& l) {
        return stringBytes(l.texture) + sizeof(float);
    };
    std::size_t layerBytes = sizeof(uint32_t);
    for (const TerrainLayer& l : layers) layerBytes += sizeof(uint32_t) + layerRecordBytes(l);

    // 節ごとに見出し + 中身 + 8 バイト境界までの詰め物（最大 7）。
    auto sectionBytes = [](std::size_t payload) {
        return kSectionHeaderBytes + alignUp8(payload);
    };
    std::vector<uint8_t> buf;
    buf.reserve(kTerrainHeaderBytes + sectionBytes(heightBytes) +
                (holes ? sectionBytes(res * res) : 0) + (splat ? sectionBytes(splatBytes) : 0) +
                (layered ? sectionBytes(layerBytes) : 0));
    buf.insert(buf.end(), kMagic, kMagic + 4);
    putLE<uint32_t>(buf, kTerrainFileVersion);
    putLE<uint32_t>(buf, uint32_t(res));
    putF32(buf, field.size());
    putF32(buf, height);
    buf.insert(buf.end(), guid.bytes.begin(), guid.bytes.end());
    putLE<uint32_t>(buf, 1u + (holes ? 1u : 0u) + (splat ? 1u : 0u) + (layered ? 1u : 0u));
    for (int k = 0; k < 2; ++k) putLE<uint32_t>(buf, 0);  // reserved（見出しを 48 バイトに）

    beginSection(buf, kTagHeights, kHeightsVersion, uint64_t(heightBytes));
    appendRaw16(field, height, buf);
    padTo8(buf);

    if (holes) {
        beginSection(buf, kTagHoles, kHolesVersion, uint64_t(res * res));
        for (std::size_t j = 0; j < res; ++j) {
            for (std::size_t i = 0; i < res; ++i) {
                buf.push_back(field.holeAt(int(i), int(j)) ? kHoleByte : kSolidByte);
            }
        }
        padTo8(buf);
    }

    if (splat) {
        beginSection(buf, kTagSplat, kSplatVersion, uint64_t(splatBytes));
        const std::size_t at = buf.size();
        buf.resize(at + splatBytes);
        field.copySplat({0, 0, int(res), int(res)}, buf.data() + at);
        padTo8(buf);
    }

    if (layered) {
        beginSection(buf, kTagLayers, kLayersVersion, uint64_t(layerBytes));
        putLE<uint32_t>(buf, uint32_t(kSplatChannels));
        for (const TerrainLayer& l : layers) {
            putLE<uint32_t>(buf, uint32_t(layerRecordBytes(l)));  // recordBytes
            putString(buf, l.texture);
            putF32(buf, l.tile);
        }
        padTo8(buf);
    }
    out = std::move(buf);
    return true;
}

bool decode(const uint8_t* data, std::size_t size, TerrainFileData& out,
            std::string& reason) {
    TerrainFileHeader header;
    if (!readHeader(data, size, header, reason)) return false;
    Sections sections;
    if (!scanSections(data, size, header.sectionCount, sections, reason)) return false;
    const SectionRef& heights = sections.heights;
    const SectionRef& holes = sections.holes;
    const SectionRef& splat = sections.splat;

    const std::size_t res = std::size_t(header.resolution);
    const std::size_t side = res + 1;
    if (!heights.found) {
        reason = "missing HGHT section";
        return false;
    }
    if (heights.length != uint64_t(side * side * 2)) {
        reason = "HGHT section length " + std::to_string(heights.length) + " does not match " +
                 "resolution " + std::to_string(res) + " (expected " +
                 std::to_string(side * side * 2) + ")";
        return false;
    }
    if (holes.found && holes.length != uint64_t(res * res)) {
        reason = "HOLE section length " + std::to_string(holes.length) + " does not match " +
                 "resolution " + std::to_string(res) + " (expected " +
                 std::to_string(res * res) + ")";
        return false;
    }
    const std::size_t splatBytes = splatSectionBytes(res);
    if (splat.found && splat.length != uint64_t(splatBytes)) {
        reason = "SPLT section length " + std::to_string(splat.length) + " does not match " +
                 "resolution " + std::to_string(res) + " (expected " +
                 std::to_string(splatBytes) + ")";
        return false;
    }
    TerrainLayers layers;
    if (sections.layers.found && !decodeLayers(sections.layers, layers, reason)) return false;

    // HGHT の中身は RAW16 のハイトマップと同じ。見出しの解像度の格子へ直接戻す
    // （標本の配列を作らない。長さは上で確かめた）。
    HeightField field(header.resolution, header.size, 0.0f, 0.0f, 0.0f);
    if (!decodeRaw16Into(heights.payload, std::size_t(heights.length), header.height, field,
                         reason)) {
        // readHeader と上の長さの検査で起きないはず（安全弁）。
        reason = "corrupt HGHT section: " + reason;
        return false;
    }
    if (holes.found) {
        for (std::size_t j = 0; j < res; ++j) {
            for (std::size_t i = 0; i < res; ++i) {
                if (holes.payload[j * res + i] == kHoleByte) field.setHole(int(i), int(j), true);
            }
        }
    }
    if (splat.found) field.pasteSplat({0, 0, int(res), int(res)}, splat.payload, splatBytes);
    out.field = std::move(field);
    out.height = header.height;
    out.guid = header.guid;
    out.layers = std::move(layers);
    return true;
}

bool writeFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
                    std::string& reason) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            reason = "cannot create directory " + parent.u8string() + ": " + ec.message();
            return false;
        }
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            reason = "cannot open " + tmp.u8string() + " for writing";
            return false;
        }
        o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        o.close();
        if (!o) {
            std::filesystem::remove(tmp, ec);
            reason = "failed to write " + tmp.u8string();
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);  // 既にあれば置き換える
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        reason = "cannot replace " + path.u8string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool readFileBytes(const std::filesystem::path& path, uintmax_t maxBytes,
                   std::vector<uint8_t>& out, std::string& reason) {
    std::error_code ec;
    const uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        reason = "cannot open " + path.u8string() + ": " + ec.message();
        return false;
    }
    if (fileSize > maxBytes) {
        reason = path.u8string() + " is too large (" + std::to_string(fileSize) + " bytes)";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        reason = "cannot open " + path.u8string();
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(fileSize));
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (std::size_t(in.gcount()) != bytes.size()) {
        reason = "failed to read " + path.u8string();
        return false;
    }
    out = std::move(bytes);
    return true;
}

bool writeFile(const std::filesystem::path& path, const HeightField& field, float height,
               const TerrainGuid& guid, std::string& reason, const TerrainLayers& layers) {
    std::vector<uint8_t> bytes;
    if (!encode(field, height, guid, bytes, reason, layers)) return false;
    return writeFileBytes(path, bytes, reason);
}

bool readFile(const std::filesystem::path& path, TerrainFileData& out, std::string& reason) {
    // 全体を読む前に見出しだけで確かめる（.terrain という名前の別物を丸ごと
    // 読まない）。理由にはパスが付く。
    TerrainFileHeader header;
    if (!readHeader(path, header, reason)) return false;
    // 短すぎるファイルも読む前に（大きさだけで）断る。大きさが取れなければ
    // readFileBytes が理由を付けて断る。
    const uintmax_t side = uintmax_t(header.resolution) + 1;
    const uintmax_t minBytes = kTerrainHeaderBytes + kSectionHeaderBytes + side * side * 2;
    std::error_code ec;
    const uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (!ec && fileSize < minBytes) {
        reason = path.u8string() + " is too short for resolution " +
                 std::to_string(header.resolution) + " (" + std::to_string(fileSize) +
                 " bytes, need at least " + std::to_string(minBytes) + ")";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, kMaxFileBytes, bytes, reason)) return false;
    if (!decode(bytes.data(), bytes.size(), out, reason)) {
        reason = path.u8string() + ": " + reason;
        return false;
    }
    return true;
}

}  // namespace terrain
}  // namespace wizengine
