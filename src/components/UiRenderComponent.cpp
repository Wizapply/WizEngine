#include "components/UiRenderComponent.h"

#include <algorithm>
#include <unordered_map>

#include "core/Log.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/SharedAssetFile.h"
#include "ui/UiLayout.h"
#include "ui/UiMesh.h"
#include "ui/UiProps.h"
#include "ui/UiValue.h"

namespace ui = wizengine::ui;

namespace {

// 既定のフォント（<font> を書かない文書・知らない font 名の字）。同梱の Noto Sans JP
// （SIL OFL 1.1、assets/fonts/OFL.txt）。FontAtlas は知らない名前を最初の
// フォント（0 番）に落とすので、これを最初に足す。
constexpr const char* kDefaultFontFile = "fonts/NotoSansJP-Regular.otf";
// フォントのファイルの大きさの上限（壊れた・別の大きなファイルを丸ごと読まない
// 安全弁。同梱の Noto Sans JP は約 4.5 MB、CJK 全部入りでも 20 MB 前後）。
constexpr std::uintmax_t kMaxFontBytes = 32u * 1024u * 1024u;

// この部品が描く Canvas か（唯一の判定）。今は overlay だけ。world / camera
// （3D の板）は PR5 で足すまで描かない。
bool drawnHere(const ui::CanvasDesc& desc) { return desc.mode == ui::CanvasMode::Overlay; }

// 文書の Path 型の決まり（assets/ 相対・長さ）で正規化したパス。通らなければ空。
std::string checkedPath(const std::string& file) {
    const ui::ValueSpec& spec = ui::propInfo(ui::Prop::File).spec;
    const std::string checked = ui::clampValue(spec, ui::textValue(file)).s;
    return checked == file ? checked : std::string();
}

}  // namespace

UiRenderComponent::UiRenderComponent() = default;
UiRenderComponent::~UiRenderComponent() = default;

void UiRenderComponent::ensureAtlas(Scene& scene) {
    if (atlas_) return;
    atlas_ = std::make_unique<ui::FontAtlas>();
    std::vector<std::uint8_t> bytes;
    std::string reason;
    const bool read =
        wizengine::sharedasset::readAssetBytes(kDefaultFontFile, kMaxFontBytes, bytes, reason);
    const int index = read ? atlas_->addFont(kDefaultFontFile, std::move(bytes)) : -1;
    if (index < 0) {
        LOGW("ui", "default font '%s' is not usable (%s) - text is drawn as boxes",
             kDefaultFontFile, reason.empty() ? "not a font" : reason.c_str());
    }
    fontFiles_[kDefaultFontFile] = index;  // 既定のフォントは読み直さない
    atlasTexture_ = scene.renderer().addUiTextureR8(atlas_->size(), atlas_->size());
}

void UiRenderComponent::syncFonts() {
    // 読めなかったファイルは、版が変わったとき（読込・XML の適用）にもう一度試す。
    for (auto it = fontFiles_.begin(); it != fontFiles_.end();) {
        const bool failed = it->second < 0 && it->first != kDefaultFontFile;
        it = failed ? fontFiles_.erase(it) : std::next(it);
    }
    // フォントはファイルごとに 1 回だけ読む（同じファイルを別の名前で宣言しても、
    // 既定のフォントと同じファイルでも、置き場を分けない）。名前はその別名。
    std::unordered_map<std::string, int> names;
    for (const auto& f : snap_.fonts) {
        const std::string file = checkedPath(f.file);
        if (file.empty()) continue;  // 文書の読み込みが警告済み
        auto found = fontFiles_.find(file);
        if (found == fontFiles_.end()) {
            std::vector<std::uint8_t> bytes;
            std::string reason;
            const bool read =
                wizengine::sharedasset::readAssetBytes(file, kMaxFontBytes, bytes, reason);
            const int index = read ? atlas_->addFont(file, std::move(bytes)) : -1;
            if (index < 0) {
                LOGW("ui", "font '%s' (%s) is not usable (%s) - using the default font",
                     f.name.c_str(), file.c_str(), reason.empty() ? "not a font" : reason.c_str());
            }
            found = fontFiles_.emplace(file, index).first;
        }
        if (found->second >= 0) names[f.name] = found->second;
    }
    atlas_->setFontNames(std::move(names));
}

void UiRenderComponent::releaseUnusedImages(Scene& scene) {
    std::set<std::string> used;
    for (const auto& item : snap_.canvases) {
        if (!item.alive || !drawnHere(item.desc)) continue;
        for (const auto& e : elementsOf(item.desc)) {
            const std::string& file = e.props.text(ui::Prop::File);
            if (!file.empty()) used.insert(file);
        }
    }
    for (auto it = images_.begin(); it != images_.end();) {
        const bool keep = it->second >= 0 && used.count(it->first) > 0;
        if (keep) {
            ++it;
            continue;
        }
        // 使わなくなった画像は返す。読めなかった記録は捨てて次でもう一度試す。
        // 返した番号を持つメッシュは Renderer が描かなくなり、版が変わったので
        // この後の組み立てで全 Canvas を組み直す。
        if (it->second >= 0) scene.renderer().releaseUiTexture(it->second);
        it = images_.erase(it);
    }
}

int UiRenderComponent::imageTexture(Scene& scene, const std::string& file) {
    const std::string checked = checkedPath(file);
    if (checked.empty()) return ui::kUiNoTexture;
    const auto it = images_.find(checked);
    if (it != images_.end()) return it->second;
    const int id = scene.renderer().loadUiImage(checked);
    if (id == ui::kUiNoTexture) {
        LOGW("ui", "image '%s' is not readable - drawn as a white panel", checked.c_str());
    }
    images_[checked] = id;  // 読めなかった物も版が変わるまで覚える（警告は 1 回）
    return id;
}

const std::vector<ui::ElementDesc>& UiRenderComponent::elementsOf(
    const ui::CanvasDesc& desc) const {
    if (desc.ui.empty()) return desc.elements;
    for (const auto& a : snap_.uiAssets) {
        if (a.desc.name == desc.ui) return a.desc.elements;
    }
    static const std::vector<ui::ElementDesc> kNone;
    return kNone;
}

void UiRenderComponent::syncCanvases(Scene& scene, bool designChanged) {
    auto& renderer = scene.renderer();
    auto draws = [this](std::size_t i) {
        const bool exists = i < snap_.canvases.size();
        return exists && snap_.canvases[i].alive && drawnHere(snap_.canvases[i].desc);
    };
    // 描かない（生きていない・無くなった・描かない mode の）Canvas の入れ物は壊す。
    for (auto it = canvases_.begin(); it != canvases_.end();) {
        if (draws(it->first)) {
            ++it;
            continue;
        }
        renderer.removeUiCanvas(it->second.rendererId);
        it = canvases_.erase(it);
    }
    for (std::size_t i = 0; i < snap_.canvases.size(); ++i) {
        const auto& item = snap_.canvases[i];
        if (!item.alive) continue;  // 生きていない枠は作らない
        if (!drawnHere(item.desc)) {
            if (notDrawnWarned_.insert(i).second) {
                LOGW("ui", "canvas '%s': mode is not drawn yet (only overlay) - skipped",
                     item.desc.name.c_str());
            }
            continue;
        }
        auto found = canvases_.find(i);
        if (found == canvases_.end()) {
            CanvasSlot slot;
            slot.rendererId = renderer.addUiCanvas();
            found = canvases_.emplace(i, std::move(slot)).first;
            designChanged = true;
        }
        CanvasSlot& slot = found->second;
        if (designChanged) {
            renderer.setUiCanvasOrder(slot.rendererId, item.desc.order);
            renderer.setUiCanvasDesignHeight(slot.rendererId, float(item.desc.height));
        }
        // 出すビュー: その Canvas を出すカメラ（canvasShownOnCamera）のうち、
        // 有効でビューが決まっている（最初の applyTo の後）もの。
        std::vector<std::size_t> views;
        for (std::size_t cam = 0; cam < scene.cameraCount(); ++cam) {
            const bool shown = ui::canvasShownOnCamera(item.desc, int(cam));
            const int view = scene.camera(cam).viewIndex();
            if (shown && scene.cameraActive(cam) && view >= 0) {
                views.push_back(std::size_t(view));
            }
        }
        if (views != slot.views) {
            renderer.setUiCanvasViews(slot.rendererId, views);
            slot.views = views;
            // 外れたビューの条件は捨てる（戻ってきたら組み直す）。
            for (auto b = slot.built.begin(); b != slot.built.end();) {
                const bool kept = std::find(views.begin(), views.end(), b->first) != views.end();
                b = kept ? std::next(b) : slot.built.erase(b);
            }
        }
    }
}

ui::UiMeshData UiRenderComponent::buildOne(Scene& scene, const ui::CanvasDesc& desc,
                                           double canvasWidth) {
    const std::vector<ui::ElementDesc>& elements = elementsOf(desc);
    // 重ね順（設計値 → アニメ → ノード → ポインタ）。今は設計値だけ（後の段は後の PR）。
    const ui::PropSet none;
    std::vector<ui::PropSet> resolved;
    resolved.reserve(elements.size());
    for (const auto& e : elements) resolved.push_back(ui::resolve(e.props, none, none, none));

    ui::UiTextureLookup lookup;
    lookup.fontPage = [this](int /*page*/) { return atlasTexture_; };
    lookup.image = [this, &scene](const std::string& file) { return imageTexture(scene, file); };
    const ui::Rect canvas{0.0, 0.0, canvasWidth, desc.height};
    return ui::buildCanvasMesh(elements, resolved, canvas, *atlas_, lookup);
}

void UiRenderComponent::buildMeshes(Scene& scene, bool all) {
    auto& renderer = scene.renderer();
    const int generation = atlas_->generation();
    for (auto& [index, slot] : canvases_) {
        const ui::CanvasDesc& desc = snap_.canvases[index].desc;
        // 同じ縦横比のビューは同じ中身（組むのは 1 回）。
        std::map<double, ui::UiMeshData> byWidth;
        for (const std::size_t view : slot.views) {
            const auto size = renderer.viewSize(view);
            const double width = ui::canvasRect(desc.height, size.x, size.y).w;
            ViewBuild& b = slot.built[view];
            const bool upToDate =
                b.version == version_ && b.canvasWidth == width && b.generation == generation;
            if (upToDate && !all) continue;
            auto mesh = byWidth.find(width);
            if (mesh == byWidth.end()) mesh = byWidth.emplace(width, buildOne(scene, desc, width)).first;
            renderer.setUiCanvasMesh(slot.rendererId, view, mesh->second);
            b = {version_, width, generation};
        }
    }
}

void UiRenderComponent::uploadAtlas(Scene& scene) {
    ui::AtlasRect r;
    while (atlas_->takeDirtyRect(r)) {
        scene.renderer().updateUiTexture(atlasTexture_, r.x, r.y, r.w, r.h,
                                         atlas_->pixels(r.page).data(), atlas_->size());
    }
}

void UiRenderComponent::onRender(Scene& scene) {
    const bool changed = scene.ui().copyIfChanged(version_, snap_);
    if (snap_.canvases.empty() && canvases_.empty()) return;  // UI の無いシーンは何もしない
    ensureAtlas(scene);
    if (changed) {
        syncFonts();
        releaseUnusedImages(scene);
    }
    syncCanvases(scene, changed);
    buildMeshes(scene, false);
    if (atlas_->needsRebuild()) {
        // 字の置き場が溢れた: 全 Canvas を組んで「使っている字」に印を付けてから
        // 組み直す。字の位置が変わる（世代が進む）ので、全部を組み直して送る。
        atlas_->beginUse();
        for (auto& [index, slot] : canvases_) {
            const ui::CanvasDesc& desc = snap_.canvases[index].desc;
            for (const std::size_t view : slot.views) {
                const auto size = scene.renderer().viewSize(view);
                buildOne(scene, desc, ui::canvasRect(desc.height, size.x, size.y).w);
            }
        }
        const ui::AtlasRebuildResult result = atlas_->rebuild();
        if (result.missing > 0) {
            LOGW("ui", "the glyph atlas is full: %d glyphs are drawn as boxes", result.missing);
        }
        buildMeshes(scene, true);
    }
    uploadAtlas(scene);
}
