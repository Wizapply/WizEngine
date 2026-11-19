#include "components/UiSelectionFrame.h"

#include <climits>
#include <utility>

#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/UiOps.h"
#include "ui/UiHandles.h"
#include "ui/UiPlane.h"

namespace ui = wizengine::ui;

UiSelectionFrame::Projection UiSelectionFrame::projectionOf(const Scene& scene) {
    Projection project;
    project.toOverlay = [&scene](const ui::CanvasDesc& desc, std::size_t cam,
                                 const ui::Rect& overlay, ui::UiMeshData& mesh) {
        return scene.plateMeshToOverlay(desc, cam, overlay, mesh);
    };
    project.scale = [&scene](const ui::CanvasDesc& desc, std::size_t cam, int w, int h,
                             const ui::Rect& rect, double& scale) {
        return scene.plateDesignPerScreenPx(desc, cam, w, h, rect, scale);
    };
    project.cameraPose = [&scene](std::size_t cam) { return scene.cameraEyeTarget(cam); };
    return project;
}

void UiSelectionFrame::copyTarget(Scene& scene) {
    // UI の編集を受け付けるとき（editsUiFrom = エディタモード × エディタカメラ × プレハブ
    // 編集中でない）に、要素を選んでいれば。
    const std::size_t editorCam = scene.editorCamera();
    const EditorState::Selection sel = scene.editor().selection();  // 組で 1 回だけ読む
    const bool element = sel.kind == EditorState::SelKind::Canvas && sel.sub >= 0;
    const bool wanted = element && scene.editsUiFrom(editorCam);
    canvas_ = wanted ? sel.index : -1;
    element_ = wanted ? sel.sub : -1;
    camera_ = int(editorCam);
}

void UiSelectionFrame::update(wizengine::Renderer& renderer,
                              const wizengine::UiState::Snapshot& snap,
                              const std::vector<int>& cameraViews, const Projection& project) {
    // 出す条件: 対象があり、エディタカメラのビューが決まっていて、その Canvas がエディタ
    // カメラに出ていて（canvasShownOnCamera。overlay も板も）、要素が今もある。
    const bool camKnown = camera_ >= 0 && std::size_t(camera_) < cameraViews.size();
    const int view = canvas_ >= 0 && camKnown ? cameraViews[std::size_t(camera_)] : -1;
    const ui::CanvasDesc* desc = view >= 0 ? snap.liveCanvas(canvas_) : nullptr;
    const bool canvasOk = desc && ui::canvasShownOnCamera(*desc, camera_);
    const std::vector<ui::ElementDesc>* elements = canvasOk ? &snap.elementsOf(*desc) : nullptr;
    const std::size_t at = elements ? ui::elementIndex(*elements, element_) : 0;
    const bool shown = elements && at < elements->size();
    const bool plate = shown && ui::isPlateCanvas(*desc);

    Key key;
    if (shown) {
        const auto size = renderer.viewSize(std::size_t(view));
        key.revision = snap.contentsRevision(canvas_);
        key.placement = snap.canvases[std::size_t(canvas_)].placementRevision;
        key.canvas = canvas_;
        key.element = element_;
        key.view = view;
        key.viewWidth = size.x;
        key.viewHeight = size.y;
        // 板の枠はカメラが動くと映像の上の位置が変わる。
        if (plate) key.camera = project.cameraPose(std::size_t(camera_));
    }
    if (key == sent_) return;
    sent_ = key;
    ui::UiMeshData mesh;
    double designHeight = 0.0;
    const bool built =
        shown && build(renderer, snap, *desc, *elements, at, view, project, mesh, designHeight);
    if (!built) {
        hide(renderer);
        return;
    }
    if (!created_) {
        rendererId_ = renderer.addUiCanvas();
        // どの Canvas よりも手前（ほかの Canvas の順位は 0 から数えた個数までなので、常に
        // いちばん手前）。
        renderer.setUiCanvasOrder(rendererId_, INT_MAX);
        created_ = true;
    }
    if (designHeight != designHeight_) {
        renderer.setUiCanvasDesignHeight(rendererId_, float(designHeight));
        designHeight_ = designHeight;
    }
    if (view != view_) {
        renderer.setUiCanvasViews(rendererId_, {std::size_t(view)});
        view_ = view;
    }
    renderer.setUiCanvasMesh(rendererId_, std::size_t(view), std::move(mesh));
}

bool UiSelectionFrame::build(wizengine::Renderer& renderer,
                             const wizengine::UiState::Snapshot& snap, const ui::CanvasDesc& desc,
                             const std::vector<ui::ElementDesc>& elements, std::size_t at,
                             int view, const Projection& project, ui::UiMeshData& mesh,
                             double& designHeight) const {
    const auto size = renderer.viewSize(std::size_t(view));
    if (size.x <= 0 || size.y <= 0) return false;
    // 要素の矩形は描く側（UiRenderComponent::buildOne）と同じく設計値で解く（重ね順の後の段は
    // 後の PR）。
    const ui::Rect canvas = ui::designCanvasRect(desc, size.x, size.y);
    const std::vector<ui::LayoutNode> nodes = ui::layoutTree(elements, {}, canvas);
    // 掴めない要素（読めなかった共用 UI の中身・見えない要素）は枠の線だけ（つまみを出さない。
    // 当たりの規則は ui::frameGrabbable = UiInputComponent と同じ）。
    const bool editable = wizengine::uiops::elementsEditable(snap.contentsInfo(canvas_));
    const bool handles = ui::frameGrabbable(editable, nodes[at]);
    const ui::Rect& rect = nodes[at].rect;
    if (!ui::isPlateCanvas(desc)) {
        designHeight = desc.height;
        mesh = ui::buildFrameMesh(rect, ui::designPerScreenPx(desc.height, size.y), handles);
        return true;
    }
    // 板: 板の設計の画素で枠を組み（つまみと線の太さは要素の見えている所での映像の 1 画素
    // ぶん = 当たりの UiInputComponent と同じ求め方）、映像の画素（この Canvas の基準の高さ =
    // ビューの高さの画素）へ写す（カメラの後ろの部分は切る）。
    const std::size_t cam = std::size_t(camera_);
    double scale = 0.0;
    if (!project.scale(desc, cam, size.x, size.y, rect, scale)) return false;
    mesh = ui::buildFrameMesh(rect, scale, handles);
    designHeight = double(size.y);
    const ui::Rect screen = ui::canvasRect(designHeight, size.x, size.y);
    return project.toOverlay(desc, cam, screen, mesh);
}

void UiSelectionFrame::hide(wizengine::Renderer& renderer) {
    if (!created_ || view_ < 0) return;
    renderer.setUiCanvasViews(rendererId_, {});
    view_ = -1;
}
