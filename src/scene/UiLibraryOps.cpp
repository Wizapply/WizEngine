#include "scene/UiLibraryOps.h"

#include <utility>

#include "core/Json.h"
#include "core/SafeName.h"
#include "scene/LibraryStatus.h"
#include "scene/UiFiles.h"

namespace wizengine {
namespace uilibrary {
namespace {

// 自分の中身から新しい UI を作るときの名前の既定（Canvas に名前が無いとき）。
constexpr const char* kNewUiName = "ui";

// args の canvas が生きていれば、その設計値（中身は持ち主から）と様子。
bool canvasOf(const UiState& state, const nlohmann::json& args, ui::CanvasDesc& desc,
              UiState::ContentsInfo& info) {
    const int canvas = jsonInt(args, "canvas", -1);
    return canvas >= 0 && state.copyCanvasContents(canvas, desc, info);
}

}  // namespace

bool findForShare(const UiState& state, const nlohmann::json& args, std::string& name,
                  std::string& file, std::string& refusal) {
    ui::CanvasDesc desc;
    UiState::ContentsInfo info;
    if (!canvasOf(state, args, desc, info)) return find(state, name, file);
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
    ui::UiAssetDesc asset;
    if (!state.copyUiAsset(name, asset)) return false;
    file = asset.file;
    return true;
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

void unshare(UiState& state, const std::string& name) { state.setUiAssetFile(name, ""); }

bool importFromLibrary(UiState& state, const std::string& name, const std::string& file,
                       std::string& text, std::string& reason) {
    ui::UiAssetDesc asset;
    asset.name = name;
    asset.file = file;
    if (!uifiles::readSharedUi(asset, reason)) return false;
    text = uifiles::libraryText(asset);
    if (!state.addUiAsset(std::move(asset))) {
        reason = "ui '" + name + "' is already declared";
        return false;
    }
    return true;
}

}  // namespace uilibrary
}  // namespace wizengine
