// グローバル（ライブラリ）のアセット: 計算式・プレハブの読み書きと、ブラウザの
// edit.lib.* / edit.mesh.add。イベントの共通化（SceneEvents.cpp の
// applySharedEventOp）と同じ流儀で、種類を増やしたもの。共用にする・ローカルにする・
// 読み込む・削除の判断の流れは計算式・プレハブ・UI で 1 本（applyLibraryFlow）で、
// 種類ごとに違うところ（どれか・中身の書式・今のシーンへの入れ方）だけを関数の組
// （LibraryKind）で渡す。UI の部品は scene/UiLibraryOps。保存の「変わったものだけ
// 書く」も 3 種を同じ手順で行う。
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
#include "scene/LibraryStatus.h"
#include "scene/SharedAssetFile.h"
#include "scene/UiFiles.h"
#include "scene/UiLibraryOps.h"

#include <filesystem>
#include <functional>
#include <map>

using namespace scene_detail;
namespace fml = wizengine::formula;
namespace libstatus = wizengine::libstatus;

namespace wizengine {
// グローバル（ライブラリ）のアセットの種類ごとに違うところ（計算式・プレハブ・UI）。
// 判断の流れ（applyLibraryFlow）は 1 本で、ここに入れた関数だけが種類ごと。
struct LibraryKind {
    const char* id = "";    // "formula" / "prefab" / "ui"（ログと覚えのキー）
    std::string label;      // ステータスの呼び名（libstatus の label）
    std::string importHint; // 読み込んだ後の案内（libstatus::imported の hint）
    // 名前 → ライブラリのファイル（assets/ 相対。名前が不正なら空）。
    std::function<std::string(const std::string& name)> libraryFile;
    // 今のシーンにあれば true と今の file（空 = ローカル）。
    std::function<bool(const std::string& name, std::string& file)> find;
    // 共用にする対象を決める（name を書き換えてよい）。断るなら false と refusal。
    std::function<bool(std::string& name, std::string& file, std::string& refusal)> findForShare;
    // 共用にしたときにファイルへ書く中身（file を付けた形）と、それを今のシーンへ入れる。
    std::function<std::string(const std::string& name, const std::string& file)> sharedText;
    std::function<void(const std::string& name, const std::string& file)> commitShare;
    // ローカルにする（参照をやめる。中身はシーンに残す）。
    std::function<void(const std::string& name)> unshare;
    // ライブラリから読んで足す（読めたら覚えも入れる = 読む側が覚えの唯一の入れ口。
    // シーンの読込と同じ）。
    std::function<bool(const std::string& name, const std::string& file, std::string& reason)>
        import;
};

}  // namespace wizengine

namespace {
using wizengine::LibraryKind;

// 種類の名前（edit.lib.* の kind・ログ・覚えのキー）。UI は uifiles::kUiLibraryKind。
constexpr const char* kFormulaKind = "formula";
constexpr const char* kPrefabKind = "prefab";

bool argIsTrue(const nlohmann::json& a, const char* key) {
    const auto it = a.find(key);
    return it != a.end() && it->is_boolean() && it->get<bool>();
}

// 結果（ステータスと、ライブラリの一覧を取り直すか）。
struct LibraryResult {
    std::string status;
    bool refresh = false;
};

// edit.lib.share / unshare / import / delete の判断の流れ（唯一の定義）。
LibraryResult applyLibraryFlow(const LibraryKind& k, const std::string& what, std::string name,
                               bool overwrite, bool editorMode,
                               std::map<std::string, std::string>& baseline) {
    LibraryResult r;
    std::string curFile;
    // グローバルにする: 中身をライブラリへ書き、以後は参照にする。
    if (what == "share") {
        if (!k.findForShare(name, curFile, r.status)) return r;
        const std::string file = k.libraryFile(name);
        if (name.empty() || file.empty()) return r;
        if (!curFile.empty()) {
            r.status = libstatus::alreadyGlobal(k.label, name);
            return r;
        }
        if (wizengine::sharedasset::libraryFileExists(file) && !overwrite) {
            r.status = libstatus::globalNameTaken(k.label, name);
            return r;
        }
        const std::string text = k.sharedText(name, file);
        std::string reason;
        if (!wizengine::sharedasset::writeAssetText(file, text, reason)) {
            LOGW("scene", "%s share '%s': %s", k.id, name.c_str(), reason.c_str());
            r.status = libstatus::shareFailed(k.label);
            return r;
        }
        k.commitShare(name, file);
        baseline[wizengine::sharedasset::libraryBaselineKey(k.id, name)] = text;
        r.refresh = true;
        r.status = libstatus::shared(k.label, name, file);
        return r;
    }
    const std::string file = k.libraryFile(name);
    if (name.empty() || file.empty()) return r;
    const bool found = k.find(name, curFile);

    // ローカルに戻す: 中身はそのまま、次の保存でシーンの XML に埋まる。
    // ライブラリのファイルは消さない（他のシーンが使っているかもしれない）。
    if (what == "unshare") {
        if (!found || curFile.empty()) return r;
        k.unshare(name);
        baseline.erase(wizengine::sharedasset::libraryBaselineKey(k.id, name));
        r.status = libstatus::unshared(k.label, name);
        return r;
    }

    // グローバルのものを今のシーンで使えるようにする（参照のまま）。
    if (what == "import") {
        if (found) {
            r.status = libstatus::alreadyInScene(k.label, name);
            return r;
        }
        std::string reason;
        if (!k.import(name, file, reason)) {
            LOGW("scene", "%s import '%s': %s", k.id, name.c_str(), reason.c_str());
            r.status = libstatus::importFailed(k.label, name);
            r.refresh = true;
            return r;
        }
        r.status = libstatus::imported(k.label, name, k.importHint);
        return r;
    }

    // ライブラリのファイルをごみ箱へ。今のシーンが使っているものは消さない。
    if (what == "delete") {
        if (!editorMode) {
            r.status = libstatus::deleteOnlyInEditor();
            return r;
        }
        if (found && curFile == file) {
            r.status = libstatus::deleteInUse(k.label);
            return r;
        }
        std::string reason;
        if (!wizengine::sharedasset::trashLibraryFile(file, reason)) {
            LOGW("scene", "%s library delete '%s': %s", k.id, name.c_str(), reason.c_str());
            r.status = libstatus::trashFailed(k.label, name);
        } else {
            LOGI("scene", "%s '%s' moved to the trash", k.id, name.c_str());
            r.status = libstatus::trashed(k.label, name);
        }
        r.refresh = true;
        return r;
    }
    return r;
}
}  // namespace

// ---- 読み書き ------------------------------------------------------------------
bool Scene::readLibraryFormula(fml::FormulaGraphDesc& g, std::string& reason) {
    const std::string name = g.name, file = g.file;
    std::string text;
    if (!wizengine::sharedasset::readAssetText(file, text, reason)) return false;
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
    libBaseline_[wizengine::sharedasset::libraryBaselineKey(kFormulaKind, name)] =
        ed::formulaToLibraryXmlText(g);
    return true;
}

bool Scene::readLibraryPrefab(ed::PrefabDesc& d, std::string& reason) {
    const std::string name = d.name, file = d.file;
    std::string text;
    if (!wizengine::sharedasset::readAssetText(file, text, reason)) return false;
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
    libBaseline_[wizengine::sharedasset::libraryBaselineKey(kPrefabKind, name)] =
        ed::prefabToLibraryXmlText(d, meshDescs());
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
    // 1 つぶん: 覚えがあり（読めなかった = 空で上書きしない）、今の中身のテキストが
    // 覚えと違うときだけ書く。
    auto writeChanged = [&](const char* kind, const std::string& name, const std::string& file,
                            const std::string& text) {
        const auto it =
            libBaseline_.find(wizengine::sharedasset::libraryBaselineKey(kind, name));
        if (it == libBaseline_.end()) return true;  // 読めなかった = 空で上書きしない
        if (text == it->second) return true;        // 変更なし
        if (!wizengine::sharedasset::writeAssetText(file, text, reason)) return false;
        it->second = text;
        written += (written.empty() ? "" : " ") + name;
        any = true;
        return true;
    };
    for (const auto& g : doc.formulas) {
        if (g.file.empty()) continue;
        if (!writeChanged(kFormulaKind, g.name, g.file, ed::formulaToLibraryXmlText(g))) {
            return false;
        }
    }
    for (const auto& d : doc.prefabs) {
        if (d.file.empty()) continue;
        if (!writeChanged(kPrefabKind, d.name, d.file,
                          ed::prefabToLibraryXmlText(d, doc.meshes))) {
            return false;
        }
    }
    for (const auto& u : doc.uiAssets) {
        if (u.file.empty()) continue;
        if (!writeChanged(wizengine::uifiles::kUiLibraryKind, u.name, u.file,
                          wizengine::uifiles::libraryText(u))) {
            return false;
        }
    }
    if (any) editor_.refreshLibraries();
    return true;
}

// ---- ブラウザの操作 ------------------------------------------------------------
// edit.lib.share / unshare / import / delete / refresh（kind = event / formula /
// prefab / ui）と edit.mesh.add。扱ったら true。
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
    LibraryKind k;
    if (kind == kFormulaKind) {
        k = formulaLibraryKind();
    } else if (kind == kPrefabKind) {
        k = prefabLibraryKind();
    } else if (kind == wizengine::uifiles::kUiLibraryKind) {
        k = uiLibraryKind(a);
    } else {
        return true;
    }
    const LibraryResult r = applyLibraryFlow(k, what, name, argIsTrue(a, "overwrite"),
                                             editor_.mode() != ed::AppMode::Simulate,
                                             libBaseline_);
    if (r.refresh) editor_.refreshLibraries();
    if (!r.status.empty()) editor_.setStatus(r.status);
    return true;
}

// ---- 種類ごとの関数の組 ----------------------------------------------------------
LibraryKind Scene::formulaLibraryKind() {
    LibraryKind k;
    k.id = kFormulaKind;
    k.label = "計算式";
    k.importHint = "制御器にすると回ります";
    k.libraryFile = &EditorState::formulaLibraryFile;
    k.find = [this](const std::string& n, std::string& file) {
        for (const auto& f : editor_.formulaAssets()) {
            if (f.name != n) continue;
            file = f.file;
            return true;
        }
        return false;
    };
    k.findForShare = [find = k.find](std::string& n, std::string& file, std::string&) {
        return find(n, file);
    };
    k.sharedText = [this](const std::string& n, const std::string& file) {
        for (auto f : editor_.formulaAssets()) {
            if (f.name != n) continue;
            f.file = file;
            return ed::formulaToLibraryXmlText(f);
        }
        return std::string();
    };
    auto setFile = [this](const std::string& n, const std::string& file) {
        auto formulas = editor_.formulaAssets();
        for (auto& f : formulas) {
            if (f.name == n) f.file = file;
        }
        editor_.setFormulaAssets(std::move(formulas));
    };
    k.commitShare = setFile;
    k.unshare = [setFile](const std::string& n) { setFile(n, ""); };
    k.import = [this](const std::string& n, const std::string& file, std::string& reason) {
        fml::FormulaGraphDesc g;
        g.name = n;
        g.file = file;
        return readLibraryFormula(g, reason) && editor_.addFormulaAsset(std::move(g));
    };
    return k;
}

LibraryKind Scene::prefabLibraryKind() {
    LibraryKind k;
    k.id = kPrefabKind;
    k.label = "プレハブ";
    k.importHint = "オブジェクトに付けて使います";
    k.libraryFile = &EditorState::prefabLibraryFile;
    k.find = [this](const std::string& n, std::string& file) {
        for (const auto& d : editor_.prefabAssets()) {
            if (d.name != n) continue;
            file = d.file;
            return true;
        }
        return false;
    };
    k.findForShare = [find = k.find](std::string& n, std::string& file, std::string&) {
        return find(n, file);
    };
    k.sharedText = [this](const std::string& n, const std::string& file) {
        for (auto d : editor_.prefabAssets()) {
            if (d.name != n) continue;
            d.file = file;
            return ed::prefabToLibraryXmlText(d, meshDescs());
        }
        return std::string();
    };
    auto setFile = [this](const std::string& n, const std::string& file) {
        auto prefabs = editor_.prefabAssets();
        for (auto& d : prefabs) {
            if (d.name == n) d.file = file;
        }
        editor_.setPrefabAssets(std::move(prefabs));
    };
    k.commitShare = setFile;
    k.unshare = [setFile](const std::string& n) { setFile(n, ""); };
    k.import = [this](const std::string& n, const std::string& file, std::string& reason) {
        ed::PrefabDesc d;
        d.name = n;
        d.file = file;
        return readLibraryPrefab(d, reason) && editor_.addPrefabAsset(d);
    };
    return k;
}

LibraryKind Scene::uiLibraryKind(const nlohmann::json& args) {
    namespace ul = wizengine::uilibrary;
    LibraryKind k;
    k.id = wizengine::uifiles::kUiLibraryKind;
    k.label = ul::kLabel;
    k.importHint = ul::kImportHint;
    k.libraryFile = &wizengine::uifiles::libraryFile;
    k.find = [this](const std::string& n, std::string& file) { return ul::find(ui_, n, file); };
    k.findForShare = [this, args](std::string& n, std::string& file, std::string& refusal) {
        return ul::findForShare(ui_, args, n, file, refusal);
    };
    k.sharedText = [this, args](const std::string& n, const std::string& file) {
        return ul::sharedText(ui_, args, n, file);
    };
    k.commitShare = [this, args](const std::string& n, const std::string& file) {
        ul::commitShare(ui_, args, n, file);
    };
    k.unshare = [this](const std::string& n) { ul::unshare(ui_, n); };
    k.import = [this](const std::string& n, const std::string& file, std::string& reason) {
        std::string text;
        if (!ul::importFromLibrary(ui_, n, file, text, reason)) return false;
        libBaseline_[wizengine::sharedasset::libraryBaselineKey(
            wizengine::uifiles::kUiLibraryKind, n)] = text;
        return true;
    };
    return k;
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
