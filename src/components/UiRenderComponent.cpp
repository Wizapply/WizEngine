#include "components/UiRenderComponent.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <unordered_map>

#include "core/Log.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/SharedAssetFile.h"
#include "ui/UiLayout.h"
#include "ui/UiMesh.h"
#include "ui/UiPlane.h"
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

// 板の中身は全ビューで 1 つ（ui/UiPlane.h）なので、送った条件はこの番号 1 つに覚える。
constexpr std::size_t kPlateBuildKey = std::size_t(-1);

// ui::plateUnitToWorld（列優先の 16 個）→ Renderer の行列。
filament::math::mat4f toMat4(const std::array<float, 16>& m) {
    using filament::math::float4;
    return filament::math::mat4f(float4{m[0], m[1], m[2], m[3]}, float4{m[4], m[5], m[6], m[7]},
                                 float4{m[8], m[9], m[10], m[11]},
                                 float4{m[12], m[13], m[14], m[15]});
}

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
        if (!item.alive) continue;
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
    // 描かない（生きていない・無くなった）Canvas と、overlay ⇄ 板が変わった Canvas の
    // 入れ物は壊す（Renderer の作りが違う。この後で作り直す）。
    auto keeps = [this](std::size_t i, const CanvasSlot& slot) {
        const bool exists = i < snap_.canvases.size() && snap_.canvases[i].alive;
        return exists && ui::isPlateCanvas(snap_.canvases[i].desc) == slot.plate;
    };
    for (auto it = canvases_.begin(); it != canvases_.end();) {
        if (keeps(it->first, it->second)) {
            ++it;
            continue;
        }
        renderer.removeUiCanvas(it->second.rendererId);
        it = canvases_.erase(it);
    }
    for (std::size_t i = 0; i < snap_.canvases.size(); ++i) {
        const auto& item = snap_.canvases[i];
        if (!item.alive) continue;  // 生きていない枠は作らない
        auto found = canvases_.find(i);
        if (found == canvases_.end()) {
            CanvasSlot slot;
            slot.plate = ui::isPlateCanvas(item.desc);
            using Placement = wizengine::Renderer::UiCanvasPlacement;
            slot.rendererId =
                renderer.addUiCanvas(slot.plate ? Placement::Plate : Placement::Overlay);
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
            // 外れたビューの条件は捨てる（戻ってきたら組み直す）。板の中身はビューに依らない。
            for (auto b = slot.built.begin(); b != slot.built.end() && !slot.plate;) {
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

wizengine::UiState::Pointer UiRenderComponent::plateCanvasPointer(const CanvasSlot& slot,
                                                                int index) const {
    // 板の中身は全ビューで 1 つなので、どれかのビューのカメラが上に居ればその見た目にする
    // （先に見つかったビュー）。
    for (const std::size_t view : slot.views) {
        const wizengine::UiState::Pointer p = pointerOn(view, index);
        if (p.hover >= 0 || p.pressed >= 0) return p;
    }
    return wizengine::UiState::Pointer{};
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
    // 重ね順（設計値 → アニメ → ノード → ポインタ。重ね方は ui::resolveElements の 1 か所）。
    // アニメの段は後の PR。
    const std::vector<ui::PropSet> resolved = ui::resolveElements(
        elements, overrides_.of(index),
        [&pointer, index](int id) { return pointer.phaseOf(index, id); });

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
        // ノードの上書き（実行時の状態）の版。変わった Canvas だけ組み直す。
        const std::uint64_t overrides = overrides_.revision(int(index));
        if (slot.plate) {
            buildPlate(renderer, int(index), slot, {revision, overrides, generation}, all);
            continue;
        }
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
            const bool sameMaterial = b.revision == revision && b.generation == generation &&
                                      b.overrides == overrides;
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
                                        pointer.pressed, overrides};
            }
        }
    }
}

void UiRenderComponent::buildPlate(wizengine::Renderer& renderer, int index, CanvasSlot& slot,
                                   const Material& material, bool all) {
    const ui::CanvasDesc& desc = snap_.canvases[std::size_t(index)].desc;
    const double width = ui::plateCanvasWidth(desc);
    const wizengine::UiState::Pointer pointer = plateCanvasPointer(slot, index);
    const ViewBuild& b = slot.built[kPlateBuildKey];
    const bool sameMaterial = b.revision == material.revision &&
                              b.generation == material.generation &&
                              b.overrides == material.overrides;
    const bool samePointer = b.hover == pointer.hover && b.pressed == pointer.pressed;
    const bool upToDate = sameMaterial && b.canvasWidth == width && samePointer;
    if (upToDate && !all) return;
    // 板の Renderer はビュー番号を見ない（Renderer.h の setUiCanvasMesh）。
    renderer.setUiCanvasMesh(slot.rendererId, 0, buildOne(renderer, index, desc, width, pointer));
    slot.built[kPlateBuildKey] = {material.revision, width,          material.generation,
                                  pointer.hover,     pointer.pressed, material.overrides};
}

void UiRenderComponent::placePlates(wizengine::Renderer& renderer, const PlaneFn& planeOf) {
    // 板の姿勢を出すビューごとに毎フレーム送る（camera の板は描くフレームのカメラに付く）。
    // world の板はカメラに依らないので 1 回だけ組んで全ビューへ。姿勢を出せない（camera の
    // 板でカメラの基底が作れない・板の大きさが 0）ビューは姿勢を忘れさせる = そのビューに
    // 映さない（前の姿勢のまま残さない）。
    for (const auto& [index, slot] : canvases_) {
        if (!slot.plate || cameraViews_.empty()) continue;
        const ui::CanvasDesc& desc = snap_.canvases[index].desc;
        const bool world = desc.mode == ui::CanvasMode::World;
        ui::CanvasPlane worldPlane;
        const bool worldPosed = world && planeOf(desc, 0, worldPlane);  // カメラは見ない
        const filament::math::mat4f worldMatrix =
            worldPosed ? toMat4(ui::plateUnitToWorld(worldPlane)) : filament::math::mat4f{};
        for (std::size_t cam = 0; cam < cameraViews_.size(); ++cam) {
            const int view = cameraViews_[cam];
            if (view < 0) continue;
            const bool listed = std::find(slot.views.begin(), slot.views.end(),
                                          std::size_t(view)) != slot.views.end();
            if (!listed) continue;
            ui::CanvasPlane plane;
            const bool posed = world ? worldPosed : planeOf(desc, cam, plane);
            if (!posed) {
                renderer.clearUiCanvasTransform(slot.rendererId, std::size_t(view));
                continue;
            }
            renderer.setUiCanvasTransform(slot.rendererId, std::size_t(view),
                                          world ? worldMatrix
                                                : toMat4(ui::plateUnitToWorld(plane)));
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
    frame_.copyTarget(scene);  // 選択枠の対象（今もあるかは組むときに写しで確かめる）
}

void UiRenderComponent::onRenderUnlocked(Scene& scene) {
    // ロックの外で使うのは Renderer と UI の状態だけ（下の関数は Scene を受けない =
    // オブジェクト一覧を読むコードが書けない）。
    // 板の姿勢はこの時点のカメラから出す（applyToRenderer がイベントのカメラの注文を
    // 当てた後 = main がこの後ビューへ当てる視点）。カメラの値は atomic（Scene::canvasPlane）。
    const PlaneFn planeOf = [&scene](const ui::CanvasDesc& desc, std::size_t cam,
                                     ui::CanvasPlane& out) {
        return scene.canvasPlane(desc, cam, out);
    };
    update(scene.renderer(), scene.ui(), planeOf);
    frame_.update(scene.renderer(), snap_, cameraViews_, UiSelectionFrame::projectionOf(scene));
}

void UiRenderComponent::update(wizengine::Renderer& renderer, const wizengine::UiState& state,
                               const PlaneFn& planeOf) {
    const bool changed = state.copyIfChanged(version_, snap_);
    // 実行時の状態（ポインタ・ノードの上書き）は別の版。変わったビュー・Canvas だけを
    // buildMeshes が組み直す。
    state.copyPointersIfChanged(pointersVersion_, pointers_);
    state.copyOverridesIfChanged(overridesVersion_, overrides_);
    if (changed) {
        anyAlive_ = anyAliveCanvas();  // 写したときに 1 回だけ数える
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
    placePlates(renderer, planeOf);
    uploadAtlas(renderer);
}

void UiRenderComponent::rebuildAtlas(wizengine::Renderer& renderer) {
    atlas_->beginUse();
    for (auto& [index, slot] : canvases_) {
        const ui::CanvasDesc& desc = snap_.canvases[index].desc;
        // 同じ縦横比のビューは同じ字を使う（buildMeshes と同じく幅ごとに 1 回）。
        std::set<double> widths;
        if (slot.plate) widths.insert(ui::plateCanvasWidth(desc));  // 板はビューに依らない
        for (const std::size_t view : slot.views) {
            if (!slot.plate) widths.insert(canvasWidthOn(renderer, desc, view));
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
