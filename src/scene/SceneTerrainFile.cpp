// テレインの出入り（PHYSICS スレッド）: テレインをシーン文書・.terrain・ブラウザの
// JSON とやり取りする（文書の <terrain> から今のテレインを作る = .terrain の解決と
// 読込・旧形式の互換生成、新しい名前と背番号、保存 = .terrain を書く、/scene の JSON）。
// 格子の差し替えそのもの（ロック・dirty・Undo・当たり判定）は SceneTerrain.cpp の
// publishTerrainField / reshapeTerrain に、ブラウザの edit.terrain の受け口は
// SceneTerrainEdit.cpp に任せる。どのファイルかの判断は TerrainLibrary、1 ファイルの
// 読み書きは TerrainFile、ハイトマップの形式は Heightmap（どれも src/terrain）。
#include "scene/SceneInternal.h"

#include "render/ImageLoader.h"
#include "terrain/Heightmap.h"
#include "terrain/TerrainFile.h"

#include <filesystem>
#include <system_error>

using namespace scene_detail;

namespace wt = wizengine::terrain;

namespace {

// 一覧の 1 件（/scene の terrainFiles）。
nlohmann::json toJson(const wt::TerrainEntry& e) {
    return nlohmann::json{{"name", e.name},
                          {"guid", wt::guidToHex(e.guid)},
                          {"resolution", e.resolution},
                          {"size", e.size},
                          {"height", e.height}};
}

// 注意書きでのテレインの呼び方（名前、無ければ背番号、どちらも無ければ「名前なし」）。
std::string terrainLabel(const std::string& name, const std::string& guid) {
    if (!name.empty()) return name;
    if (!guid.empty()) return "（背番号 " + guid + "）";
    return "（名前なし）";
}

// Missing（ファイルが見つからない / 読めない）の説明の文。日本語の文はここだけが
// 持つ（読込の注意 terrainLoadCaveat_ と /scene の terrainMissing が同じ文になる）。
std::string terrainMissingNote(bool unreadable, const std::string& label) {
    if (unreadable) {
        return "テレインのファイル " + label +
               " が読めません（コンソール参照。ファイルを差し替えて開き直すか、"
               "別のテレインを開いてください）";
    }
    return "テレインのファイル " + label +
           " が見つかりません（assets/terrains/ に戻して開き直すか、"
           "別のテレインを開いてください）";
}

// ハイトマップの書き出し先（.terrain と同じ名前の隣で、拡張子が .png / .raw）。
// 名前が空・不正なら空のパス。書き出しと /scene の heightmapFiles が同じ場所を見る。
std::filesystem::path heightmapPath(const wt::TerrainLibrary& library, const std::string& name,
                                    bool png) {
    std::filesystem::path path = library.pathFor(name);
    if (!path.empty()) path.replace_extension(png ? ".png" : ".raw");
    return path;
}

// パスにファイルがあるか（空のパス・調べられないときは false = 投げない）。
bool fileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

}  // namespace

// ---- 新しいテレインの名前と背番号 -------------------------------------------

void Scene::fillTerrainIdentity(ed::TerrainDesc& desc, const std::string& base) const {
    // 空いている名前（衝突すれば _2, _3 …。base が空なら "terrain"）。
    if (desc.data.empty()) desc.data = terrainLibrary_->uniqueName(base);
    if (desc.guid.empty()) desc.guid = wt::guidToHex(wt::makeRandomGuid());
}

std::string Scene::publishNewTerrain(ed::TerrainDesc desc,
                                     std::shared_ptr<const wt::HeightField> field,
                                     const std::string& base) {
    desc.data.clear();
    desc.guid.clear();
    fillTerrainIdentity(desc, base);
    publishTerrainField(desc, std::move(field), TerrainHistory::Clear,
                        TerrainFileState::Dirty);
    return desc.data;
}

// ---- 読み込み -----------------------------------------------------------------

std::shared_ptr<const wt::HeightField> Scene::readTerrainFile(const std::string& name,
                                                              ed::TerrainDesc& desc,
                                                              std::string& reason) const {
    const std::filesystem::path path = terrainLibrary_->pathFor(name);
    if (path.empty()) {
        reason = "invalid terrain name '" + name + "'";
        return nullptr;
    }
    wt::TerrainFileData data;
    if (!wt::readFile(path, data, reason)) return nullptr;
    if (data.guid.empty()) {
        reason = path.u8string() + ": the guid is all zero";
        return nullptr;
    }
    ed::TerrainDesc got = desc;
    got.data = name;
    got.guid = wt::guidToHex(data.guid);
    got.size = data.field.size();
    got.resolution = data.field.resolution();
    got.height = data.height;
    const ed::TerrainDesc clamped = ed::clampTerrain(got);
    if (clamped.size != got.size || clamped.resolution != got.resolution ||
        clamped.height != got.height) {
        reason = path.u8string() + ": size " + std::to_string(got.size) + " / resolution " +
                 std::to_string(got.resolution) + " / height " + std::to_string(got.height) +
                 " is outside the editor's range";
        return nullptr;
    }
    // decode は原点中心・baseY 0 の格子を返す = そのまま使う（置き場所は
    // テレインのフレーム。terrain/TerrainFrame.h）。
    wt::HeightField field = data.field.withGeometry(data.field.size(), 0.0f, 0.0f, 0.0f);
    if (!field.valid()) {
        reason = path.u8string() + ": cannot place the grid at the terrain position";
        return nullptr;
    }
    desc = got;
    return std::make_shared<const wt::HeightField>(std::move(field));
}

void Scene::loadTerrainField(const ed::TerrainDesc& doc) {
    // 解決は一覧で引くので、読む前に取り直す（エクスプローラーで改名・追加された
    // ファイルを拾う）。
    refreshTerrainFiles();
    const std::string label = terrainLabel(doc.data, doc.guid);
    wt::TerrainGuid guid;  // 空 = 背番号無し（D）
    wt::guidFromHex(doc.guid, guid);
    const wt::TerrainResolution res = terrainLibrary_->resolve(doc.data, guid);
    for (const std::string& w : res.warnings) LOGW("scene", "terrain: %s", w.c_str());
    for (const std::string& n : res.notes) LOGI("scene", "terrain: %s", n.c_str());
    TerrainFileState missing = TerrainFileState::Missing;
    if (!res.file.empty()) {
        ed::TerrainDesc desc = doc;
        std::string reason;
        auto field = readTerrainFile(res.file, desc, reason);
        if (field) {
            if (res.dataChanged) {
                terrainLoadCaveat_ = "（テレイン " + label + " は背番号で見つけた " +
                                     desc.data + " を使いました）";
            } else if (!res.warnings.empty()) {
                terrainLoadCaveat_ = "（テレインの警告 " +
                                     std::to_string(res.warnings.size()) +
                                     " 件 - コンソール参照）";
            }
            publishTerrainField(desc, std::move(field), TerrainHistory::Clear,
                                TerrainFileState::Saved);
            return;
        }
        // 使えないファイル: 使わず、上書きもしない（壊れたファイル・共有ファイルの
        // 劣化・背番号の重複を作らない）。
        LOGW("scene", "terrain: %s - not used; the terrain stays missing (not drawn, "
                      "no collision, not editable) until the file is replaced",
             reason.c_str());
        missing = TerrainFileState::Unreadable;
        // 文に出すのは実際に読めなかったファイル（背番号で別の名前に解決したときも）。
        std::lock_guard<std::mutex> lk(objectsMutex_);
        terrainUnreadableFile_ = res.file;
    } else {
        LOGW("scene", "terrain: no file for '%s' (guid '%s') in assets/terrains/ - the "
                      "terrain stays missing (not drawn, no collision, not editable) until "
                      "the file is back",
             doc.data.c_str(), doc.guid.c_str());
    }
    // Missing: 文書の名前と背番号をそのまま覚えて（次の保存の XML に
    // 残る = ファイルを戻せば元に戻る）、格子無しで出す。注意の文は publish の後に
    // 状態から作る（/scene の terrainMissing と同じ口 = 名前が食い違わない）。
    publishTerrainField(doc, nullptr, TerrainHistory::Clear, missing);
    terrainLoadCaveat_ = "（注意: " + terrainMissingText() + "）";
}

bool Scene::refersToTerrain(const std::string& name) const {
    return hasTerrain_ && wt::sameTerrainName(terrain_.data, name);
}

bool Scene::isCurrentTerrain(const std::string& name) const {
    return terrainLoaded() && refersToTerrain(name);
}

bool Scene::isUnreadableTerrainFile(const std::string& name) const {
    return terrainUnreadable() && wt::sameTerrainName(terrainUnreadableFile_, name);
}

bool Scene::sameTerrainIdentity(const ed::TerrainDesc& a) const {
    if (!terrainLoaded()) return false;
    // 背番号は正規化済み（clampTerrain = 小文字の 32 桁）なので文字列で比べてよい。
    if (!a.guid.empty() && !terrain_.guid.empty()) return a.guid == terrain_.guid;
    return !a.data.empty() && isCurrentTerrain(a.data);
}

void Scene::setTerrainFromDocument(const ed::SceneDocument& doc, bool keepSameTerrain) {
    terrainLoadCaveat_.clear();
    if (!doc.hasTerrain) {
        publishTerrainField(ed::TerrainDesc{}, nullptr, TerrainHistory::Clear,
                            TerrainFileState::Saved);
        return;
    }
    if (keepSameTerrain && sameTerrainIdentity(doc.terrain)) {
        // 同じテレイン（XML の適用・/scene.xml の往復）: 手元の格子と名前・背番号・
        // 形を保ち、XML からは pos と表面だけを取る（未保存の編集を捨てない）。
        ed::TerrainDesc next = doc.terrain;
        next.data = terrain_.data;
        next.guid = terrain_.guid;
        next.size = terrain_.size;
        next.resolution = terrain_.resolution;
        next.height = terrain_.height;
        reshapeTerrain(next);
        return;
    }
    const ed::TerrainLegacyDesc& legacy = doc.terrainLegacy;
    if (legacy.used) {
        // 旧形式（data 無し）: 属性の大きさとノイズで一度生成し、新しい名前と
        // 背番号を付ける（次の保存で .terrain と新形式の XML になる）。
        ed::TerrainNoiseParams noise = legacy.noise;
        if (!legacy.hasNoise) noise.octaves = 0;  // <terrain/> は平ら
        const auto t0 = std::chrono::steady_clock::now();
        auto field = std::make_shared<const wt::HeightField>(
            wt::generateHeightField(toTerrainParams(doc.terrain, noise)));
        const double ms = elapsedMs(t0);
        const std::string name = publishNewTerrain(doc.terrain, std::move(field), doc.model);
        LOGI("scene", "terrain: legacy <terrain> without data - generated it from the "
                      "attributes (%s, %dx%d cells, %.1f ms) as '%s'",
             legacy.hasNoise ? "noise" : "flat", doc.terrain.resolution,
             doc.terrain.resolution, ms, name.c_str());
        terrainLoadCaveat_ = "（旧形式のテレインをテレイン " + name +
                             " として開きました - 保存すると .terrain に移ります）";
        return;
    }
    // 新形式: 名前と背番号で assets/terrains/ を解決して読む。
    loadTerrainField(doc.terrain);
}

std::string Scene::terrainLoadCaveat() {
    std::string out;
    out.swap(terrainLoadCaveat_);
    return out;
}

// ---- 保存 ---------------------------------------------------------------------

bool Scene::saveTerrainData(const std::string& sceneName, std::string& written,
                            std::string& reason) {
    written.clear();
    const std::shared_ptr<const wt::HeightField> field = terrainField();
    if (!field) return true;
    // 名前・背番号の欠けている方だけをここで決める（XML に data="" を出さない。
    // 正しい背番号・名前は捨てない）。
    if (terrain_.data.empty() || terrain_.guid.empty()) {
        ed::TerrainDesc d = terrain_;
        fillTerrainIdentity(d, sceneName);
        {
            std::lock_guard<std::mutex> lk(objectsMutex_);
            terrain_.data = d.data;
            terrain_.guid = d.guid;
        }
        refreshHeightmapFiles();
    }
    wt::TerrainGuid guid;
    wt::guidFromHex(terrain_.guid, guid);
    // 変えたときだけ書く（共有しているファイルを、触っていないシーンの保存で
    // 書き換えない）。ファイルが無ければ必ず書く。
    const std::filesystem::path path = terrainLibrary_->pathFor(terrain_.data);
    if (!terrainDirty() && fileExists(path)) return true;
    if (!wt::writeFile(path, *field, float(terrain_.height), guid, reason)) return false;
    terrainFile_ = TerrainFileState::Saved;  // 保存の成功（publish の外で書く唯一の場所）
    refreshTerrainFiles();
    written = terrain_.data;
    LOGI("scene", "terrain '%s' written: %s", written.c_str(), path.u8string().c_str());
    return true;
}

// ---- ハイトマップ -------------------------------------------------------------

void Scene::importTerrainHeightmap(const std::string& file, bool flipRows) {
    const std::shared_ptr<const wt::HeightField> field = terrainField();
    if (!field) {
        editor_.setStatus("テレインがありません（ハイトマップを取り込めません）");
        return;
    }
    if (!ed::assetFileAllowed(file)) {
        editor_.setStatus("ハイトマップは assets/ からの相対パスで指定してください");
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::string path = wizengine::assetPath(file);
    const std::filesystem::path fsPath = std::filesystem::u8path(path);
    // どれを呼ぶかを拡張子で決める。形式の検査は terrain/Heightmap（decodeRaw16 /
    // heightFieldFromSamples）。画像だけは、デコードの前に Scene が見出しで正方形と
    // 上限を断る（確保を避けるため。上限の値は Heightmap の定数）。RAW は Heightmap の
    // 最大バイト数を超えるファイルを読まない。
    std::vector<uint16_t> samples;
    int width = 0;
    int rows = 0;
    std::string reason;
    bool read = false;
    if (wt::sameTerrainName(fsPath.extension().u8string(), ".raw")) {
        std::vector<uint8_t> bytes;
        read = wt::readFileBytes(fsPath, wt::kMaxRaw16Bytes, bytes, reason) &&
               wt::decodeRaw16(bytes.data(), bytes.size(), samples, width, reason);
        rows = width;
    } else if (!readImageSize(path, width, rows)) {
        reason = path + ": " + imageFailureReason();
    } else if (width != rows) {
        // デコードの前に断る（正方形でない・大きすぎる画像で確保しない）。
        reason = "non-square image (the heightmap must be square)";
    } else if (width > wt::kMaxHeightmapSide) {
        reason = "image too large (at most " + std::to_string(wt::kMaxHeightmapSide) +
                 " pixels per side)";
    } else {
        read = loadImageGray16(path, samples, width, rows);
        if (!read) reason = path + ": " + imageFailureReason();
    }
    wt::HeightField gen;
    if (read) {
        gen = wt::heightFieldFromSamples(samples, width, field->size(), float(terrain_.height),
                                         field->resolution(), flipRows, reason);
    }
    if (!gen.valid()) {
        LOGW("scene", "terrain heightmap import '%s' (%dx%d): %s", file.c_str(), width, rows,
             reason.c_str());
        editor_.setStatus("ハイトマップ " + file + " を取り込めません（コンソール参照）");
        return;
    }
    samples = {};  // 4097² で 33 MB。差し替えの前に返す
    // 今の格子へ高さだけ入れる（穴は保つ・取り消し 1 段・未保存の印）。
    if (!replaceTerrainHeights(std::move(gen), "imported")) {
        editor_.setStatus("ハイトマップを取り込めませんでした（コンソール参照）");
        return;
    }
    const int res = field->resolution();
    LOGI("scene", "terrain heightmap imported: %s (%dx%d samples%s) -> %dx%d cells in %.1f ms",
         path.c_str(), width, rows, flipRows ? ", rows flipped" : "", res, res, elapsedMs(t0));
    char status[256];
    std::snprintf(status, sizeof(status),
                  "（%d×%d → 解像度 %d、0〜%g m。穴はそのまま、取り消しできます）", width,
                  rows, res, terrain_.height);
    editor_.setStatus("ハイトマップ " + file + " を取り込みました" + status);
}

void Scene::exportTerrainHeightmap(const std::string& format, bool overwrite,
                                   const std::string& expected) {
    const bool png = format == "png";
    if (!png && format != "raw") {
        editor_.setStatus("書き出しの形式が正しくありません（export は \"raw\" か \"png\"）");
        return;
    }
    const std::shared_ptr<const wt::HeightField> field = terrainField();
    if (!field) {
        editor_.setStatus("テレインがありません（ハイトマップを書き出せません）");
        return;
    }
    // ブラウザが確かめたテレインと今のテレインが違う（確認と送信の間に開き替え・
    // 改名された）なら、別のファイルを上書きしないよう断る。
    if (!expected.empty() && !wt::sameTerrainName(expected, terrain_.data)) {
        LOGW("scene", "terrain heightmap export refused: asked for '%s' but the current "
                      "terrain is '%s'", expected.c_str(), terrain_.data.c_str());
        editor_.setStatus("テレインが変わったので書き出しませんでした（もう一度選んでください）");
        return;
    }
    // 書き先は .terrain と同じ名前の隣。名前がまだ無いテレインは
    // ここで名前を決めない（決めるのは保存 = saveTerrainData）。
    const std::filesystem::path path = heightmapPath(*terrainLibrary_, terrain_.data, png);
    if (path.empty()) {
        editor_.setStatus("テレインに名前が無いので書き出せません（先に保存してください）");
        return;
    }
    // 見せるのは実際の場所（作業フォルダ基準の相対パスでは build/ の下だと分からない）。
    std::error_code absEc;
    const std::filesystem::path absolute = std::filesystem::absolute(path, absEc);
    const std::string shown = (absEc ? path : absolute).generic_u8string();
    // 既にあるファイル（取り込んだ元・同じ場所のテクスチャ）は、ブラウザが確認して
    // overwrite を付けたときだけ置き換える。
    if (!overwrite && fileExists(path)) {
        LOGI("scene", "terrain heightmap export refused: %s already exists (no overwrite)",
             shown.c_str());
        editor_.setStatus("ハイトマップ " + shown + " はすでにあります（上書きの確認が要ります）");
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int side = field->verticesPerSide();
    const float height = float(terrain_.height);
    std::vector<uint8_t> bytes;
    if (png) bytes = wt::encodePng16Gray(wt::heightmapSamples(*field, height), side);
    else wt::appendRaw16(*field, height, bytes);
    std::string reason = "cannot encode the grid";
    const bool ok = !bytes.empty() && wt::writeFileBytes(path, bytes, reason);
    if (!ok) {
        LOGW("scene", "terrain heightmap export to %s: %s", shown.c_str(), reason.c_str());
        editor_.setStatus("ハイトマップを書き出せません: " + shown + "（コンソール参照）");
        return;
    }
    LOGI("scene", "terrain heightmap exported: %s (%dx%d samples, %zu bytes) in %.1f ms",
         shown.c_str(), side, side, bytes.size(), elapsedMs(t0));
    refreshHeightmapFiles();  // 書き出し先ができた（/scene の heightmapFiles）
    // 上限より高い格子値は 65535 に切られている（通常の操作では起きにくい）。境は
    // 量子化の 1 段の半分の上（比例伸縮の丸めで 1ulp 超えた点は数えない）。
    const std::size_t clipped = field->countAbove(wt::quantizeClipLevel(height));
    const std::string clippedNote =
        clipped > 0 ? "。" + std::to_string(clipped) + " 点が上限で切られました" : "";
    char status[256];
    std::snprintf(status, sizeof(status), "（%d² × 16 bit、0〜%g m%s%s）", side, terrain_.height,
                  field->anyHoles() ? "。穴は書き出していません" : "", clippedNote.c_str());
    editor_.setStatus("ハイトマップを " + shown + " に書き出しました" + status);
}

// ---- ブラウザ ---------------------------------------------------------------

std::string Scene::terrainMissingText() const {
    if (!terrainMissing()) return std::string();
    if (terrainUnreadable())
        return terrainMissingNote(true, terrainUnreadableFile_);
    return terrainMissingNote(false, terrainLabel(terrain_.data, terrain_.guid));
}

nlohmann::json Scene::terrainJson() const {
    nlohmann::json j;
    // 値はテレインが無いときも直前のものを出す（欄を空にしない）。ただし
    // 有効にし直すと新規 = 既定の大きさの平らなテレインになる（位置と表面だけが
    // この値。applyTerrainOp の enabled:true）。
    j["hasTerrain"] = hasTerrain_;
    j["terrain"] = ed::toJson(terrain_);
    j["terrainDirty"] = terrainDirty();
    // Missing（ファイルが無い・読めない）なら説明の文、そうでなければ空。
    j["terrainMissing"] = terrainMissingText();
    // 読めている今のテレインの名前（無ければ空。isCurrentTerrain と同じ意味 =
    // ブラウザの「開いています」の判定はこれだけを見る）。
    j["terrainOpen"] = isCurrentTerrain(terrain_.data) ? terrain_.data : std::string();
    nlohmann::json files = nlohmann::json::array();
    for (const wt::TerrainEntry& e : terrainLibrary_->entries()) files.push_back(toJson(e));
    j["terrainFiles"] = std::move(files);
    // 今のテレインの書き出し先にファイルがあるか（ブラウザが書き出しの前に上書きを
    // 確かめる。exportTerrainHeightmap と同じ場所）。
    j["heightmapFiles"] = {{"raw", heightmapFiles_.raw}, {"png", heightmapFiles_.png}};
    return j;
}

void Scene::refreshTerrainFiles() {
    for (const std::string& w : terrainLibrary_->refresh())
        LOGW("scene", "terrain files: %s", w.c_str());
    // エクスプローラでハイトマップを足した・消したぶんも同じ時に拾う。
    refreshHeightmapFiles();
}

void Scene::refreshHeightmapFiles() {
    // ディスクはロックの外で見る（terrain_ を書くのはこのスレッドだけ = 読むのに
    // ロック不要）。
    HeightmapFilesFound found;
    found.raw = fileExists(heightmapPath(*terrainLibrary_, terrain_.data, false));
    found.png = fileExists(heightmapPath(*terrainLibrary_, terrain_.data, true));
    std::lock_guard<std::mutex> lk(objectsMutex_);
    heightmapFiles_ = found;
}
