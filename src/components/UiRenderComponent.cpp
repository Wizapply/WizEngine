#include "components/UiRenderComponent.h"

#include <algorithm>
#include <climits>
#include <tuple>
#include <unordered_map>

#include "core/Log.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/SharedAssetFile.h"
#include "scene/UiOps.h"
#include "ui/UiHandles.h"
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

// そのビューで Canvas を組む幅（canvasRect の幅 = 基準の高さ × ビューの縦横比）。
// 同じ幅のビューは同じ中身になる（組み立ても字の印付けも幅ごとに 1 回）。
double canvasWidthOn(const wizengine::Renderer& renderer, const ui::CanvasDesc& desc,
                     std::size_t view) {
    const auto size = renderer.viewSize(view);
    return ui::canvasRect(desc.height, size.x, size.y).w;
}

}  // namespace

UiRenderComponent::UiRenderComponent() = default;
UiRenderComponent::~UiRenderComponent() = default;

void UiRenderComponent::ensureAtlas(wizengine::Renderer& renderer) {
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
    atlasTexture_ = renderer.addUiTextureR8(atlas_->size(), atlas_->size());
}

void UiRenderComponent::syncFonts(bool reloaded) {
    // 読めなかったファイルをもう一度試すのは、読込・XML の適用（reloaded）のときと、
    // 宣言から外れたとき（後でまた宣言されたら読み直す）だけ。編集のたびに試すと、
    // 無いファイルを版ごとに読みに行き、警告も毎回出る。
    std::set<std::string> declared;
    for (const auto& f : snap_.fonts) declared.insert(checkedPath(f.file));
    for (auto it = fontFiles_.begin(); it != fontFiles_.end();) {
        const bool retry = reloaded || declared.count(it->first) == 0;
        const bool failed = it->second < 0 && it->first != kDefaultFontFile;
        it = (failed && retry) ? fontFiles_.erase(it) : std::next(it);
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

void UiRenderComponent::releaseUnusedImages(wizengine::Renderer& renderer, bool reloaded) {
    std::set<std::string> used;
    for (const auto& item : snap_.canvases) {
        if (!item.alive || !drawnHere(item.desc)) continue;
        for (const auto& e : snap_.elementsOf(item.desc)) {
            const std::string& file = e.props.text(ui::Prop::File);
            if (!file.empty()) used.insert(file);
        }
    }
    for (auto it = images_.begin(); it != images_.end();) {
        // 読めなかった記録は、まだ使っている間は読込・XML の適用（reloaded）まで残す
        // （編集のたびに無いファイルを読みに行かない。パスを変えれば新しいパスを読む）。
        const bool inUse = used.count(it->first) > 0;
        const bool keepFailed = it->second < 0 && !reloaded;
        const bool keep = inUse && (it->second >= 0 || keepFailed);
        if (keep) {
            ++it;
            continue;
        }
        // 使わなくなった画像は返す。読めなかった記録は捨てて次でもう一度試す。
        // 返した番号を持つメッシュは Renderer が描かなくなり、版が変わったので
        // この後の組み立てで全 Canvas を組み直す。
        if (it->second >= 0) renderer.releaseUiTexture(it->second);
        it = images_.erase(it);
    }
}

int UiRenderComponent::imageTexture(wizengine::Renderer& renderer, const std::string& file) {
    const std::string checked = checkedPath(file);
    if (checked.empty()) return ui::kUiNoTexture;
    const auto it = images_.find(checked);
    if (it != images_.end()) return it->second;
    // 読めなければ Renderer が警告する（報告はそこの 1 か所）。
    const int id = renderer.loadUiImage(checked);
    images_[checked] = id;  // 読めなかった物も覚える（読み直しは releaseUnusedImages が決める）
    return id;
}

void UiRenderComponent::syncCanvases(wizengine::Renderer& renderer, bool designChanged) {
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
        if (!item.alive) {  // 生きていない枠は作らない
            notDrawnWarned_.erase(i);
            continue;
        }
        if (!drawnHere(item.desc)) {
            // 同じ Canvas は mode が変わったときだけ知らせ直す（編集のたびに出さない）。
            const auto warned = notDrawnWarned_.find(i);
            const bool known = warned != notDrawnWarned_.end() && warned->second == item.desc.mode;
            if (!known) {
                LOGW("ui", "canvas '%s': mode is not drawn yet (only overlay) - skipped",
                     item.desc.name.c_str());
                notDrawnWarned_[i] = item.desc.mode;
            }
            continue;
        }
        notDrawnWarned_.erase(i);
        auto found = canvases_.find(i);
        if (found == canvases_.end()) {
            CanvasSlot slot;
            slot.rendererId = renderer.addUiCanvas();
            found = canvases_.emplace(i, std::move(slot)).first;
            designChanged = true;
        }
        CanvasSlot& slot = found->second;
        if (designChanged) {
            renderer.setUiCanvasDesignHeight(slot.rendererId, float(item.desc.height));
        }
        // 出すビュー: その Canvas を出すカメラ（canvasShownOnCamera）のうち、
        // 有効でビューが決まっている（最初の applyTo の後）もの。
        std::vector<std::size_t> views;
        for (std::size_t cam = 0; cam < cameraViews_.size(); ++cam) {
            const bool shown = ui::canvasShownOnCamera(item.desc, int(cam));
            const int view = cameraViews_[cam];
            if (shown && view >= 0) views.push_back(std::size_t(view));
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
    syncRanks(renderer);
}

void UiRenderComponent::syncRanks(wizengine::Renderer& renderer) {
    std::vector<std::size_t> order;
    order.reserve(canvases_.size());
    for (const auto& entry : canvases_) order.push_back(entry.first);
    std::sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
        return ui::canvasBehind(snap_.canvases[a].desc.order, int(a),
                                snap_.canvases[b].desc.order, int(b));
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        CanvasSlot& slot = canvases_[order[rank]];
        if (slot.rank == int(rank)) continue;
        slot.rank = int(rank);
        renderer.setUiCanvasOrder(slot.rendererId, slot.rank);
    }
}

wizengine::UiState::Pointer UiRenderComponent::pointerOn(std::size_t view, int index) const {
    wizengine::UiState::Pointer none;
    if (!simulating_) return none;
    for (std::size_t cam = 0; cam < cameraViews_.size() && cam < pointers_.size(); ++cam) {
        const bool onView = cameraViews_[cam] >= 0 && std::size_t(cameraViews_[cam]) == view;
        if (onView) return pointers_[cam].canvas == index ? pointers_[cam] : none;
    }
    return none;
}

ui::UiMeshData UiRenderComponent::buildOne(wizengine::Renderer& renderer, int index,
                                           const ui::CanvasDesc& desc, double canvasWidth,
                                           const wizengine::UiState::Pointer& pointer) {
    const std::vector<ui::ElementDesc>& elements = snap_.elementsOf(desc);
    // 重ね順（設計値 → アニメ → ノード → ポインタ）。アニメとノードの段は後の PR。
    const ui::PropSet none;
    std::vector<ui::PropSet> resolved;
    resolved.reserve(elements.size());
    for (const auto& e : elements) {
        resolved.push_back(ui::resolve(e.props, none, none, pointer.phaseOf(index, e.id)));
    }

    ui::UiTextureLookup lookup;
    lookup.fontPage = [this](int /*page*/) { return atlasTexture_; };
    lookup.image = [this, &renderer](const std::string& file) {
        return imageTexture(renderer, file);
    };
    const ui::Rect canvas{0.0, 0.0, canvasWidth, desc.height};
    return ui::buildCanvasMesh(elements, resolved, canvas, *atlas_, lookup);
}

void UiRenderComponent::buildMeshes(wizengine::Renderer& renderer, bool all) {
    const int generation = atlas_->generation();
    for (auto& [index, slot] : canvases_) {
        const ui::CanvasDesc& desc = snap_.canvases[index].desc;
        // 組み直すのは、この Canvas の材料（自分・使っている UI・フォント）が変わったときだけ
        // （ほかの Canvas の編集やドラッグでは組まない）。
        const std::uint64_t revision = snap_.contentsRevision(int(index));
        // 同じ縦横比で同じポインタのビューは同じ中身（組むのはその組ごとに 1 回）。
        using Key = std::tuple<double, int, int>;  // 幅・ポインタの上・押下
        struct Pending {
            wizengine::UiState::Pointer pointer;
            std::vector<std::size_t> views;
        };
        std::map<Key, Pending> pending;  // 組 → そのポインタと組み直すビュー
        for (const std::size_t view : slot.views) {
            const double width = canvasWidthOn(renderer, desc, view);
            const wizengine::UiState::Pointer pointer = pointerOn(view, int(index));
            const ViewBuild& b = slot.built[view];
            const bool sameMaterial = b.revision == revision && b.generation == generation;
            const bool samePointer = b.hover == pointer.hover && b.pressed == pointer.pressed;
            const bool upToDate = sameMaterial && b.canvasWidth == width && samePointer;
            if (upToDate && !all) continue;
            Pending& p = pending[{width, pointer.hover, pointer.pressed}];
            p.pointer = pointer;
            p.views.push_back(view);
        }
        for (const auto& [key, p] : pending) {
            const double width = std::get<0>(key);
            const wizengine::UiState::Pointer& pointer = p.pointer;
            const std::vector<std::size_t>& views = p.views;
            ui::UiMeshData mesh = buildOne(renderer, int(index), desc, width, pointer);
            for (std::size_t k = 0; k < views.size(); ++k) {
                // 最後のビューへは渡し切る（それより前は写す）。
                const bool last = k + 1 == views.size();
                renderer.setUiCanvasMesh(slot.rendererId, views[k],
                                         last ? std::move(mesh) : ui::UiMeshData(mesh));
                slot.built[views[k]] = {revision, width, generation, pointer.hover,
                                        pointer.pressed};
            }
        }
    }
}

void UiRenderComponent::uploadAtlas(wizengine::Renderer& renderer) {
    ui::AtlasRect r;
    while (atlas_->takeDirtyRect(r)) {
        renderer.updateUiTexture(atlasTexture_, r.x, r.y, r.w, r.h,
                                         atlas_->pixels(r.page).data(), atlas_->size());
    }
}

void UiRenderComponent::onRender(Scene& scene) {
    // オブジェクトのロックの中: カメラの有効フラグ（構造はこのロックが守る）と
    // ビュー番号を写すだけ（数個の int なので UI の無いシーンでも毎フレーム写す =
    // 写しが古いまま残らない）。
    simulating_ = !scene.editor().isEditor();
    cameraViews_.resize(scene.cameraCount());
    for (std::size_t cam = 0; cam < cameraViews_.size(); ++cam) {
        cameraViews_[cam] = scene.cameraViewIndex(cam);
    }
    // 選択枠の対象: UI の編集を受け付けるとき（editsUiFrom = エディタモード × エディタ
    // カメラ × プレハブ編集中でない）に、要素を選んでいれば。今もあるかは組むときに
    // 写しで確かめる。
    const std::size_t editorCam = scene.editorCamera();
    const EditorState& editor = scene.editor();
    const bool element = editor.selKind() == EditorState::SelKind::Canvas && editor.selSub() >= 0;
    const bool wanted = element && scene.editsUiFrom(editorCam);
    frameCanvas_ = wanted ? editor.selIndex() : -1;
    frameElement_ = wanted ? editor.selSub() : -1;
    frameCamera_ = int(editorCam);
}

void UiRenderComponent::onRenderUnlocked(Scene& scene) {
    // ロックの外で使うのは Renderer と UI の状態だけ（下の関数は Scene を受けない =
    // オブジェクト一覧を読むコードが書けない）。
    update(scene.renderer(), scene.ui());
    updateFrame(scene.renderer());
}

void UiRenderComponent::updateFrame(wizengine::Renderer& renderer) {
    // 出す条件: 対象があり、エディタカメラのビューが決まっていて、その Canvas が映像の
    // 上で枠を出せる物（overlayShownOnCamera）で、要素が今もある。
    const bool camKnown = frameCamera_ >= 0 && std::size_t(frameCamera_) < cameraViews_.size();
    const int view = frameCanvas_ >= 0 && camKnown ? cameraViews_[std::size_t(frameCamera_)] : -1;
    const ui::CanvasDesc* desc = view >= 0 ? snap_.liveCanvas(frameCanvas_) : nullptr;
    const bool canvasOk = desc && ui::overlayShownOnCamera(*desc, frameCamera_);
    const std::vector<ui::ElementDesc>* elements = canvasOk ? &snap_.elementsOf(*desc) : nullptr;
    const std::size_t at = elements ? ui::elementIndex(*elements, frameElement_) : 0;
    const bool shown = elements && at < elements->size();

    FrameKey key;
    if (shown) {
        const auto size = renderer.viewSize(std::size_t(view));
        key = {snap_.contentsRevision(frameCanvas_), frameCanvas_, frameElement_, view, size.x,
               size.y};
    }
    if (key == frameSent_) return;
    frameSent_ = key;
    if (!shown) {
        if (frameCreated_) renderer.setUiCanvasViews(frameRendererId_, {});
        return;
    }
    if (!frameCreated_) {
        frameRendererId_ = renderer.addUiCanvas();
        // どの Canvas よりも手前（ほかの Canvas の順位は 0 から数えた個数までなので、常に
        // いちばん手前）。
        renderer.setUiCanvasOrder(frameRendererId_, INT_MAX);
        frameCreated_ = true;
    }
    renderer.setUiCanvasDesignHeight(frameRendererId_, float(desc->height));
    renderer.setUiCanvasViews(frameRendererId_, {std::size_t(view)});
    // 要素の矩形は描く側（buildOne）と同じく設計値で解く（重ね順の後の段は後の PR）。
    const ui::Rect canvas{0.0, 0.0, canvasWidthOn(renderer, *desc, std::size_t(view)),
                          desc->height};
    const std::vector<ui::LayoutNode> nodes = ui::layoutTree(*elements, {}, canvas);
    // 掴めない要素（読めなかった共用 UI の中身・見えない要素）は枠の線だけ（つまみを出さない。
    // 当たりの規則は ui::frameGrabbable = UiInputComponent と同じ）。
    const bool editable = wizengine::uiops::elementsEditable(snap_.contentsInfo(frameCanvas_));
    const bool handles = ui::frameGrabbable(editable, nodes[at]);
    renderer.setUiCanvasMesh(
        frameRendererId_, std::size_t(view),
        ui::buildFrameMesh(nodes[at].rect, ui::designPerScreenPx(desc->height, key.viewHeight),
                           handles));
}

void UiRenderComponent::update(wizengine::Renderer& renderer, const wizengine::UiState& state) {
    const bool changed = state.copyIfChanged(version_, snap_);
    // ポインタ（実行時の状態）は別の版。変わったビューだけを buildMeshes が組み直す。
    state.copyPointersIfChanged(runtimeVersion_, pointers_);
    if (changed) {
        anyAlive_ = anyAliveCanvas();  // 写したときに 1 回だけ数える
        // 一覧が縮んだ（全消し・読込）ら、外になった番号の警告の記録を捨てる（次に同じ
        // 番号で来た Canvas をまた知らせる）。
        notDrawnWarned_.erase(notDrawnWarned_.lower_bound(snap_.canvases.size()),
                              notDrawnWarned_.end());
    }
    // UI の無いシーンは何もしない（字の置き場もフォントも作らない。作った物は残す）。
    if (canvases_.empty() && !anyAlive_) return;
    ensureAtlas(renderer);
    if (changed) {
        // 中身を丸ごと入れ替えた（読込・XML の適用・全消し）か。読めなかったファイルを
        // 読み直す区切り。
        const bool reloaded = snap_.loadGeneration != loadGeneration_;
        loadGeneration_ = snap_.loadGeneration;
        // フォントの宣言が変わったとき（読込・全消し）だけ読み直す（要素の編集では読まない）。
        if (reloaded || snap_.fontsRevision != fontsRevision_) {
            fontsRevision_ = snap_.fontsRevision;
            syncFonts(reloaded);
        }
        releaseUnusedImages(renderer, reloaded);
    }
    syncCanvases(renderer, changed);
    buildMeshes(renderer, false);
    if (atlas_->needsRebuild()) {
        // 字の位置が変わる（世代が進む）ので、全部を組み直して送る。
        rebuildAtlas(renderer);
        buildMeshes(renderer, true);
    }
    uploadAtlas(renderer);
}

void UiRenderComponent::rebuildAtlas(wizengine::Renderer& renderer) {
    atlas_->beginUse();
    for (auto& [index, slot] : canvases_) {
        const ui::CanvasDesc& desc = snap_.canvases[index].desc;
        // 同じ縦横比のビューは同じ字を使う（buildMeshes と同じく幅ごとに 1 回）。
        std::set<double> widths;
        for (const std::size_t view : slot.views) {
            widths.insert(canvasWidthOn(renderer, desc, view));
        }
        // 字は色だけが変わるポインタの段に依らないので、ポインタ無しで組む。
        for (const double width : widths) {
            buildOne(renderer, int(index), desc, width, wizengine::UiState::Pointer{});
        }
    }
    const ui::AtlasRebuildResult result = atlas_->rebuild();
    if (result.missing > 0) {
        LOGW("ui", "the glyph atlas is full: %d glyphs are drawn as boxes", result.missing);
    }
}

bool UiRenderComponent::anyAliveCanvas() const {
    return std::any_of(snap_.canvases.begin(), snap_.canvases.end(),
                       [](const wizengine::UiState::CanvasItem& c) { return c.alive; });
}
