// グローバル（ライブラリ）のアセット: 計算式・プレハブの読み書きと、ブラウザの
// edit.lib.* / edit.mesh.add。イベントの共通化（SceneEvents.cpp の
// applySharedEventOp）と同じ流儀で、種類を増やしたもの。共用にする・ローカルにする・
// 読み込む・削除の判断の流れは計算式・プレハブ・UI で 1 本（scene/LibraryFlow の
// applyLibraryFlow）で、ここは種類ごとに違うところ（どれか・中身の書式・今のシーンへの
// 入れ方）の組（LibraryKind）を作って渡す。UI の部品は scene/UiLibraryOps。保存の
// 「変わったものだけ書く」も 3 種を同じ手順で行う。
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
// テキスト）を libBaseline_（LibraryBaseline）に覚え、保存で今の中身から作った
// テキストと比べて違うものだけ書く = 触っていないものは書き戻さない（他のシーンの
// 編集を巻き戻さない）。読めなかったもの（覚えが無い）は書かない（空で上書きしない）。
// すべて PHYSICS thread。
#include "scene/SceneInternal.h"
#include "scene/LibraryFlow.h"
#include "scene/SharedAssetFile.h"
#include "scene/UiFiles.h"
#include "scene/UiLibraryOps.h"

#include <filesystem>
#include <functional>

using namespace scene_detail;
namespace fml = wizengine::formula;

namespace {
using wizengine::LibraryKind;

// 種類の名前（edit.lib.* の kind・ログ・覚えのキー）。UI は uifiles::kUiLibraryKind。
constexpr const char* kFormulaKind = "formula";
constexpr const char* kPrefabKind = "prefab";

bool argIsTrue(const nlohmann::json& a, const char* key) {
    const auto it = a.find(key);
    return it != a.end() && it->is_boolean() && it->get<bool>();
}

}  // namespace

// ---- 読み書き ------------------------------------------------------------------
bool Scene::readLibraryFormula(fml::FormulaGraphDesc& g, std::string& reason) {
    const std::string name = g.name, file = g.file;
    // 読めなければ前の覚えも消す（覚えが「読めたか」の持ち主。古い覚えが残ると、空の中身を
    // 保存でファイルへ書いたり、ローカルにできたりする）。
    auto fail = [this, &name]() {
        libBaseline_.forget(kFormulaKind, name);
        return false;
    };
    std::string text;
    if (!wizengine::sharedasset::readAssetText(file, text, reason)) return fail();
    fml::FormulaGraphDesc got;
    std::string error;
    std::vector<std::string> warnings;
    if (!ed::parseFormulaLibraryXml(text, got, error, &warnings)) {
        reason = file + ": " + error;
        return fail();
    }
    for (const auto& w : warnings) LOGW("scene", "formula file '%s': %s", file.c_str(), w.c_str());
    got.name = name;  // 参照名はシーンの側が正（制御器がこの名前で指している）
    got.file = file;
    g = std::move(got);
    libBaseline_.remember(kFormulaKind, name, ed::formulaToLibraryXmlText(g));
    return true;
}

bool Scene::readLibraryPrefab(ed::PrefabDesc& d, std::string& reason) {
    const std::string name = d.name, file = d.file;
    // 読めなければ前の覚えも消す（覚えが「読めたか」の持ち主。古い覚えが残ると、空の中身を
    // 保存でファイルへ書いたり、ローカルにできたりする）。
    auto fail = [this, &name]() {
        libBaseline_.forget(kPrefabKind, name);
        return false;
    };
    std::string text;
    if (!wizengine::sharedasset::readAssetText(file, text, reason)) return fail();
    ed::PrefabDesc got;
    std::vector<ed::MeshAssetDesc> decls;
    std::string error;
    std::vector<std::string> warnings;
    if (!ed::parsePrefabLibraryXml(text, got, decls, error, &warnings)) {
        reason = file + ": " + error;
        return fail();
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
    libBaseline_.remember(kPrefabKind, name, ed::prefabToLibraryXmlText(d, meshDescs()));
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
        const std::string* known = libBaseline_.find(kind, name);
        if (!known) return true;          // 読めなかった = 空で上書きしない
        if (text == *known) return true;  // 変更なし
        if (!wizengine::sharedasset::writeAssetText(file, text, reason)) return false;
        libBaseline_.remember(kind, name, text);
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
        k = wizengine::uilibrary::libraryKind(ui_, a, libBaseline_);
    } else {
        return true;
    }
    const wizengine::LibraryResult r =
        wizengine::applyLibraryFlow(k, what, name, argIsTrue(a, "overwrite"),
                                    editor_.mode() != ed::AppMode::Simulate, libBaseline_);
    // UI の「読めなかった」の印は覚えの写し（共用にした・読み込んだで覚えが変わる）。
    if (kind == wizengine::uifiles::kUiLibraryKind) {
        wizengine::uifiles::syncReadable(ui_, libBaseline_);
    }
    if (r.refresh) editor_.refreshLibraries();
    if (!r.status.empty()) editor_.setStatus(r.status);
    return true;
}

// ---- 種類ごとの関数の組 ----------------------------------------------------------
// 計算式とプレハブは同じ形（EditorState の一覧の 1 つ）なので、部品は bindListKind が組み、
// ここは一覧の出し入れとファイルの書式、読み込み方だけを渡す。
LibraryKind Scene::formulaLibraryKind() {
    LibraryKind k;
    k.id = kFormulaKind;
    k.label = "計算式";
    k.importHint = "制御器にすると回ります";
    k.libraryFile = &EditorState::formulaLibraryFile;
    wizengine::bindListKind<fml::FormulaGraphDesc>(
        k, [this] { return editor_.formulaAssets(); },
        [this](std::vector<fml::FormulaGraphDesc> v) { editor_.setFormulaAssets(std::move(v)); },
        [](const fml::FormulaGraphDesc& g) { return ed::formulaToLibraryXmlText(g); });
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
    wizengine::bindListKind<ed::PrefabDesc>(
        k, [this] { return editor_.prefabAssets(); },
        [this](std::vector<ed::PrefabDesc> v) { editor_.setPrefabAssets(std::move(v)); },
        [this](const ed::PrefabDesc& d) { return ed::prefabToLibraryXmlText(d, meshDescs()); });
    k.import = [this](const std::string& n, const std::string& file, std::string& reason) {
        ed::PrefabDesc d;
        d.name = n;
        d.file = file;
        return readLibraryPrefab(d, reason) && editor_.addPrefabAsset(d);
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
