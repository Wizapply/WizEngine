#include "scene/UiState.h"

#include <utility>

namespace wizengine {

std::uint64_t UiState::version() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return version_;
}

bool UiState::copyIfChanged(std::uint64_t& known, Snapshot& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (known == version_) return false;
    out.version = version_;
    out.canvases = canvases_;
    out.uiAssets = uiAssets_;
    out.fonts = fonts_;
    known = version_;
    return true;
}

UiState::Snapshot UiState::snapshot() const {
    Snapshot out;
    std::uint64_t known = 0;
    copyIfChanged(known, out);
    return out;
}

const ui::CanvasDesc* UiState::Snapshot::liveCanvas(int index) const {
    if (index < 0 || std::size_t(index) >= canvases.size()) return nullptr;
    const CanvasItem& item = canvases[std::size_t(index)];
    return item.alive ? &item.desc : nullptr;
}

const std::vector<ui::ElementDesc>& UiState::Snapshot::elementsOf(
    const ui::CanvasDesc& desc) const {
    if (desc.ui.empty()) return desc.elements;
    for (const auto& a : uiAssets) {
        if (a.desc.name == desc.ui) return a.desc.elements;
    }
    static const std::vector<ui::ElementDesc> kNone;
    return kNone;
}

bool UiState::Snapshot::selectionExists(int canvas, int element) const {
    const ui::CanvasDesc* desc = liveCanvas(canvas);
    if (!desc) return false;
    if (element < 0) return true;
    for (const auto& e : elementsOf(*desc)) {
        if (e.id == element) return true;
    }
    return false;
}

std::vector<int> UiState::toDocument(editor::SceneDocument& doc) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<int> remap(canvases_.size(), -1);
    int next = 0;
    for (std::size_t i = 0; i < canvases_.size(); ++i) {
        if (!canvases_[i].alive) continue;
        remap[i] = next++;
        doc.canvases.push_back(canvases_[i].desc);
    }
    for (const auto& a : uiAssets_) doc.uiAssets.push_back(a.desc);
    doc.fonts = fonts_;
    return remap;
}

std::size_t UiState::fromDocument(const std::vector<ui::CanvasDesc>& canvases,
                                  const std::vector<ui::UiAssetDesc>& uiAssets,
                                  const std::vector<ui::FontAssetDesc>& fonts, LoadMode mode) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (mode == LoadMode::Replace) {
        // 番号は詰めずに残す（前の Canvas を指していたものが、読込後の別の
        // Canvas を指さないよう、古い番号は alive = false で残して後ろへ足す
        // = ライトの読込と同じ）。宣言（共用 UI・フォント）は置き換え。
        retireAllCanvasesLocked();
        uiAssets_.clear();
        fonts_.clear();
    }
    const std::size_t base = canvases_.size();
    for (const auto& c : canvases) canvases_.push_back({c, true});
    auto hasAsset = [this](const std::string& name) {
        for (const auto& a : uiAssets_) {
            if (a.desc.name == name) return true;
        }
        return false;
    };
    for (const auto& a : uiAssets) {
        if (!hasAsset(a.name)) uiAssets_.push_back({a, false});
    }
    auto hasFont = [this](const std::string& name) {
        for (const auto& f : fonts_) {
            if (f.name == name) return true;
        }
        return false;
    };
    for (const auto& f : fonts) {
        if (!hasFont(f.name)) fonts_.push_back(f);
    }
    bumpLocked();
    return base;
}

void UiState::retireAllCanvasesLocked() {
    // 番号は詰めない（alive = false）が、中身（要素の木）は捨てる = 読込を繰り返しても
    // 生きていない枠が中身を溜めない（読み手は alive を見るだけで足りる）。
    for (auto& c : canvases_) {
        c.alive = false;
        c.desc = ui::CanvasDesc{};
    }
}

void UiState::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    retireAllCanvasesLocked();
    uiAssets_.clear();
    fonts_.clear();
    bumpLocked();
}

}  // namespace wizengine
