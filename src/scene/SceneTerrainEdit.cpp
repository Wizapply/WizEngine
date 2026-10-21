// テレインの編集操作とファイルの出入り（PHYSICS スレッド）: 文書の <terrain>
// から今のテレインを作る（.terrain の解決と読込・旧形式の互換生成）、新規・
// 開く・名前の変更・外す、保存（.terrain を書く）、ブラウザ向けの JSON。
// 格子の差し替えそのもの（ロック・dirty・Undo・当たり判定）は SceneTerrain.cpp の
// publishTerrainField / reshapeTerrain に任せ、ここは「どの格子を、どの名前で」
// だけを決める。どのファイルかの判断は TerrainLibrary、1 ファイルの読み書きは
// TerrainFile（どちらも src/terrain）。
#include "scene/SceneInternal.h"

#include <filesystem>
#include <system_error>

#include "core/SafeName.h"

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

// 形（size / resolution / height）を新規テレインの既定値（ed::TerrainDesc{}）へ。
// 位置と表面はそのまま。
void resetToDefaultShape(ed::TerrainDesc& d) {
    const ed::TerrainDesc def;
    d.size = def.size;
    d.resolution = def.resolution;
    d.height = def.height;
}

// 注意書きでのテレインの呼び方（名前、無ければ背番号、どちらも無ければ「名前なし」）。
std::string terrainLabel(const std::string& name, const std::string& guid) {
    if (!name.empty()) return name;
    if (!guid.empty()) return "（背番号 " + guid + "）";
    return "（名前なし）";
}

// desc の大きさと位置の平らな格子。
std::shared_ptr<const wt::HeightField> flatTerrainField(const ed::TerrainDesc& d) {
    return std::make_shared<const wt::HeightField>(
        d.resolution, float(d.size), float(d.position.x), float(d.position.y),
        float(d.position.z));
}

// name のファイルを読み、使えるなら desc.position に置いた格子を返して、desc の
// data / guid / size / resolution / height を読んだ値で埋める（位置と表面は
// そのまま）。**使えないファイル**は null と英語の reason（desc は触らない）:
//   * 読めない（壊れている・形式が違う）
//   * 背番号が全部 0（背番号での解決と食い違う）
//   * 形が clampTerrain を通すと変わる（解像度 2048・size 5000 など。エディタの
//     範囲で丸めて使うと、保存で共有ファイルが黙って劣化する）
// 範囲の定義は clampTerrain の 1 か所で、ここは「通すと変わるか」を比べるだけ。
std::shared_ptr<const wt::HeightField> readTerrainFile(const wt::TerrainLibrary& lib,
                                                       const std::string& name,
                                                       ed::TerrainDesc& desc,
                                                       std::string& reason) {
    const std::filesystem::path path = lib.pathFor(name);
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
    // decode は原点中心・baseY 0 の格子を返すので、文書の pos へ置き直す。
    wt::HeightField field = data.field.withGeometry(
        data.field.size(), float(got.position.x), float(got.position.y),
        float(got.position.z));
    if (!field.valid()) {
        reason = path.u8string() + ": cannot place the grid at the terrain position";
        return nullptr;
    }
    desc = got;
    return std::make_shared<const wt::HeightField>(std::move(field));
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

std::shared_ptr<const wt::HeightField> Scene::loadTerrainField(ed::TerrainDesc& desc,
                                                               wt::TerrainResolution& res) {
    const std::string asked = desc.data;
    const std::string askedLabel = terrainLabel(desc.data, desc.guid);
    wt::TerrainGuid guid;  // 空 = 背番号無し（D）
    wt::guidFromHex(desc.guid, guid);
    res = terrainLibrary_->resolve(asked, guid);
    if (!res.file.empty()) {
        std::string reason;
        auto field = readTerrainFile(*terrainLibrary_, res.file, desc, reason);
        if (field) {
            if (res.dataChanged) {
                terrainLoadCaveat_ = "（テレイン " + askedLabel + " は背番号で見つけた " +
                                     desc.data + " を使いました）";
            } else if (!res.warnings.empty()) {
                terrainLoadCaveat_ = "（テレインの警告 " +
                                     std::to_string(res.warnings.size()) +
                                     " 件 - コンソール参照）";
            }
            return field;
        }
        // 使えないファイル: 使わず、上書きもしない（壊れたファイル・共有ファイルの
        // 劣化・背番号の重複を作らない）= 新しい名前と背番号の平らなテレイン。
        res.warnings.push_back(reason + "; not used - starting flat as a new terrain");
        res.dirty = true;
        desc.data.clear();
        desc.guid.clear();
        fillTerrainIdentity(desc, asked);
        terrainLoadCaveat_ = "（注意: テレイン " + res.file +
                             " のファイルが使えないので、平らな新しいテレイン " +
                             desc.data + " で開きました - 元のファイルは残ります）";
    } else {
        // ファイルが無い: 名前と解決した背番号のまま（次の保存でできる）。
        desc.guid = wt::guidToHex(res.guid);
        terrainLoadCaveat_ = "（注意: テレイン " + terrainLabel(desc.data, desc.guid) +
                             " のファイルが無いので平らで開きました）";
    }
    resetToDefaultShape(desc);
    return flatTerrainField(desc);
}

bool Scene::sameTerrainIdentity(const ed::TerrainDesc& a) const {
    if (!hasTerrain_) return false;
    // 背番号は正規化済み（clampTerrain = 小文字の 32 桁）なので文字列で比べてよい。
    if (!a.guid.empty() && !terrain_.guid.empty()) return a.guid == terrain_.guid;
    return !a.data.empty() && wt::sameTerrainName(a.data, terrain_.data);
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
    // 新形式: 名前と背番号で assets/terrains/ を解決して読む。解決は一覧で引くので、
    // 読む前に取り直す（エクスプローラーで改名・追加されたファイルを拾う）。
    refreshTerrainFiles();
    ed::TerrainDesc desc = doc.terrain;
    wt::TerrainResolution res;
    auto field = loadTerrainField(desc, res);
    for (const std::string& w : res.warnings) LOGW("scene", "terrain: %s", w.c_str());
    for (const std::string& n : res.notes) LOGI("scene", "terrain: %s", n.c_str());
    publishTerrainField(desc, std::move(field), TerrainHistory::Clear,
                        res.dirty ? TerrainFileState::Dirty : TerrainFileState::Saved);
}

std::string Scene::terrainLoadCaveat() {
    std::string out;
    out.swap(terrainLoadCaveat_);
    return out;
}

// ---- 新規・開く・名前の変更 ---------------------------------------------------

void Scene::newTerrain(const std::string& base) {
    ed::TerrainDesc desc = terrain_;  // 位置と表面は今のもの
    resetToDefaultShape(desc);
    // ステータスは publish の前（系を切り替えたら「物理を切り替えました」が残る）。
    editor_.setStatus("新しいテレインを作りました（保存すると書きます）");
    // 名前は base、空ならシーン名（どちらも空なら uniqueName が "terrain"）。
    publishNewTerrain(desc, flatTerrainField(desc), base.empty() ? editor_.sceneFile() : base);
}

void Scene::openTerrain(const std::string& name) {
    if (hasTerrain_ && wt::sameTerrainName(terrain_.data, name)) return;  // 開いている
    // 一覧の正式な綴り（"HILL" と送られても "hill"）。名前だけの解決（規則 D）は
    // 名前のファイルがあればその名前を返す。エクスプローラーで足したファイルも
    // 開けるよう、引く前に一覧を取り直す。
    refreshTerrainFiles();
    const std::string file = terrainLibrary_->resolve(name, wt::TerrainGuid{}).file;
    ed::TerrainDesc desc = terrain_;  // 位置と表面は今のもの
    std::string reason = "terrain '" + name + "' is not in the list";
    auto field = file.empty() ? nullptr : readTerrainFile(*terrainLibrary_, file, desc, reason);
    if (!field) {
        LOGW("scene", "terrain: cannot open '%s': %s", name.c_str(), reason.c_str());
        editor_.setStatus("テレイン " + name + " を開けません（コンソール参照）");
        return;
    }
    editor_.setStatus("テレイン " + desc.data + " を開きました");
    publishTerrainField(desc, std::move(field), TerrainHistory::Clear,
                        TerrainFileState::Saved);
}

void Scene::renameTerrain(const std::string& from, const std::string& to) {
    const bool current = hasTerrain_ && wt::sameTerrainName(terrain_.data, from);
    wt::TerrainRenameResult r;
    const std::string toName = wizengine::sanitizeSafeName(to);
    if (hasTerrain_ && !current && wt::sameTerrainName(toName, terrain_.data)) {
        // 今のテレインの名前（未保存でファイルが無くても）は使用中: 改名させると
        // 次の保存でそのファイルを今の格子で上書きしてしまう。
        r.status = wt::TerrainRenameStatus::AlreadyExists;
        r.name = toName;
        r.reason = "'" + toName + "' is the name of the current terrain";
    } else {
        r = terrainLibrary_->rename(from, to);
    }
    // 今のテレインの参照（data）だけを新しい名前へ。格子はそのまま。
    auto retarget = [this](const std::string& name) {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        terrain_.data = name;
    };
    switch (r.status) {
        case wt::TerrainRenameStatus::Ok:
            if (current) retarget(r.name);
            LOGI("scene", "terrain renamed: '%s' -> '%s'", from.c_str(), r.name.c_str());
            editor_.setStatus("テレインの名前を " + r.name + " に変えました");
            return;
        case wt::TerrainRenameStatus::NotFound:
            // まだ保存していない今のテレイン（ファイルが無い）: 参照だけ変える
            // （NotFound なら r.name は正規化済みで空いている）。
            if (current) {
                retarget(r.name);
                editor_.setStatus("テレインの名前を " + r.name +
                                  " に変えました（未保存 - 保存で書きます）");
                return;
            }
            editor_.setStatus("テレイン " + from + " が見つかりません");
            break;
        case wt::TerrainRenameStatus::InvalidName:
            editor_.setStatus("テレインの名前が不正です（英数字と _ - のみ）");
            break;
        case wt::TerrainRenameStatus::SameName:
            editor_.setStatus("同じ名前です");
            break;
        case wt::TerrainRenameStatus::AlreadyExists:
            editor_.setStatus("テレイン " + r.name + " は既にあります（名前は変えていません）");
            break;
        case wt::TerrainRenameStatus::IoError:
            editor_.setStatus("テレインのファイルの名前を変えられません（コンソール参照）");
            break;
    }
    LOGW("scene", "terrain rename '%s' -> '%s': %s", from.c_str(), to.c_str(),
         r.reason.c_str());
}

// ---- 保存 ---------------------------------------------------------------------

bool Scene::saveTerrainData(const std::string& sceneName, std::string& written,
                            std::string& reason) {
    written.clear();
    const std::shared_ptr<const wt::HeightField> field = hasTerrain_ ? terrainField_ : nullptr;
    if (!field) return true;
    // 名前・背番号の欠けている方だけをここで決める（XML に data="" を出さない。
    // 正しい背番号・名前は捨てない）。
    if (terrain_.data.empty() || terrain_.guid.empty()) {
        ed::TerrainDesc d = terrain_;
        fillTerrainIdentity(d, sceneName);
        std::lock_guard<std::mutex> lk(objectsMutex_);
        terrain_.data = d.data;
        terrain_.guid = d.guid;
    }
    wt::TerrainGuid guid;
    wt::guidFromHex(terrain_.guid, guid);
    // 変えたときだけ書く（共有しているファイルを、触っていないシーンの保存で
    // 書き換えない）。ファイルが無ければ必ず書く。
    const std::filesystem::path path = terrainLibrary_->pathFor(terrain_.data);
    std::error_code ec;
    if (!terrainDirty_ && !path.empty() && std::filesystem::exists(path, ec)) return true;
    if (!wt::writeFile(path, *field, float(terrain_.height), guid, reason)) return false;
    terrainDirty_ = false;  // 保存の成功（publish の外で戻す唯一の場所）
    refreshTerrainFiles();
    written = terrain_.data;
    LOGI("scene", "terrain '%s' written: %s", written.c_str(), path.u8string().c_str());
    return true;
}

// ---- ブラウザ ---------------------------------------------------------------

nlohmann::json Scene::terrainJson() const {
    nlohmann::json j;
    // 値はテレインが無いときも直前のものを出す（欄を空にしない）。ただし
    // 有効にし直すと新規 = 既定の大きさの平らなテレインになる（位置と表面だけが
    // この値。applyTerrainOp の enabled:true）。
    j["hasTerrain"] = hasTerrain_;
    j["terrain"] = ed::toJson(terrain_);
    j["terrainDirty"] = terrainDirty_.load();
    nlohmann::json files = nlohmann::json::array();
    for (const wt::TerrainEntry& e : terrainLibrary_->entries()) files.push_back(toJson(e));
    j["terrainFiles"] = std::move(files);
    return j;
}

void Scene::refreshTerrainFiles() {
    for (const std::string& w : terrainLibrary_->refresh())
        LOGW("scene", "terrain files: %s", w.c_str());
}

// edit.terrain（PHYSICS スレッド）。applyEditorOp の "terrain" 分岐から呼ぶ。
// キーで分岐: refresh / rename{from,to} / open / generate{seed,...} / undo /
// enabled(+name) / それ以外は部分更新。
// モードによらず受ける（名前の変更もシミュレート中に受ける）。
void Scene::applyTerrainOp(const EditorState::Op& op) {
    const nlohmann::json& a = op.args;
    if (const auto it = a.find("refresh");
        it != a.end() && it->is_boolean() && it->get<bool>()) {
        // ブラウザのタブに戻ったとき（focus）に来る。ステータスは変えない。
        refreshTerrainFiles();
        return;
    }
    if (const auto it = a.find("rename"); it != a.end() && it->is_object()) {
        renameTerrain(ed::jsonString(*it, "from", ""), ed::jsonString(*it, "to", ""));
        return;
    }
    if (a.contains("open")) {
        openTerrain(ed::jsonString(a, "open", ""));
        return;
    }
    // generate / undo は形がおかしくても部分更新へ落とさない（成功に見せない）。
    if (const auto it = a.find("generate"); it != a.end()) {
        if (!it->is_object()) {
            editor_.setStatus("ノイズの指定が正しくありません（generate はオブジェクト）");
            return;
        }
        // 送られなかったキーは既定値（ノイズの値はテレインに覚えない）。
        generateTerrainNoise(ed::terrainNoiseFromJson(*it, ed::TerrainNoiseParams{}));
        return;
    }
    if (const auto it = a.find("undo"); it != a.end()) {
        if (!it->is_boolean() || !it->get<bool>()) {
            editor_.setStatus("取り消しの指定が正しくありません（undo は true）");
            return;
        }
        undoTerrain();
        return;
    }
    if (const auto it = a.find("enabled"); it != a.end() && it->is_boolean()) {
        if (!it->get<bool>()) {
            // 外す（.terrain ファイルは消さない）。ステータスは publish の前。
            editor_.setStatus(terrainDirty_ ? "テレインを外しました（保存していない変更は"
                                              "捨てました。ファイルは残ります）"
                                            : "テレインを外しました（ファイルは残ります）");
            publishTerrainField(terrain_, nullptr, TerrainHistory::Clear,
                                TerrainFileState::Saved);
            return;
        }
        if (!hasTerrain_) {
            // 新規: 同じパッチの他のキー（size など）は使わず、既定の大きさの
            // 平らなテレインを今の位置と表面で作る（name だけを使う）。
            newTerrain(ed::jsonString(a, "name", ""));
            return;
        }
    }
    // 部分更新: 送られてきたキーだけ上書き（edit.ground と同じ）。テレインが無い
    // 間に送った値は位置と表面だけが次の新規に効く（形は新規の既定値）。
    // 当たり判定はモードによらずその場で張り直す（publishTerrainField）。
    const ed::TerrainDesc next = ed::clampTerrain(ed::terrainFromJson(a, terrain_));
    if (!hasTerrain_) {
        editor_.setStatus("テレインの設定を更新（無効のまま）");
    } else if (next.resolution != terrain_.resolution) {
        // 再サンプル（HeightField::resampled）の性質を一言。
        editor_.setStatus("テレインを更新（解像度を下げると細い起伏は消え、穴は少し"
                          "太ります）");
    } else {
        editor_.setStatus("テレインを更新");
    }
    reshapeTerrain(next);
}
