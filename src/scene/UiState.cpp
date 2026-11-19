#include "scene/UiState.h"

#include <algorithm>
#include <utility>

#include "core/SceneLimits.h"
#include "ui/UiKinds.h"   // acceptsClick / catchesPointer
#include "ui/UiLayout.h"  // parentIndices / elementVisible

namespace wizengine {
namespace {

// 生きている枠（次の要素の id は中身の最大の次から）。revision = 入れたときの版、
// generation = 中身の世代（UiState::nextGenerationLocked の新しい値）。
UiState::CanvasItem liveItem(ui::CanvasDesc desc, std::uint64_t revision,
                             std::uint64_t generation) {
    UiState::CanvasItem item;
    item.nextElementId = ui::nextElementId(desc.elements);
    item.desc = std::move(desc);
    item.revision = revision;
    item.contentsGeneration = generation;
    return item;
}

// 生きていない枠にする: 番号は詰めない（alive = false）が、中身（要素の木）は捨てる =
// 読込や削除を繰り返しても生きていない枠が中身を溜めない（読み手は alive を見るだけで
// 足りる）。
void retire(UiState::CanvasItem& item, std::uint64_t revision) {
    item.alive = false;
    item.desc = ui::CanvasDesc{};
    item.revision = revision;
}

// 名前 name の UI の宣言（無ければ nullptr）。Snapshot と UiState（ロックの中）が使う。
template <class Assets>
auto findAsset(Assets& assets, const std::string& name) -> decltype(&assets[0]) {
    for (auto& a : assets) {
        if (a.desc.name == name) return &a;
    }
    return nullptr;
}

// 番号 index の生きている枠（範囲外・消えた物は nullptr）。Snapshot と UiState（ロックの
// 中）が使う。
template <class Canvases>
auto liveItemAt(Canvases& canvases, int index) -> decltype(&canvases[0]) {
    if (index < 0 || std::size_t(index) >= canvases.size()) return nullptr;
    auto& item = canvases[std::size_t(index)];
    return item.alive ? &item : nullptr;
}

// Canvas が使う要素の並び（唯一の定義。Snapshot::elementsOf と selectionIn）。
const std::vector<ui::ElementDesc>& elementsIn(const ui::CanvasDesc& desc,
                                               const std::vector<UiState::UiAssetState>& assets) {
    if (desc.ui.empty()) return desc.elements;
    if (const UiState::UiAssetState* a = findAsset(assets, desc.ui)) return a->desc.elements;
    static const std::vector<ui::ElementDesc> kNone;
    return kNone;
}

// エディタの選択が今もある物を指すか（唯一の定義。Snapshot と UiState の selectionExists）。
bool selectionIn(const std::vector<UiState::CanvasItem>& canvases,
                 const std::vector<UiState::UiAssetState>& assets, int canvas, int element) {
    const UiState::CanvasItem* item = liveItemAt(canvases, canvas);
    if (!item) return false;
    if (element < 0) return true;
    const std::vector<ui::ElementDesc>& elements = elementsIn(item->desc, assets);
    return ui::elementIndex(elements, element) < elements.size();
}

// Canvas の要素の持ち主の様子（唯一の定義。Snapshot::contentsInfo と copyCanvasContents）。
UiState::ContentsInfo infoOf(const UiState::CanvasItem& item,
                             const std::vector<UiState::UiAssetState>& assets) {
    UiState::ContentsInfo info;
    info.asset = item.desc.ui;
    info.nextElementId = item.nextElementId;
    info.contentsGeneration = item.contentsGeneration;
    if (info.asset.empty()) return info;
    const UiState::UiAssetState* a = findAsset(assets, info.asset);
    info.declared = a != nullptr;
    info.unreadable = a && a->unreadable;
    info.nextElementId = a ? a->nextElementId : 1;
    info.contentsGeneration = a ? a->contentsGeneration : 0;
    return info;
}

// from の並びを to へ写す。revision が同じ物は写さない（to は前に同じ持ち主から写した
// 並び）。版は増える一方なので、同じ番号に別の物が入れば revision は必ず違う。
template <class Item>
void copyChangedItems(const std::vector<Item>& from, std::vector<Item>& to) {
    to.resize(from.size());
    for (std::size_t i = 0; i < from.size(); ++i) {
        if (to[i].revision != from[i].revision) to[i] = from[i];
    }
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
    copyChangedItems(canvases_, out.canvases);
    copyChangedItems(uiAssets_, out.uiAssets);
    if (out.fontsRevision != fontsRevision_) {
        out.fonts = fonts_;
        out.fontsRevision = fontsRevision_;
    }
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
    const CanvasItem* item = liveItemAt(canvases_, index);
    if (!item) return false;
    out = item->desc;
    return true;
}

bool UiState::selectionExists(int canvas, int element) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return selectionIn(canvases_, uiAssets_, canvas, element);
}

bool UiState::canvasNameTaken(const std::string& name, int except) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const CanvasItem* self = liveItemAt(canvases_, except);
    return ui::nameTaken(canvases_, name, [self](const CanvasItem& c) {
        return (c.alive && &c != self) ? &c.desc.name : nullptr;
    });
}

bool UiState::findUiAsset(const std::string& name, std::string& file) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const UiAssetState* a = findAsset(uiAssets_, name);
    if (!a) return false;
    file = a->desc.file;
    return true;
}

void UiState::syncReadable(const std::function<bool(const std::string& name)>& readable) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::uint64_t rev = 0;  // 変えたときだけ版を進める（全部同じなら進めない）
    for (auto& a : uiAssets_) {
        const bool unreadable = !a.desc.file.empty() && !readable(a.desc.name);
        if (unreadable == a.unreadable) continue;
        if (rev == 0) rev = bumpLocked();
        a.unreadable = unreadable;
        a.revision = rev;
    }
}

std::uint64_t UiState::Snapshot::contentsRevision(int index) const {
    const CanvasItem* item = liveItemAt(canvases, index);
    if (!item) return 0;
    std::uint64_t r = std::max(item->revision, fontsRevision);
    if (const UiAssetState* a = item->desc.ui.empty() ? nullptr : uiAsset(item->desc.ui)) {
        r = std::max(r, a->revision);
    }
    return r;
}

const ui::CanvasDesc* UiState::Snapshot::liveCanvas(int index) const {
    const CanvasItem* item = liveItemAt(canvases, index);
    return item ? &item->desc : nullptr;
}

const UiState::UiAssetState* UiState::Snapshot::uiAsset(const std::string& name) const {
    return findAsset(uiAssets, name);
}

UiState::ContentsInfo UiState::Snapshot::contentsInfo(int index) const {
    const CanvasItem* item = liveItemAt(canvases, index);
    return item ? infoOf(*item, uiAssets) : ContentsInfo{};
}

const std::vector<ui::ElementDesc>& UiState::Snapshot::elementsOf(
    const ui::CanvasDesc& desc) const {
    return elementsIn(desc, uiAssets);
}

bool UiState::Snapshot::selectionExists(int canvas, int element) const {
    return selectionIn(canvases, uiAssets, canvas, element);
}

UiState::ElementRef UiState::Snapshot::elementRef(int canvas, int element) const {
    ElementRef ref;
    ref.canvas = canvas;
    ref.element = element;
    ref.loadGeneration = loadGeneration;
    ref.contentsGeneration = contentsInfo(canvas).contentsGeneration;
    return ref;
}

bool UiState::refValid(const ElementRef& ref) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return refValidLocked(ref);
}

bool UiState::refValidLocked(const ElementRef& ref) const {
    const CanvasItem* item = liveItemAt(canvases_, ref.canvas);
    if (!item || ref.loadGeneration != loadGeneration_) return false;  // 消えた・読み直した
    // 中身を XML で丸ごと入れ替えた・持ち主が替わった（同じ id の別の要素）。
    if (infoOf(*item, uiAssets_).contentsGeneration != ref.contentsGeneration) return false;
    const std::vector<ui::ElementDesc>& elements = elementsIn(item->desc, uiAssets_);
    return ui::elementIndex(elements, ref.element) < elements.size();
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
                                          LoadMode mode) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::uint64_t rev = bumpLocked();
    const std::size_t assetsBefore = mode == LoadMode::Replace ? 0 : uiAssets_.size();
    if (mode == LoadMode::Replace) {
        // 番号は詰めずに残す（前の Canvas を指していたものが、読込後の別の
        // Canvas を指さないよう、古い番号は alive = false で残して後ろへ足す
        // = ライトの読込と同じ）。宣言（共用 UI・フォント）は置き換え。
        retireAllCanvasesLocked(rev);
        uiAssets_.clear();
    }
    // フォントの宣言の版は、宣言が本当に変わったとき（消した・足した）だけ進める（取込の
    // たびに全 Canvas を組み直さない）。
    bool fontsChanged = false;
    if (mode == LoadMode::Replace && !fonts_.empty()) {
        fonts_.clear();
        fontsChanged = true;
    }
    LoadResult result;
    result.base = canvases_.size();
    for (const auto& c : canvases) canvases_.push_back(liveItem(c, rev, nextGenerationLocked()));
    for (const auto& a : uiAssets) {
        const UiAssetState* have = findAsset(uiAssets_, a.name);
        // 同じ文書の中の 2 回目（今回足した物と重なる）か、今のシーンの物と重なったか。
        const bool sameDocument =
            have && std::size_t(have - uiAssets_.data()) >= assetsBefore;
        if (have) {
            (sameDocument ? result.duplicatedUi : result.skippedUi).push_back(a.name);
            result.enteredUi.push_back(false);
            continue;
        }
        UiAssetState state;
        state.desc = a;
        state.nextElementId = ui::nextElementId(a.elements);
        state.revision = rev;
        state.contentsGeneration = nextGenerationLocked();
        uiAssets_.push_back(std::move(state));
        result.enteredUi.push_back(true);
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
        fontsChanged = true;
    }
    if (fontsChanged) fontsRevision_ = rev;
    ++loadGeneration_;
    return result;
}

void UiState::retireAllCanvasesLocked(std::uint64_t revision) {
    for (auto& c : canvases_) {
        if (c.alive) retire(c, revision);
    }
}

void UiState::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::uint64_t rev = bumpLocked();
    retireAllCanvasesLocked(rev);
    uiAssets_.clear();
    if (!fonts_.empty()) {
        fonts_.clear();
        fontsRevision_ = rev;
    }
    ++loadGeneration_;
}

int UiState::addCanvas(ui::CanvasDesc desc) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::uint64_t rev = bumpLocked();
    canvases_.push_back(liveItem(std::move(desc), rev, nextGenerationLocked()));
    return int(canvases_.size() - 1);
}

bool UiState::removeCanvas(int index, ui::CanvasDesc* removed) {
    std::lock_guard<std::mutex> lk(mutex_);
    CanvasItem* item = liveItemAt(canvases_, index);
    if (!item) return false;
    if (removed) *removed = std::move(item->desc);
    retire(*item, bumpLocked());
    return true;
}

bool UiState::replaceCanvas(int index, ui::CanvasDesc desc) {
    std::lock_guard<std::mutex> lk(mutex_);
    CanvasItem* item = liveItemAt(canvases_, index);
    if (!item) return false;
    item->nextElementId = std::max(item->nextElementId, ui::nextElementId(desc.elements));
    item->desc = std::move(desc);
    item->revision = bumpLocked();
    return true;
}

bool UiState::copyCanvasContents(int index, ui::CanvasDesc& out, ContentsInfo& info) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const CanvasItem* item = liveItemAt(canvases_, index);
    if (!item) return false;
    out = item->desc;
    info = infoOf(*item, uiAssets_);
    if (info.asset.empty()) return true;
    const UiAssetState* a = findAsset(uiAssets_, info.asset);
    if (a) {
        ui::copyContents(a->desc, out);
    } else {
        ui::clearContents(out);  // 宣言の無い UI は空
    }
    return true;
}

bool UiState::replaceCanvasContents(int index, ui::CanvasDesc desc, bool wholesale) {
    std::lock_guard<std::mutex> lk(mutex_);
    CanvasItem* item = liveItemAt(canvases_, index);
    if (!item || item->desc.ui != desc.ui) return false;
    if (desc.ui.empty()) {
        item->nextElementId = std::max(item->nextElementId, ui::nextElementId(desc.elements));
        item->desc = std::move(desc);
        item->revision = bumpLocked();
        if (wholesale) item->contentsGeneration = nextGenerationLocked();
        return true;
    }
    UiAssetState* a = findAsset(uiAssets_, desc.ui);
    if (!a) return false;
    a->nextElementId = std::max(a->nextElementId, ui::nextElementId(desc.elements));
    ui::moveContents(desc, a->desc);
    a->revision = bumpLocked();
    if (wholesale) a->contentsGeneration = nextGenerationLocked();
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
    state.revision = bumpLocked();
    state.contentsGeneration = nextGenerationLocked();
    uiAssets_.push_back(std::move(state));
    return true;
}

bool UiState::setUiAssetFile(const std::string& name, const std::string& file) {
    std::lock_guard<std::mutex> lk(mutex_);
    UiAssetState* a = findAsset(uiAssets_, name);
    if (!a) return false;
    a->desc.file = file;
    a->revision = bumpLocked();
    return true;
}

bool UiState::moveCanvasContentsToAsset(int index, ui::UiAssetDesc asset) {
    std::lock_guard<std::mutex> lk(mutex_);
    CanvasItem* item = liveItemAt(canvases_, index);
    if (!item || !item->desc.ui.empty()) return false;
    if (asset.name.empty() || findAsset(uiAssets_, asset.name)) return false;
    const std::uint64_t rev = bumpLocked();
    UiAssetState state;
    // 消した番号を使い回さない約束は、Canvas の番号から引き継ぐ。
    state.nextElementId = std::max(item->nextElementId, ui::nextElementId(asset.elements));
    state.revision = rev;
    // 中身の世代も引き継ぐ（その中身の上のドラッグは、共用にしても続く）。
    state.contentsGeneration = item->contentsGeneration;
    item->desc.ui = asset.name;
    ui::clearContents(item->desc);
    item->revision = rev;
    state.desc = std::move(asset);
    uiAssets_.push_back(std::move(state));
    return true;
}

// ---- 実行時の状態 ------------------------------------------------------------------
std::uint64_t UiState::pointersVersion() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return pointersVersion_;
}

std::uint64_t UiState::overridesVersion() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return overridesVersion_;
}

UiState::Generations UiState::generations() const {
    std::lock_guard<std::mutex> lk(mutex_);
    Generations g;
    g.runtime = runtimeGeneration_;
    g.pause = pauseGeneration_;
    return g;
}

void UiState::setPaused(bool paused) {
    if (paused_.load() == paused) return;  // 毎パスの「変わっていない」はロックを取らない
    std::lock_guard<std::mutex> lk(mutex_);
    paused_.store(paused);
    ++pauseGeneration_;
    if (paused) clicks_.clear();
}

bool UiState::clickableOnCamera(int camera) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const CanvasItem& c : canvases_) {
        if (!c.alive || !ui::canvasShownOnCamera(c.desc, camera)) continue;
        const std::vector<ui::ElementDesc>& elements = elementsIn(c.desc, uiAssets_);
        if (std::any_of(elements.begin(), elements.end(), ui::catchesPointer)) return true;
    }
    return false;
}

void UiState::setPointer(std::size_t camera, const Pointer& pointer,
                         std::uint64_t runtimeGeneration) {
    if (camera >= std::size_t(kMaxCameraSlots)) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (runtimeGeneration != runtimeGeneration_) return;  // 止めた・Reset の前に読んだ
    if (pointers_.size() <= camera) {
        if (pointer == Pointer{}) return;  // 無い枠は「無し」と同じ
        pointers_.resize(camera + 1);
    }
    if (pointers_[camera] == pointer) return;
    pointers_[camera] = pointer;
    ++pointersVersion_;
}

bool UiState::copyPointersIfChanged(std::uint64_t& known, std::vector<Pointer>& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (known == pointersVersion_) return false;
    out = pointers_;
    known = pointersVersion_;
    return true;
}

bool UiState::copyOverridesIfChanged(std::uint64_t& known, NodeOverrideSet& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (known == overridesVersion_) return false;
    // 消えた Canvas の分を落とし、版の違う Canvas だけを写す（版は増える一方 = 同じ版なら
    // 同じ中身）。
    for (auto it = out.canvases.begin(); it != out.canvases.end();) {
        it = overrides_.count(it->first) ? std::next(it) : out.canvases.erase(it);
    }
    for (const auto& [index, slot] : overrides_) {
        CanvasOverrides& mine = out.canvases[index];
        if (mine.revision != slot.revision) mine = slot;
    }
    known = overridesVersion_;
    return true;
}

const ui::NodeOverrides* UiState::NodeOverrideSet::of(int index) const {
    const auto it = canvases.find(index);
    return it == canvases.end() ? nullptr : &it->second.props;
}

std::uint64_t UiState::NodeOverrideSet::revision(int index) const {
    const auto it = canvases.find(index);
    return it == canvases.end() ? 0 : it->second.revision;
}

UiState::NodePropResult UiState::setNodeProp(int canvas, const std::string& element,
                                             int elementId, ui::Prop prop,
                                             const ui::PropValue& value) {
    std::lock_guard<std::mutex> lk(mutex_);
    const CanvasItem* item = liveItemAt(canvases_, canvas);
    if (!item) return NodePropResult::NoCanvas;
    const std::vector<ui::ElementDesc>& elements = elementsIn(item->desc, uiAssets_);
    const ui::ElementDesc* e = nullptr;
    if (!element.empty()) {
        e = ui::findElementByName(elements, element);
    } else {
        const std::size_t at = ui::elementIndex(elements, elementId);
        e = at < elements.size() ? &elements[at] : nullptr;
    }
    if (!e) return NodePropResult::NoElement;
    const ui::ElementKindInfo* kind = ui::kindInfo(e->kind);
    if (!kind || !(ui::propsOfKind(kind->id) & ui::propBit(prop))) {
        return NodePropResult::KindLacksProp;
    }
    const ui::ValueSpec& spec = ui::propInfo(prop).spec;
    // ノードの欄が持たない成分（色の不透明度）は設計値のまま（ui::withNodeValue）。
    const ui::PropValue clamped =
        ui::withNodeValue(spec, ui::clampValue(spec, value), ui::effectiveValue(e->props, prop));
    CanvasOverrides& slot = overrides_[canvas];
    ui::PropSet& props = slot.props[e->id];
    if (props.has(prop) && ui::sameValue(spec, props.get(prop), clamped)) {
        return NodePropResult::Unchanged;
    }
    props.set(prop, clamped);
    slot.revision = ++overridesVersion_;
    return NodePropResult::Ok;
}

void UiState::pushClick(const PendingClick& click) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (paused_.load()) return;  // 止まっている間に離した（再開後に押したぶんだけ効く）
    if (clicks_.size() >= kMaxPendingClicks) clicks_.erase(clicks_.begin());
    clicks_.push_back(click);
}

bool UiState::clickValidLocked(const PendingClick& c, Click& out) const {
    if (c.generations.runtime != runtimeGeneration_) return false;  // Reset をまたいだ押下
    if (c.generations.pause != pauseGeneration_) return false;  // 一時停止をまたいだ・中の押下
    if (!refValidLocked(c.ref)) return false;  // 消えた・読み直した・入れ替えた
    const ui::CanvasDesc& desc = canvases_[std::size_t(c.ref.canvas)].desc;
    if (!ui::canvasShownOnCamera(desc, int(c.camera))) return false;  // 他のカメラ向け
    const std::vector<ui::ElementDesc>& elements = elementsIn(desc, uiAssets_);
    const std::size_t at = ui::elementIndex(elements, c.ref.element);
    // 性質はノードの上書きまで重ねた値（描く側・当たりと同じ口 ui::resolveElements）。
    const auto over = overrides_.find(c.ref.canvas);
    const std::vector<ui::PropSet> resolved =
        ui::resolveElements(elements, over == overrides_.end() ? nullptr : &over->second.props);
    // 見えるか（祖先も）は矩形を解かずに決める（描く側の layoutTree と同じ規則）。
    const bool visible = ui::elementVisible(elements, resolved, ui::parentIndices(elements), at);
    if (!ui::acceptsClick(elements[at], resolved[at], visible)) return false;
    out.camera = c.camera;
    out.canvas = c.ref.canvas;
    out.element = c.ref.element;
    out.name = elements[at].name;
    return true;
}

std::vector<UiState::Click> UiState::takeClicks(std::uint64_t modeEpoch) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Click> out;
    if (clicks_.empty()) return out;
    for (const PendingClick& c : clicks_) {
        Click click;
        if (c.modeEpoch == modeEpoch && clickValidLocked(c, click)) out.push_back(std::move(click));
    }
    clicks_.clear();
    return out;
}

void UiState::resetRuntime() {
    std::lock_guard<std::mutex> lk(mutex_);
    pointers_.clear();
    clicks_.clear();
    overrides_.clear();
    ++pointersVersion_;
    ++overridesVersion_;
    ++runtimeGeneration_;
}

}  // namespace wizengine
