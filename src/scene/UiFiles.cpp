#include "scene/UiFiles.h"

#include <utility>
#include <vector>

#include "core/Log.h"
#include "scene/SharedAssetFile.h"
#include "ui/UiXml.h"

namespace wizengine {
namespace uifiles {

bool readSharedUi(ui::UiAssetDesc& asset, std::string& reason) {
    asset.elements.clear();
    asset.anims.clear();
    std::string text;
    if (!sharedasset::readAssetText(asset.file, text, reason)) return false;
    ui::UiAssetDesc got;
    std::string error;
    std::vector<std::string> warnings;
    if (!ui::parseUiAssetXml(text, got, error, &warnings)) {
        reason = asset.file + ": " + error;
        return false;
    }
    for (const auto& w : warnings) LOGW("scene", "ui file '%s': %s", asset.file.c_str(), w.c_str());
    if (!got.name.empty() && got.name != asset.name) {
        // 参照名はシーンの側が正（Canvas がこの名前で指している）。
        LOGW("scene", "ui file '%s' is named '%s' - using it as '%s'", asset.file.c_str(),
             got.name.c_str(), asset.name.c_str());
    }
    asset.elements = std::move(got.elements);
    asset.anims = std::move(got.anims);
    return true;
}

}  // namespace uifiles
}  // namespace wizengine
