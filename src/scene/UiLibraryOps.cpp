#include "scene/UiLibraryOps.h"

#include <utility>

#include "core/Json.h"
#include "core/SafeName.h"
#include "scene/LibraryStatus.h"
#include "scene/UiFiles.h"
#include "scene/UiOps.h"  // readNameArg

namespace wizengine {
namespace uilibrary {
namespace {

// 自分の中身から新しい UI を作るときの名前の既定（Canvas に名前が無いとき）。
constexpr const char* kNewUiName = "ui";

// args が canvas を指しているか（0 以上の番号）。
bool canvasGiven(const nlohmann::json& args) { return jsonInt(args, "canvas", -1) >= 0; }

// args の canvas が生きていれば、その設計値（中身は持ち主から）と様子。
bool canvasOf(const UiState& state, const nlohmann::json& args, ui::CanvasDesc& desc,
              UiState::ContentsInfo& info) {
    return canvasGiven(args) && state.copyCanvasContents(jsonInt(args, "canvas", -1), desc, info);
}

// 送った名前が正規化で全部落ちたか（日本語だけなど。空で送ったのとは違う。読み方は
// uiops::readNameArg、正規化は名前の決まり sanitizeSafeName = applyLibraryOp と同じ）。
bool nameUnusable(const nlohmann::json& args) {
    std::string normalized;
    auto normalize = [](const std::string& raw) { return sanitizeSafeName(raw); };
    return uiops::readNameArg(args, normalize, normalized) == uiops::NameArg::Unusable;
}

}  // namespace

LibraryKind libraryKind(UiState& state, const nlohmann::json& args, LibraryBaseline& baseline) {
    LibraryKind k;
    k.id = uifiles::kUiLibraryKind;
    k.label = kLabel;
    k.importHint = kImportHint;
    k.libraryFile = &uifiles::libraryFile;
    k.find = [&state](const std::string& n, std::string& file) { return find(state, n, file); };
    k.findForShare = [&state, args](std::string& n, std::string& file, std::string& refusal) {
        return findForShare(state, args, n, file, refusal);
    };
    k.sharedText = [&state, args](const std::string& n, const std::string& file) {
        return sharedText(state, args, n, file);
    };
    k.commitShare = [&state, args](const std::string& n, const std::string& file) {
        return commitShare(state, args, n, file);
    };
    k.unshare = [&state](const std::string& n) { return unshare(state, n); };
    k.import = [&state, &baseline](const std::string& n, const std::string& file,
                                   std::string& reason) {
        return importFromLibrary(state, n, file, baseline, reason);
    };
    return k;
}

bool findForShare(const UiState& state, const nlohmann::json& args, std::string& name,
                  std::string& file, std::string& refusal) {
    if (nameUnusable(args)) {
        refusal = libstatus::unusableName(kLabel);
        return false;
    }
    if (!canvasGiven(args)) return find(state, name, file);
    ui::CanvasDesc desc;
    UiState::ContentsInfo info;
    if (!canvasOf(state, args, desc, info)) return false;  // 消えた（名前で探し直さない）
    if (!info.asset.empty()) {
        name = info.asset;  // Canvas が使っている UI
        return find(state, name, file);
    }
    // 自分の中身の Canvas: 中身を新しい UI にする。
    if (name.empty()) name = sanitizeSafeName(desc.name);
    if (name.empty()) name = kNewUiName;
    if (find(state, name, file)) {
        refusal = libstatus::alreadyInScene(kLabel, name);
        return false;
    }
    file.clear();
    return true;
}

bool find(const UiState& state, const std::string& name, std::string& file) {
    return state.findUiAsset(name, file);
}

std::string sharedText(const UiState& state, const nlohmann::json& args,
                       const std::string& name, const std::string& file) {
    ui::UiAssetDesc asset;
    ui::CanvasDesc desc;
    UiState::ContentsInfo info;
    if (canvasOf(state, args, desc, info) && info.asset.empty()) {
        asset.name = name;
        ui::copyContents(desc, asset);
    } else if (!state.copyUiAsset(name, asset)) {
        return {};
    }
    asset.file = file;
    return uifiles::libraryText(asset);
}

bool commitShare(UiState& state, const nlohmann::json& args, const std::string& name,
                 const std::string& file) {
    ui::CanvasDesc desc;
    UiState::ContentsInfo info;
    if (canvasOf(state, args, desc, info) && info.asset.empty()) {
        ui::UiAssetDesc asset;
        asset.name = name;
        asset.file = file;
        ui::moveContents(desc, asset);
        return state.moveCanvasContentsToAsset(jsonInt(args, "canvas", -1), std::move(asset));
    }
    return state.setUiAssetFile(name, file);
}

bool unshare(UiState& state, const std::string& name) { return state.setUiAssetFile(name, ""); }

bool importFromLibrary(UiState& state, const std::string& name, const std::string& file,
                       LibraryBaseline& baseline, std::string& reason) {
    ui::UiAssetDesc asset;
    asset.name = name;
    asset.file = file;
    if (!uifiles::readSharedUi(asset, reason)) return false;
    if (!state.addUiAsset(asset)) {
        reason = "ui '" + name + "' is already declared";
        return false;
    }
    uifiles::rememberBaseline(baseline, asset);
    return true;
}

}  // namespace uilibrary
}  // namespace wizengine
