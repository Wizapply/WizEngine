// グローバル（ライブラリ）のアセット: 計算式・プレハブの読み書きと、ブラウザの
// edit.lib.* / edit.mesh.add。イベントの共通化（SceneEvents.cpp の
// applySharedEventOp）と同じ流儀で、種類を増やしたもの。
//
//   ローカル  = このシーンの文書に中身を書く（今までどおり）
//   グローバル = assets/<種類>/<名前>.xml に中身があり、シーンの文書は
//                <formula name file/> / <prefab name file/> の参照だけを書く。
//                どのシーンからも読み込んで使え、編集は（保存で）全シーンに効く
//
// メッシュ（.glb / .gltf）は元々 assets/ のファイルを <mesh> で宣言して使う
// ので、グローバル = assets/ のファイル一覧、ローカル = このシーンの宣言。
// edit.mesh.add がファイルをこのシーンに宣言する（同じファイルの宣言があれば
// それを使う）。
//
// 変更の目印: 読み込んだとき / 書いたときのファイルの中身（正規化した XML の
// テキスト）を libBaseline_ に覚え、保存で今の中身から作ったテキストと比べて
// 違うものだけ書く = 触っていないものは書き戻さない（他のシーンの編集を巻き
// 戻さない）。読めなかったもの（覚えが無い）は書かない（空で上書きしない）。
// すべて PHYSICS thread。
#include "scene/SceneInternal.h"
#include "scene/SharedAssetFile.h"

#include "core/Trash.h"

#include <filesystem>

using namespace scene_detail;
namespace fml = wizengine::formula;

namespace {
bool argIsTrue(const nlohmann::json& a, const char* key) {
    const auto it = a.find(key);
    return it != a.end() && it->is_boolean() && it->get<bool>();
}
bool libraryFileExists(const std::string& file) {
    std::error_code ec;
    return !file.empty() && std::filesystem::exists(wizengine::assetPath(file), ec);
}
}  // namespace

// ---- 読み書き ------------------------------------------------------------------
bool Scene::readLibraryFormula(fml::FormulaGraphDesc& g, std::string& reason) {
    const std::string name = g.name, file = g.file;
    if (file.empty() || !ed::assetFileAllowed(file)) {
        reason = "'" + file + "' is not an assets-relative path";
        return false;
    }
    std::string text;
    if (!wizengine::sharedasset::readTextFile(wizengine::assetPath(file), text, reason)) return false;
    fml::FormulaGraphDesc got;
    std::string error;
    std::vector<std::string> warnings;
    if (!ed::parseFormulaLibraryXml(text, got, error, &warnings)) {
        reason = file + ": " + error;
        return false;
    }
    for (const auto& w : warnings) LOGW("scene", "formula file '%s': %s", file.c_str(), w.c_str());
    got.name = name;  // 参照名はシーンの側が正（制御器がこの名前で指している）
    got.file = file;
    g = std::move(got);
    libBaseline_["formula:" + name] = ed::formulaToLibraryXmlText(g);
    return true;
}

bool Scene::readLibraryPrefab(ed::PrefabDesc& d, std::string& reason) {
    const std::string name = d.name, file = d.file;
    if (file.empty() || !ed::assetFileAllowed(file)) {
        reason = "'" + file + "' is not an assets-relative path";
        return false;
    }
    std::string text;
    if (!wizengine::sharedasset::readTextFile(wizengine::assetPath(file), text, reason)) return false;
    ed::PrefabDesc got;
    std::vector<ed::MeshAssetDesc> decls;
    std::string error;
    std::vector<std::string> warnings;
    if (!ed::parsePrefabLibraryXml(text, got, decls, error, &warnings)) {
        reason = file + ": " + error;
        return false;
    }
    for (const auto& w : warnings) LOGW("scene", "prefab file '%s': %s", file.c_str(), w.c_str());
    // 部品が使うメッシュの宣言を、このシーンに無ければ足す（同じ名前があれば
    // シーンの宣言が勝つ）。
    {
        std::lock_guard<std::mutex> lk(objectsMutex_);
        for (const auto& m : decls) {
            bool have = false;
            for (const auto& a : meshes_) have = have || a.desc.name == m.name;
            if (have) continue;
            MeshAsset a;
            a.desc = m;
            meshes_.push_back(std::move(a));
        }
    }
    got.name = name;
    got.file = file;
    d = std::move(got);
    libBaseline_["prefab:" + name] = ed::prefabToLibraryXmlText(d, meshDescs());
    return true;
}

std::vector<ed::MeshAssetDesc> Scene::meshDescs() {
    std::lock_guard<std::mutex> lk(objectsMutex_);
    std::vector<ed::MeshAssetDesc> out;
    for (const auto& m : meshes_) out.push_back(m.desc);
    return out;
}

void Scene::resolveLibraryAssets(std::vector<fml::FormulaGraphDesc>& formulas,
                                 std::vector<ed::PrefabDesc>& prefabs, std::size_t& missing) {
    for (auto& g : formulas) {
        if (g.file.empty()) continue;
        std::string reason;
        if (!readLibraryFormula(g, reason)) {
            LOGW("scene", "global formula '%s': %s - it is empty until the file is back",
                 g.name.c_str(), reason.c_str());
            g.nodes.clear();
            g.wires.clear();
            ++missing;
        }
    }
    for (auto& d : prefabs) {
        if (d.file.empty()) continue;
        std::string reason;
        if (!readLibraryPrefab(d, reason)) {
            LOGW("scene", "global prefab '%s': %s - it is empty until the file is back",
                 d.name.c_str(), reason.c_str());
            d.parts.clear();
            ++missing;
        }
    }
}

bool Scene::saveLibraryAssets(const ed::SceneDocument& doc, std::string& written,
                              std::string& reason) {
    written.clear();
    bool any = false;
    for (const auto& g : doc.formulas) {
        if (g.file.empty()) continue;
        const std::string key = "formula:" + g.name;
        const auto it = libBaseline_.find(key);
        if (it == libBaseline_.end()) continue;  // 読めなかった = 空で上書きしない
        const std::string text = ed::formulaToLibraryXmlText(g);
        if (text == it->second) continue;       // 変更なし
        if (!wizengine::sharedasset::writeTextFile(wizengine::assetPath(g.file), text, reason)) return false;
        it->second = text;
        written += (written.empty() ? "" : " ") + g.name;
        any = true;
    }
    for (const auto& d : doc.prefabs) {
        if (d.file.empty()) continue;
        const std::string key = "prefab:" + d.name;
        const auto it = libBaseline_.find(key);
        if (it == libBaseline_.end()) continue;
        const std::string text = ed::prefabToLibraryXmlText(d, doc.meshes);
        if (text == it->second) continue;
        if (!wizengine::sharedasset::writeTextFile(wizengine::assetPath(d.file), text, reason)) return false;
        it->second = text;
        written += (written.empty() ? "" : " ") + d.name;
        any = true;
    }
    if (any) editor_.refreshLibraries();
    return true;
}

// ---- ブラウザの操作 ------------------------------------------------------------
// edit.lib.share / unshare / import / delete / refresh（kind = event / formula /
// prefab）と edit.mesh.add。扱ったら true。
bool Scene::applyLibraryOp(const EditorState::Op& op) {
    const nlohmann::json& a = op.args;
    if (op.kind == "mesh.add") {
        addMeshDeclaration(a);
        return true;
    }
    if (op.kind.rfind("lib.", 0) != 0) return false;
    const std::string what = op.kind.substr(4);
    const std::string kind = ed::jsonString(a, "kind", "");
    const std::string name = ed::sanitizeEventName(ed::jsonString(a, "name", ""));

    if (what == "refresh") {
        editor_.refreshLibraries();  // ブラウザのタブに戻ったとき。ステータスは変えない
        return true;
    }
    // イベントは既存の共通化の口へ（event.share などと同じ）。
    if (kind == "event") {
        EditorState::Op ev = op;
        ev.kind = "event." + std::string(what == "delete" ? "libdelete" : what);
        applySharedEventOp(ev);
        return true;
    }
    if (kind != "formula" && kind != "prefab") return true;
    const bool isFormula = kind == "formula";
    const std::string label = isFormula ? "計算式" : "プレハブ";
    const std::string file = isFormula ? EditorState::formulaLibraryFile(name)
                                       : EditorState::prefabLibraryFile(name);
    if (name.empty() || file.empty()) return true;

    // 今のシーンの同じ名前のアセット（無ければ found = false）。
    auto formulas = editor_.formulaAssets();
    auto prefabs = editor_.prefabAssets();
    fml::FormulaGraphDesc* g = nullptr;
    ed::PrefabDesc* d = nullptr;
    for (auto& f : formulas) if (f.name == name) g = &f;
    for (auto& p : prefabs) if (p.name == name) d = &p;
    const bool found = isFormula ? g != nullptr : d != nullptr;
    const std::string curFile = !found ? std::string() : isFormula ? g->file : d->file;

    // グローバルにする: 中身をライブラリへ書き、以後は参照にする。
    if (what == "share") {
        if (!found) return true;
        if (!curFile.empty()) {
            editor_.setStatus(label + " " + name + " は既にグローバルです");
            return true;
        }
        if (libraryFileExists(file) && !argIsTrue(a, "overwrite")) {
            editor_.setStatus("グローバルに同じ名前の" + label + " " + name +
                              " があります（もう一度選ぶと上書きを確認します）");
            return true;
        }
        std::string reason, text;
        if (isFormula) {
            g->file = file;
            text = ed::formulaToLibraryXmlText(*g);
        } else {
            d->file = file;
            text = ed::prefabToLibraryXmlText(*d, meshDescs());
        }
        if (!wizengine::sharedasset::writeTextFile(wizengine::assetPath(file), text, reason)) {
            LOGW("scene", "%s share '%s': %s", kind.c_str(), name.c_str(), reason.c_str());
            editor_.setStatus(label + "をグローバルにできません（コンソール参照）");
            return true;
        }
        libBaseline_[kind + ":" + name] = text;
        if (isFormula) {
            editor_.setFormulaAssets(std::move(formulas));
        } else {
            editor_.setPrefabAssets(std::move(prefabs));
        }
        editor_.refreshLibraries();
        editor_.setStatus(label + " " + name + " をグローバルにしました（" + file +
                          "。他のシーンからも使えます）");
        return true;
    }

    // ローカルに戻す: 中身はそのまま、次の保存でシーンの XML に埋まる。
    // ライブラリのファイルは消さない（他のシーンが使っているかもしれない）。
    if (what == "unshare") {
        if (!found || curFile.empty()) return true;
        if (isFormula) {
            g->file.clear();
            editor_.setFormulaAssets(std::move(formulas));
        } else {
            d->file.clear();
            editor_.setPrefabAssets(std::move(prefabs));
        }
        libBaseline_.erase(kind + ":" + name);
        editor_.setStatus(label + " " + name + " をこのシーン専用（ローカル）にしました"
                          "（保存でシーンに埋め込みます。グローバルのファイルは残ります）");
        return true;
    }

    // グローバルのものを今のシーンで使えるようにする（参照のまま）。
    if (what == "import") {
        if (found) {
            editor_.setStatus("このシーンには既に" + label + " " + name + " があります");
            return true;
        }
        std::string reason;
        if (isFormula) {
            fml::FormulaGraphDesc ng;
            ng.name = name;
            ng.file = file;
            if (!readLibraryFormula(ng, reason) || !editor_.addFormulaAsset(std::move(ng))) {
                LOGW("scene", "formula import '%s': %s", name.c_str(), reason.c_str());
                editor_.setStatus("計算式 " + name + " を読み込めません（コンソール参照）");
                editor_.refreshLibraries();
                return true;
            }
            editor_.setStatus("計算式 " + name +
                              " をこのシーンで使えるようにしました（制御器にすると回ります）");
        } else {
            ed::PrefabDesc nd;
            nd.name = name;
            nd.file = file;
            if (!readLibraryPrefab(nd, reason) || !editor_.addPrefabAsset(nd)) {
                LOGW("scene", "prefab import '%s': %s", name.c_str(), reason.c_str());
                editor_.setStatus("プレハブ " + name + " を読み込めません（コンソール参照）");
                editor_.refreshLibraries();
                return true;
            }
            editor_.setStatus("プレハブ " + name +
                              " をこのシーンで使えるようにしました（オブジェクトに付けて使います）");
        }
        return true;
    }

    // ライブラリのファイルをごみ箱へ。今のシーンが使っているものは消さない。
    if (what == "delete") {
        if (editor_.mode() == ed::AppMode::Simulate) {
            editor_.setStatus("グローバルからの削除はエディタモードでだけできます");
            return true;
        }
        if (found && curFile == file) {
            editor_.setStatus("このシーンが使っている" + label +
                              "は削除できません（ローカルにするか消してから）");
            return true;
        }
        std::string reason;
        if (!wizengine::moveToTrash(std::filesystem::path(wizengine::assetPath(file)), reason)) {
            LOGW("scene", "%s library delete '%s': %s", kind.c_str(), name.c_str(),
                 reason.c_str());
            editor_.setStatus(label + " " + name + " をごみ箱へ移せません（コンソール参照）");
        } else {
            LOGI("scene", "%s '%s' moved to the trash", kind.c_str(), name.c_str());
            editor_.setStatus("グローバルの" + label + " " + name + " をごみ箱へ移しました");
        }
        editor_.refreshLibraries();
        return true;
    }
    return true;
}

// assets/ のメッシュファイルをこのシーンに宣言する（{file, name?, scale?}）。
// 同じファイルの宣言があれば何もしない（ブラウザはその名前で置く）。
void Scene::addMeshDeclaration(const nlohmann::json& a) {
    const std::string file = ed::jsonString(a, "file", "");
    if (!ed::assetFileAllowed(file)) {
        editor_.setStatus("メッシュを宣言できません: assets/ 相対のファイル名ではありません");
        return;
    }
    std::string name = ed::sanitizeEventName(ed::jsonString(a, "name", ""));
    if (name.empty()) {
        name = ed::sanitizeEventName(std::filesystem::path(file).stem().string());
    }
    if (name.empty()) name = "mesh";
    const double scale = ed::jsonNumber(a, "scale", 1.0);
    std::lock_guard<std::mutex> lk(objectsMutex_);
    for (const auto& m : meshes_) {
        if (m.desc.file == file) return;  // 宣言済み
    }
    // 名前が他のファイルで使われていれば番号を足す。
    std::string unique = name;
    for (int n = 2;; ++n) {
        bool used = false;
        for (const auto& m : meshes_) used = used || m.desc.name == unique;
        if (!used) break;
        unique = name + "_" + std::to_string(n);
    }
    MeshAsset m;
    m.desc.name = unique;
    m.desc.file = file;
    m.desc.scale = scale > 1e-9 ? scale : 1.0;
    meshes_.push_back(std::move(m));
    editor_.setStatus("メッシュ " + unique + " をこのシーンに宣言しました（" + file + "）");
}
