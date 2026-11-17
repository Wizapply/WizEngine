#include "scene/UiState.h"

#include <algorithm>
#include <utility>

namespace wizengine {
namespace {

// 生きている枠（次の要素の id は中身の最大の次から）。
UiState::CanvasItem liveItem(ui::CanvasDesc desc) {
    UiState::CanvasItem item;
    item.nextElementId = ui::nextElementId(desc.elements);
    item.desc = std::move(desc);
    return item;
}

// 生きていない枠にする: 番号は詰めない（alive = false）が、中身（要素の木）は捨てる =
// 読込や削除を繰り返しても生きていない枠が中身を溜めない（読み手は alive を見るだけで
// 足りる）。
void retire(UiState::CanvasItem& item) {
    item.alive = false;
    item.desc = ui::CanvasDesc{};
}

// 名前 name の UI の宣言（無ければ nullptr）。Snapshot と UiState（ロックの中）が使う。
template <class Assets>
auto findAsset(Assets& assets, const std::string& name) -> decltype(&assets[0]) {
    for (auto& a : assets) {
        if (a.desc.name == name) return &a;
    }
    return nullptr;
}

// Canvas の要素の持ち主の様子（唯一の定義。Snapshot::contentsInfo と copyCanvasContents）。
UiState::ContentsInfo infoOf(const UiState::CanvasItem& item,
                             const std::vector<UiState::UiAssetState>& assets) {
    UiState::ContentsInfo info;
    info.asset = item.desc.ui;
    info.nextElementId = item.nextElementId;
    if (info.asset.empty()) return info;
    const UiState::UiAssetState* a = findAsset(assets, info.asset);
    info.declared = a != nullptr;
    info.unreadable = a && a->unreadable;
    info.nextElementId = a ? a->nextElementId : 1;
    return info;
}

}  // namespace

std::uint64_t UiState::version() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return version_;
}

std::uint64_t UiState::loadGeneration() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return loadGeneration_;
}

bool UiState::copyIfChanged(std::uint64_t& known, Snapshot& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (known == version_) return false;
    out.version = version_;
    out.loadGeneration = loadGeneration_;
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

bool UiState::copyCanvas(int index, ui::CanvasDesc& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= canvases_.size()) return false;
    const CanvasItem& item = canvases_[std::size_t(index)];
    if (!item.alive) return false;
    out = item.desc;
    return true;
}

const ui::CanvasDesc* UiState::Snapshot::liveCanvas(int index) const {
    if (index < 0 || std::size_t(index) >= canvases.size()) return nullptr;
    const CanvasItem& item = canvases[std::size_t(index)];
    return item.alive ? &item.desc : nullptr;
}

const UiState::UiAssetState* UiState::Snapshot::uiAsset(const std::string& name) const {
    return findAsset(uiAssets, name);
}

UiState::ContentsInfo UiState::Snapshot::contentsInfo(int index) const {
    if (!liveCanvas(index)) return {};
    return infoOf(canvases[std::size_t(index)], uiAssets);
}

const std::vector<ui::ElementDesc>& UiState::Snapshot::elementsOf(
    const ui::CanvasDesc& desc) const {
    if (desc.ui.empty()) return desc.elements;
    if (const UiAssetState* a = uiAsset(desc.ui)) return a->desc.elements;
    static const std::vector<ui::ElementDesc> kNone;
    return kNone;
}

bool UiState::Snapshot::selectionExists(int canvas, int element) const {
    const ui::CanvasDesc* desc = liveCanvas(canvas);
    if (!desc) return false;
    if (element < 0) return true;
    const std::vector<ui::ElementDesc>& elements = elementsOf(*desc);
    return ui::elementIndex(elements, element) < elements.size();
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

UiState::LoadResult UiState::fromDocument(const std::vector<ui::CanvasDesc>& canvases,
                                          const std::vector<ui::UiAssetDesc>& uiAssets,
                                          const std::vector<ui::FontAssetDesc>& fonts,
                                          LoadMode mode,
                                          const std::vector<std::string>& unreadable) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (mode == LoadMode::Replace) {
        // 番号は詰めずに残す（前の Canvas を指していたものが、読込後の別の
        // Canvas を指さないよう、古い番号は alive = false で残して後ろへ足す
        // = ライトの読込と同じ）。宣言（共用 UI・フォント）は置き換え。
        retireAllCanvasesLocked();
        uiAssets_.clear();
        fonts_.clear();
    }
    LoadResult result;
    result.base = canvases_.size();
    for (const auto& c : canvases) canvases_.push_back(liveItem(c));
    for (const auto& a : uiAssets) {
        if (findAsset(uiAssets_, a.name)) {
            result.skippedUi.push_back(a.name);
            continue;
        }
        UiAssetState state;
        state.desc = a;
        state.unreadable =
            std::find(unreadable.begin(), unreadable.end(), a.name) != unreadable.end();
        state.nextElementId = ui::nextElementId(a.elements);
        uiAssets_.push_back(std::move(state));
    }
    auto hasFont = [this](const std::string& name) {
        for (const auto& f : fonts_) {
            if (f.name == name) return true;
        }
        return false;
    };
    for (const auto& f : fonts) {
        if (hasFont(f.name)) {
            result.skippedFonts.push_back(f.name);
            continue;
        }
        fonts_.push_back(f);
    }
    ++loadGeneration_;
    bumpLocked();
    return result;
}

void UiState::retireAllCanvasesLocked() {
    for (auto& c : canvases_) retire(c);
}

void UiState::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    retireAllCanvasesLocked();
    uiAssets_.clear();
    fonts_.clear();
    ++loadGeneration_;
    bumpLocked();
}

int UiState::addCanvas(ui::CanvasDesc desc) {
    std::lock_guard<std::mutex> lk(mutex_);
    canvases_.push_back(liveItem(std::move(desc)));
    bumpLocked();
    return int(canvases_.size() - 1);
}

bool UiState::removeCanvas(int index) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= canvases_.size()) return false;
    CanvasItem& item = canvases_[std::size_t(index)];
    if (!item.alive) return false;
    retire(item);
    bumpLocked();
    return true;
}

bool UiState::replaceCanvas(int index, ui::CanvasDesc desc) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= canvases_.size()) return false;
    CanvasItem& item = canvases_[std::size_t(index)];
    if (!item.alive) return false;
    item.nextElementId = std::max(item.nextElementId, ui::nextElementId(desc.elements));
    item.desc = std::move(desc);
    bumpLocked();
    return true;
}

bool UiState::copyCanvasContents(int index, ui::CanvasDesc& out, ContentsInfo& info) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= canvases_.size()) return false;
    const CanvasItem& item = canvases_[std::size_t(index)];
    if (!item.alive) return false;
    out = item.desc;
    info = infoOf(item, uiAssets_);
    if (info.asset.empty()) return true;
    const UiAssetState* a = findAsset(uiAssets_, info.asset);
    if (a) {
        ui::copyContents(a->desc, out);
    } else {
        ui::clearContents(out);  // 宣言の無い UI は空
    }
    return true;
}

bool UiState::replaceCanvasContents(int index, ui::CanvasDesc desc) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= canvases_.size()) return false;
    CanvasItem& item = canvases_[std::size_t(index)];
    if (!item.alive || item.desc.ui != desc.ui) return false;
    if (desc.ui.empty()) {
        item.nextElementId = std::max(item.nextElementId, ui::nextElementId(desc.elements));
        item.desc = std::move(desc);
        bumpLocked();
        return true;
    }
    UiAssetState* a = findAsset(uiAssets_, desc.ui);
    if (!a) return false;
    a->nextElementId = std::max(a->nextElementId, ui::nextElementId(desc.elements));
    ui::moveContents(desc, a->desc);
    bumpLocked();
    return true;
}

bool UiState::copyUiAsset(const std::string& name, ui::UiAssetDesc& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const UiAssetState* a = findAsset(uiAssets_, name);
    if (!a) return false;
    out = a->desc;
    return true;
}

bool UiState::addUiAsset(ui::UiAssetDesc asset) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (asset.name.empty() || findAsset(uiAssets_, asset.name)) return false;
    UiAssetState state;
    state.nextElementId = ui::nextElementId(asset.elements);
    state.desc = std::move(asset);
    uiAssets_.push_back(std::move(state));
    bumpLocked();
    return true;
}

bool UiState::setUiAssetFile(const std::string& name, const std::string& file) {
    std::lock_guard<std::mutex> lk(mutex_);
    UiAssetState* a = findAsset(uiAssets_, name);
    if (!a) return false;
    a->desc.file = file;
    // 共用にした（今の中身を書いた）・やめた（中身はこのシーンの物）= どちらも、今の中身が
    // そのまま正になる。
    a->unreadable = false;
    bumpLocked();
    return true;
}

bool UiState::moveCanvasContentsToAsset(int index, ui::UiAssetDesc asset) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= canvases_.size()) return false;
    CanvasItem& item = canvases_[std::size_t(index)];
    if (!item.alive || !item.desc.ui.empty()) return false;
    if (asset.name.empty() || findAsset(uiAssets_, asset.name)) return false;
    UiAssetState state;
    // 消した番号を使い回さない約束は、Canvas の番号から引き継ぐ。
    state.nextElementId = std::max(item.nextElementId, ui::nextElementId(asset.elements));
    item.desc.ui = asset.name;
    ui::clearContents(item.desc);
    state.desc = std::move(asset);
    uiAssets_.push_back(std::move(state));
    bumpLocked();
    return true;
}

}  // namespace wizengine
