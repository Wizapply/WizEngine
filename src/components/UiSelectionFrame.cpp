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
                                 const ui::Rect& overlay,
                                 std::vector<std::array<double, 2>>& points) {
        return scene.plateToOverlay(desc, cam, overlay, points);
    };
    project.scale = [&scene](const ui::CanvasDesc& desc, std::size_t cam, int w, int h,
                             double px, double py, double& scale) {
        return scene.plateDesignPerScreenPx(desc, cam, w, h, px, py, scale);
    };
    return project;
}

void UiSelectionFrame::copyTarget(Scene& scene) {
    // UI の編集を受け付けるとき（editsUiFrom = エディタモード × エディタカメラ × プレハブ
    // 編集中でない）に、要素を選んでいれば。
    const std::size_t editorCam = scene.editorCamera();
    const EditorState& editor = scene.editor();
    const bool element = editor.selKind() == EditorState::SelKind::Canvas && editor.selSub() >= 0;
    const bool wanted = element && scene.editsUiFrom(editorCam);
    canvas_ = wanted ? editor.selIndex() : -1;
    element_ = wanted ? editor.selSub() : -1;
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
        key = {snap.contentsRevision(canvas_), canvas_, element_, view, size.x, size.y};
    }
    // 板の枠はカメラが動くと映像の上の位置が変わるので、条件が同じでも組む。
    if (!plate && key == sent_) return;
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
    // 板: 板の設計の画素で枠を組み（つまみと線の太さは要素の中心での映像の 1 画素ぶん =
    // 当たりの UiInputComponent と同じ求め方）、頂点を映像の画素（この Canvas の基準の高さ =
    // ビューの高さの画素）へ写す。
    const std::size_t cam = std::size_t(camera_);
    double scale = 0.0;
    const double cx = rect.x + rect.w * 0.5, cy = rect.y + rect.h * 0.5;
    if (!project.scale(desc, cam, size.x, size.y, cx, cy, scale)) return false;
    mesh = ui::buildFrameMesh(rect, scale, handles);
    std::vector<std::array<double, 2>> points;
    points.reserve(mesh.vertices.size());
    for (const ui::UiVertex& v : mesh.vertices) points.push_back({double(v.x), double(v.y)});
    designHeight = double(size.y);
    const ui::Rect screen = ui::canvasRect(designHeight, size.x, size.y);
    if (!project.toOverlay(desc, cam, screen, points)) return false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        mesh.vertices[i].x = float(points[i][0]);
        mesh.vertices[i].y = float(points[i][1]);
    }
    return true;
}

void UiSelectionFrame::hide(wizengine::Renderer& renderer) {
    if (!created_ || view_ < 0) return;
    renderer.setUiCanvasViews(rendererId_, {});
    view_ = -1;
}
