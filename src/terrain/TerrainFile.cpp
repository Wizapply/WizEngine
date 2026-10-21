#include "terrain/TerrainFile.h"

#include "core/Guid.h"

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
constexpr uint32_t kHeightsVersion = 1;
constexpr uint32_t kHolesVersion = 1;
constexpr std::size_t kSectionHeaderBytes = 16;
constexpr uint8_t kHoleByte = 0;     // HOLE 節: 穴
constexpr uint8_t kSolidByte = 255;  // HOLE 節: 中身
// readFile が読む大きさの上限（壊れた・別のファイルを丸ごと読まない安全弁）。
// 4096 の格子でも HGHT は約 34 MB。後の節（SPLT など）の余地を見て広めに。
constexpr uintmax_t kMaxFileBytes = 1024ull * 1024ull * 1024ull;

// ---- リトルエンディアンの読み書き ----
void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(uint8_t(v >> (8 * k)));
}
void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int k = 0; k < 8; ++k) out.push_back(uint8_t(v >> (8 * k)));
}
void putF32(std::vector<uint8_t>& out, float f) {
    uint32_t v = 0;
    std::memcpy(&v, &f, 4);
    putU32(out, v);
}
uint32_t getU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
uint64_t getU64(const uint8_t* p) {
    return uint64_t(getU32(p)) | (uint64_t(getU32(p + 4)) << 32);
}
float getF32(const uint8_t* p) {
    const uint32_t v = getU32(p);
    float f = 0.0f;
    std::memcpy(&f, &v, 4);
    return f;
}

std::size_t alignUp8(std::size_t n) { return (n + 7) & ~std::size_t(7); }

void padTo8(std::vector<uint8_t>& out) { out.resize(alignUp8(out.size()), 0); }

// 節の見出し（tag・版・payload の長さ）を書く。payload は呼び手が続けて足し、
// padTo8 で次の節の境界へ揃える。
void beginSection(std::vector<uint8_t>& out, const char tag[4], uint32_t version,
                  uint64_t byteLength) {
    out.insert(out.end(), tag, tag + 4);
    putU32(out, version);
    putU64(out, byteLength);
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

uint16_t quantize(float h, float height) {
    float t = h / height;
    if (!(t >= 0.0f)) t = 0.0f;  // NaN も 0
    if (t > 1.0f) t = 1.0f;
    return uint16_t(std::lround(t * 65535.0f));
}

// 節の位置（payload の先頭と長さ）。
struct SectionRef {
    const uint8_t* payload = nullptr;
    uint64_t length = 0;
    bool found = false;
};

// 節を辿って HGHT と HOLE を拾う。未知の節は読み飛ばす。
bool scanSections(const uint8_t* data, std::size_t size, uint32_t count,
                  SectionRef& heights, SectionRef& holes, std::string& reason) {
    std::size_t offset = kTerrainHeaderBytes;
    for (uint32_t s = 0; s < count; ++s) {
        if (size - offset < kSectionHeaderBytes) {
            reason = "section " + std::to_string(s) + " header runs past end of data";
            return false;
        }
        const uint8_t* p = data + offset;
        const uint32_t version = getU32(p + 4);
        const uint64_t length = getU64(p + 8);
        const std::size_t start = offset + kSectionHeaderBytes;
        if (length > uint64_t(size - start)) {
            reason = "section '" + std::string(reinterpret_cast<const char*>(p), 4) +
                     "' length " + std::to_string(length) + " runs past end of data";
            return false;
        }
        SectionRef* known = sameTag(p, kTagHeights) ? &heights
                            : sameTag(p, kTagHoles) ? &holes
                                                    : nullptr;
        if (known) {
            const uint32_t expected = (known == &heights) ? kHeightsVersion : kHolesVersion;
            const std::string tag(reinterpret_cast<const char*>(p), 4);
            if (known->found) {
                reason = "duplicate " + tag + " section";
                return false;
            }
            if (version != expected) {
                reason = "unsupported " + tag + " section version " + std::to_string(version);
                return false;
            }
            known->payload = data + start;
            known->length = length;
            known->found = true;
        }
        // 最後の節の後ろの詰め物は無くてもよい（size で頭打ち）。
        offset = std::min(alignUp8(start + std::size_t(length)), size);
    }
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
    h.version = getU32(data + 4);
    const uint32_t res = getU32(data + 8);
    h.size = getF32(data + 12);
    h.height = getF32(data + 16);
    std::memcpy(h.guid.bytes.data(), data + 20, 16);
    h.sectionCount = getU32(data + 36);
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
            std::vector<uint8_t>& out, std::string& reason) {
    if (!field.valid()) {
        reason = "height field is empty";
        return false;
    }
    if (!std::isfinite(height) || !(height > 0.0f)) {
        reason = "height must be a positive finite number";
        return false;
    }
    const std::size_t res = std::size_t(field.resolution());
    const bool holes = field.anyHoles();
    const std::vector<float>& samples = field.samples();

    std::vector<uint8_t> buf;
    buf.reserve(kTerrainHeaderBytes + 2 * kSectionHeaderBytes + samples.size() * 2 +
                (holes ? res * res : 0) + 16);
    buf.insert(buf.end(), kMagic, kMagic + 4);
    putU32(buf, kTerrainFileVersion);
    putU32(buf, uint32_t(res));
    putF32(buf, field.size());
    putF32(buf, height);
    buf.insert(buf.end(), guid.bytes.begin(), guid.bytes.end());
    putU32(buf, holes ? 2u : 1u);
    for (int k = 0; k < 2; ++k) putU32(buf, 0);  // reserved（見出しを 48 バイトに）

    beginSection(buf, kTagHeights, kHeightsVersion, uint64_t(samples.size()) * 2);
    for (const float h : samples) {
        const uint16_t q = quantize(h, height);
        buf.push_back(uint8_t(q & 0xff));
        buf.push_back(uint8_t(q >> 8));
    }
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
    out = std::move(buf);
    return true;
}

bool decode(const uint8_t* data, std::size_t size, TerrainFileData& out,
            std::string& reason) {
    TerrainFileHeader header;
    if (!readHeader(data, size, header, reason)) return false;
    SectionRef heights, holes;
    if (!scanSections(data, size, header.sectionCount, heights, holes, reason)) return false;

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

    HeightField field(header.resolution, header.size, 0.0f, 0.0f, 0.0f);
    if (!field.valid()) {  // readHeader の検査で起きないはず（安全弁）
        reason = "cannot build a height field from the header";
        return false;
    }
    std::vector<float>& samples = field.samples();
    const float scale = header.height / 65535.0f;
    for (std::size_t k = 0; k < samples.size(); ++k) {
        const uint16_t q =
            uint16_t(heights.payload[2 * k] | (uint16_t(heights.payload[2 * k + 1]) << 8));
        samples[k] = float(q) * scale;
    }
    if (holes.found) {
        for (std::size_t j = 0; j < res; ++j) {
            for (std::size_t i = 0; i < res; ++i) {
                if (holes.payload[j * res + i] == kHoleByte) field.setHole(int(i), int(j), true);
            }
        }
    }
    out.field = std::move(field);
    out.height = header.height;
    out.guid = header.guid;
    return true;
}

bool writeFile(const std::filesystem::path& path, const HeightField& field, float height,
               const TerrainGuid& guid, std::string& reason) {
    std::vector<uint8_t> bytes;
    if (!encode(field, height, guid, bytes, reason)) return false;

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

bool readFile(const std::filesystem::path& path, TerrainFileData& out, std::string& reason) {
    std::error_code ec;
    const uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        reason = "cannot open " + path.u8string() + ": " + ec.message();
        return false;
    }
    if (fileSize > kMaxFileBytes) {
        reason = path.u8string() + " is too large (" + std::to_string(fileSize) + " bytes)";
        return false;
    }
    // 全体を読む前に見出しだけで確かめる（.terrain という名前の別物を丸ごと
    // 読まない）。理由にはパスが付く。
    TerrainFileHeader header;
    if (!readHeader(path, header, reason)) return false;
    const uintmax_t side = uintmax_t(header.resolution) + 1;
    const uintmax_t minBytes = kTerrainHeaderBytes + kSectionHeaderBytes + side * side * 2;
    if (fileSize < minBytes) {
        reason = path.u8string() + " is too short for resolution " +
                 std::to_string(header.resolution) + " (" + std::to_string(fileSize) +
                 " bytes, need at least " + std::to_string(minBytes) + ")";
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
    if (!decode(bytes.data(), bytes.size(), out, reason)) {
        reason = path.u8string() + ": " + reason;
        return false;
    }
    return true;
}

}  // namespace terrain
}  // namespace wizengine
