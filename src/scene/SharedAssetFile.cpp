#include "scene/SharedAssetFile.h"

#include <fstream>
#include <iterator>

#include "core/AssetError.h"
#include "core/Log.h"
#include "core/Trash.h"
#include "document/EditorTypes.h"

namespace wizengine {
namespace sharedasset {
namespace {

// assets/ の外を指すパスを弾く（文書は手で書けるので、参照の file を信じない）。
bool allowed(const std::string& file, std::string& reason) {
    if (!file.empty() && editor::assetFileAllowed(file)) return true;
    reason = "'" + file + "' is not an assets-relative path";
    return false;
}

}  // namespace

bool writeTextFile(const std::string& path, const std::string& text, std::string& reason) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        reason = "cannot write: " + path;
        return false;
    }
    f << text;
    if (!f) {
        reason = "write error: " + path;
        return false;
    }
    LOGI("editor", "saved %s", path.c_str());
    return true;
}

bool readTextFile(const std::string& path, std::string& text, std::string& reason) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        reason = "not found: " + path;
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (f.bad()) {
        reason = "read error: " + path;
        return false;
    }
    LOGI("editor", "loaded %s", path.c_str());
    return true;
}

bool readAssetText(const std::string& file, std::string& text, std::string& reason) {
    if (!allowed(file, reason)) return false;
    return readTextFile(assetPath(file), text, reason);
}

bool writeAssetText(const std::string& file, const std::string& text, std::string& reason) {
    if (!allowed(file, reason)) return false;
    return writeTextFile(assetPath(file), text, reason);
}

std::string libraryFile(const std::string& subdir, const std::string& name,
                        const std::string& ext) {
    const std::string safe = sanitizeSafeName(name);
    if (safe.empty()) return {};
    return subdir + "/" + safe + ext;
}

std::vector<std::string> listLibrary(const std::string& subdir, const std::string& ext) {
    return listLibraryIn(std::filesystem::path(assetPath(subdir)), ext);
}

bool trashLibraryFile(const std::string& file, std::string& reason) {
    if (!allowed(file, reason)) return false;
    return moveToTrash(std::filesystem::path(assetPath(file)), reason);
}

}  // namespace sharedasset
}  // namespace wizengine
