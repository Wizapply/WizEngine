#include "scene/LibraryFlow.h"

#include "core/Log.h"
#include "scene/LibraryStatus.h"
#include "scene/SharedAssetFile.h"

namespace wizengine {

LibraryResult applyLibraryFlow(const LibraryKind& k, const std::string& what, std::string name,
                               bool overwrite, bool editorMode, LibraryBaseline& baseline) {
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
        if (sharedasset::libraryFileExists(file) && !overwrite) {
            r.status = libstatus::globalNameTaken(k.label, name);
            return r;
        }
        const std::string text = k.sharedText(name, file);
        std::string reason;
        if (!sharedasset::writeAssetText(file, text, reason)) {
            LOGW("scene", "%s share '%s': %s", k.id, name.c_str(), reason.c_str());
            r.status = libstatus::shareFailed(k.label);
            return r;
        }
        if (!k.commitShare(name, file)) {
            // ファイルは書いたが、シーンの側を参照に変えられなかった（対象が消えた）。
            // 覚えは入れない（このシーンはそのファイルを使っていない）。
            LOGW("scene", "%s share '%s': the scene item is gone", k.id, name.c_str());
            r.status = libstatus::shareFailed(k.label);
            r.refresh = true;
            return r;
        }
        baseline.remember(k.id, name, text);
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
        // ファイルが読めなかった物（覚えが無い）はローカルにしない（空の中身が正になり、
        // ファイルの参照も消えるため）。3 種類とも同じ規則。
        if (!baseline.find(k.id, name)) {
            r.status = libstatus::unshareUnreadable(k.label, name);
            return r;
        }
        if (!k.unshare(name)) return r;
        baseline.forget(k.id, name);
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
        if (!sharedasset::trashLibraryFile(file, reason)) {
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

}  // namespace wizengine
