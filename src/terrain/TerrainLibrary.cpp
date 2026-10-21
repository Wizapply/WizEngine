// assets/terrains/ の窓口。宣言と解決規則の表は TerrainLibrary.h。
#include "terrain/TerrainLibrary.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include "core/SafeName.h"

namespace wizengine {
namespace terrain {
namespace {

constexpr const char* kExtension = ".terrain";

// 名前順（一覧の並び・「名前順の最初」の選び方）。
bool byName(const TerrainEntry& a, const TerrainEntry& b) { return a.name < b.name; }

const TerrainEntry* findByName(const std::vector<TerrainEntry>& entries,
                               const std::string& name) {
    if (name.empty()) return nullptr;
    for (const TerrainEntry& e : entries)
        if (sameTerrainName(e.name, name)) return &e;
    return nullptr;
}

// 背番号が guid のエントリ（名前順）。
std::vector<const TerrainEntry*> findByGuid(const std::vector<TerrainEntry>& entries,
                                            const TerrainGuid& guid) {
    std::vector<const TerrainEntry*> out;
    for (const TerrainEntry& e : entries)
        if (e.guid == guid) out.push_back(&e);
    std::sort(out.begin(), out.end(),
              [](const TerrainEntry* a, const TerrainEntry* b) { return byName(*a, *b); });
    return out;
}

std::string inQuotes(const std::string& s) { return "'" + s + "'"; }

// メッセージでのテレインの呼び方。data 名が空なら背番号だけ。
std::string describe(const std::string& dataName, const TerrainGuid& guid) {
    if (dataName.empty()) return "terrain (guid " + guidToHex(guid) + ")";
    return "terrain " + inQuotes(dataName) + " (guid " + guidToHex(guid) + ")";
}

// 一覧に入れられない名前なら、その理由（英語）。入れられるなら空。
std::string unusableNameReason(const std::string& name) {
    if (isSafeName(name)) return {};
    if (name.empty()) return "name is empty";
    const std::string safe = sanitizeSafeName(name);
    if (name.size() > kSafeNameMaxLength && safe == name.substr(0, kSafeNameMaxLength))
        return "name is too long (max " + std::to_string(kSafeNameMaxLength) + " characters)";
    return "name must use only letters, digits, '_' and '-'";
}

// 背番号で走査した結果を r に入れる（B と C の共通部分）。見つかれば true。
bool adoptByGuid(const std::vector<TerrainEntry>& entries, const std::string& dataName,
                 const TerrainGuid& guid, TerrainResolution& r) {
    const std::vector<const TerrainEntry*> hits = findByGuid(entries, guid);
    if (hits.empty()) return false;
    r.file = hits.front()->name;
    r.guid = guid;
    const std::string what = describe(dataName, guid);
    if (hits.size() == 1) {
        r.notes.push_back(what + " found as " + inQuotes(r.file));
    } else {
        r.warnings.push_back(what + " matches " + std::to_string(hits.size()) +
                             " files; using " + inQuotes(r.file));
    }
    return true;
}

}  // namespace

bool sameTerrainName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        char x = a[k], y = b[k];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

TerrainResolution resolveTerrain(const std::vector<TerrainEntry>& entries,
                                 const std::string& dataName, const TerrainGuid& guid) {
    TerrainResolution r;
    const TerrainEntry* named = findByName(entries, dataName);
    if (guid.empty()) {
        // D: 手書きの XML。名前のファイルの背番号を採る。無ければ新しく採番。
        if (named) {
            r.file = named->name;
            r.guid = named->guid;
        } else {
            r.guid = makeRandomGuid();
            r.dirty = true;
            if (!dataName.empty())
                r.warnings.push_back("terrain file " + inQuotes(dataName) +
                                     " not found; starting flat");
        }
    } else if (named && named->guid == guid) {
        // A
        r.file = named->name;
        r.guid = guid;
    } else if (named) {
        // B: 名前の違うファイルが同じ背番号を持っていればそれ（改名されたもの）。
        if (!adoptByGuid(entries, dataName, guid, r)) {
            r.file = named->name;
            r.guid = named->guid;
            r.warnings.push_back("terrain " + inQuotes(dataName) + ": guid " + guidToHex(guid) +
                                 " not found; using the file's guid " +
                                 guidToHex(named->guid));
        }
    } else if (!adoptByGuid(entries, dataName, guid, r)) {
        // C の 0 個: 背番号を保ったまま平らで作る（次の保存でこの名前のファイルになる）。
        r.guid = guid;
        r.dirty = true;
        r.warnings.push_back(describe(dataName, guid) + " not found; starting flat");
    }
    r.dataChanged = !r.file.empty() && r.file != dataName;
    return r;
}

TerrainLibrary::TerrainLibrary(std::filesystem::path dir) : dir_(std::move(dir)) {}

std::vector<std::string> TerrainLibrary::refresh() {
    std::vector<std::string> warnings;
    std::vector<TerrainEntry> found;
    std::error_code ec;
    // フォルダが無い = テレインがまだ 1 つも無い（エラーではない）。あるのに
    // 調べられないときだけ警告する。
    const bool isDir = std::filesystem::is_directory(dir_, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        warnings.push_back("cannot access " + dir_.u8string() + ": " + ec.message());
    ec.clear();
    if (isDir) {
        for (std::filesystem::directory_iterator it(dir_, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::filesystem::path& path = it->path();
            std::error_code fileEc;
            // 書き込みの残骸 *.terrain.tmp は拡張子が .tmp なのでここで落ちる。
            // 拡張子の大文字・小文字は問わない（Windows の Hill.TERRAIN）。
            if (!it->is_regular_file(fileEc) ||
                !sameTerrainName(path.extension().u8string(), kExtension))
                continue;
            const std::string name = path.stem().u8string();
            const std::string unusable = unusableNameReason(name);
            if (!unusable.empty()) {
                warnings.push_back(path.u8string() + ": " + unusable + "; ignored");
                continue;
            }
            TerrainFileHeader header;
            std::string reason;
            if (!readHeader(path, header, reason)) {
                warnings.push_back(reason + "; ignored");
                continue;
            }
            found.push_back({name, header.guid, header.resolution, header.size, header.height});
        }
        if (ec) warnings.push_back("cannot list " + dir_.u8string() + ": " + ec.message());
    }
    std::sort(found.begin(), found.end(), byName);
    std::lock_guard<std::mutex> lock(mutex_);
    entries_ = std::move(found);
    return warnings;
}

std::vector<TerrainEntry> TerrainLibrary::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

TerrainResolution TerrainLibrary::resolve(const std::string& dataName,
                                          const TerrainGuid& guid) const {
    return resolveTerrain(entries(), dataName, guid);
}

TerrainRenameResult TerrainLibrary::rename(const std::string& from, const std::string& to) {
    TerrainRenameResult r;
    r.name = sanitizeSafeName(to);
    if (r.name.empty()) {
        r.status = TerrainRenameStatus::InvalidName;
        r.reason = "invalid terrain name " + inQuotes(to);
        return r;
    }
    if (r.name == from) {
        r.status = TerrainRenameStatus::SameName;
        r.reason = "terrain is already named " + inQuotes(from);
        return r;
    }
    // 大文字・小文字だけの変更は自分自身と重なるので数えない。
    if (taken(r.name, from)) {
        r.status = TerrainRenameStatus::AlreadyExists;
        r.reason = "terrain " + inQuotes(r.name) + " already exists";
        return r;
    }
    const std::filesystem::path src = pathFor(from);
    std::error_code ec;
    if (src.empty() || !std::filesystem::is_regular_file(src, ec)) {
        r.status = TerrainRenameStatus::NotFound;
        r.reason = "terrain file " + inQuotes(from) + " not found";
        return r;
    }
    // 置き換え先が既にあれば失敗にする（std::filesystem::rename は上書きする）。
    // taken() は大文字・小文字だけの違いをディスクで見ないので、区別する OS で
    // foo と Foo が両方あるときもここで止まる。同じファイル（区別しない OS での
    // 大文字・小文字だけの改名）なら通す。
    const std::filesystem::path dst = pathFor(r.name);
    std::error_code existsEc, sameEc;
    if (std::filesystem::exists(dst, existsEc) &&
        !std::filesystem::equivalent(src, dst, sameEc)) {
        r.status = TerrainRenameStatus::AlreadyExists;
        r.reason = "terrain " + inQuotes(r.name) + " already exists";
        return r;
    }
    std::filesystem::rename(src, dst, ec);
    if (ec) {
        r.status = TerrainRenameStatus::IoError;
        r.reason = "cannot rename " + src.u8string() + ": " + ec.message();
        return r;
    }
    r.status = TerrainRenameStatus::Ok;
    std::lock_guard<std::mutex> lock(mutex_);
    // 改名した 1 件だけを書き換える（綴りまで一致するもの、無ければ大文字・
    // 小文字を問わず最初の 1 件）。背番号と見出しはそのまま。
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const TerrainEntry& e) { return e.name == from; });
    if (it == entries_.end())
        it = std::find_if(entries_.begin(), entries_.end(), [&](const TerrainEntry& e) {
            return sameTerrainName(e.name, from);
        });
    if (it != entries_.end()) it->name = r.name;
    std::sort(entries_.begin(), entries_.end(), byName);
    return r;
}

std::string TerrainLibrary::uniqueName(const std::string& base) const {
    std::string name = sanitizeSafeName(base);
    if (name.empty()) name = "terrain";
    if (!taken(name)) return name;
    for (int n = 2;; ++n) {
        const std::string suffix = "_" + std::to_string(n);
        const std::string candidate = name.substr(0, kSafeNameMaxLength - suffix.size()) + suffix;
        if (!taken(candidate)) return candidate;
    }
}

std::filesystem::path TerrainLibrary::pathFor(const std::string& name) const {
    if (!isSafeName(name)) return {};
    return dir_ / (name + kExtension);
}

bool TerrainLibrary::taken(const std::string& name, const std::string& except) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const TerrainEntry& e : entries_)
            if (sameTerrainName(e.name, name) && !sameTerrainName(e.name, except)) return true;
    }
    // キャッシュの後にできたファイルも見る。except と同じ名前（大文字・小文字
    // だけの違い）は、大文字・小文字を区別しないファイルシステムでは自分自身。
    if (sameTerrainName(name, except)) return false;
    std::error_code ec;
    return std::filesystem::exists(pathFor(name), ec);
}

}  // namespace terrain
}  // namespace wizengine
