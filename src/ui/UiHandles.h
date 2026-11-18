#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "ui/UiLayout.h"
#include "ui/UiMesh.h"
#include "ui/UiProps.h"
#include "ui/UiTypes.h"

// 映像の上で UI の要素を動かす・大きさを変えるための、枠と 8 つのつまみ（純粋な計算）。
//
// 座標は UiLayout と同じ設計上の画素（左上が原点・y 下向き）。つまみの大きさは画面の
// 画素で決めるので、呼び手が「設計の画素で半分の大きさ」（half）に直して渡す。
//   当たり      … hitHandle（どのつまみか、枠の中か。枠の外は当たらない）
//   ドラッグ    … dragPlacement（開始時の性質 + 動かした量 → 新しい pos / size）
//   選択枠      … frameShape（四角の並び）/ buildFrameMesh（UI のメッシュ）
// ドラッグは**開始時の値だけ**から計算する（今の値からの差分を取らない = 当てた結果が
// 次の入力に混ざらない）。Chrono / Filament / Scene を知らない（tests/ui で検査する）。
namespace wizengine {
namespace ui {

// つまみ。Body は枠の中（全体を動かす）。辺の真ん中 4 つと角 4 つが大きさを変える。
enum class FrameHandle : int {
    None = -1, Body = 0,
    Left, Right, Top, Bottom,
    TopLeft, TopRight, BottomLeft, BottomRight
};

// 描くつまみの一辺（画面の画素）と枠の線の太さ（画面の画素）。画面の画素は映像の画素で、
// ブラウザは映像を縮めて表示する（1920 幅を半分ほどにしても見える大きさにしてある）。
inline constexpr double kHandleScreenPx = 14.0;
// つまみの当たりの一辺（画面の画素）。描く四角より大きくする（縮めて表示した映像の数画素の
// 四角は狙って押せないため）。枠の外へのはみ出しの当たりも同じ幅。
inline constexpr double kHandleHitScreenPx = 28.0;
inline constexpr double kFrameLineScreenPx = 2.0;
// スナップの刻み（設計の画素）。スナップしないときも 1 画素の刻みに揃える
// （保存される値が小数の端数だらけにならない）。
inline constexpr double kSnapStepPx = 10.0;
inline constexpr double kFineStepPx = 1.0;
// 縮めたときの最小の大きさ（設計の画素）。始めからこれより小さい要素はその大きさまで。
inline constexpr double kMinElementPx = 4.0;

// つまみが動かす辺（軸ごと）: -1 = 小さい側の辺、+1 = 大きい側の辺、2 = 両方（移動）、
// 0 = 動かさない。
struct HandleEdges {
    int x = 0, y = 0;
};
inline HandleEdges handleEdges(FrameHandle h) {
    switch (h) {
        case FrameHandle::Body: return {2, 2};
        case FrameHandle::Left: return {-1, 0};
        case FrameHandle::Right: return {1, 0};
        case FrameHandle::Top: return {0, -1};
        case FrameHandle::Bottom: return {0, 1};
        case FrameHandle::TopLeft: return {-1, -1};
        case FrameHandle::TopRight: return {1, -1};
        case FrameHandle::BottomLeft: return {-1, 1};
        case FrameHandle::BottomRight: return {1, 1};
        case FrameHandle::None: break;
    }
    return {0, 0};
}

// 選択枠の色（リニア値。UI のビューは後処理なし = この値のまま出る）。線は橙、つまみは
// 白の四角に暗い縁。
inline constexpr float kFrameLineColor[4] = {1.0f, 0.45f, 0.0f, 1.0f};
inline constexpr float kHandleFillColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
inline constexpr float kHandleEdgeColor[4] = {0.05f, 0.05f, 0.05f, 1.0f};

// 画面の画素 → 設計の画素（Canvas の高さ = 基準の高さが、ビューの高さの画素に重なる）。
// つまみの当たり（INPUT）と描く大きさ（RENDER）が同じ換算を使う。
inline double designPerScreenPx(double designHeight, int viewHeight) {
    return viewHeight > 0 ? designHeight / double(viewHeight) : 1.0;
}

// 描くつまみの一辺の半分（設計の画素）。scale = designPerScreenPx（buildFrameMesh が使う）。
inline double handleHalfDesign(double scale) { return kHandleScreenPx * 0.5 * scale; }
// つまみの当たりの一辺の半分（設計の画素。hitHandle の half に渡す。換算は handleHalfDesign と
// 同じ）。
inline double handleHitHalfDesign(double scale) { return kHandleHitScreenPx * 0.5 * scale; }

// 映像の上で枠を出して動かせる Canvas か（唯一の定義）: 映像に重ねる overlay（camera /
// world は板の上の座標になるので、映像の位置をそのまま Canvas の画素に写せない）で、
// そのカメラに出ている。
inline bool frameShownOnCamera(const CanvasDesc& desc, int camera) {
    return desc.mode == CanvasMode::Overlay && canvasShownOnCamera(desc, camera);
}

// 選択枠のつまみを出して映像の上で掴めるか（唯一の定義。当たり = UiInputComponent、描く =
// UiRenderComponent）: 中身を編集してよく（editable = uiops::elementsEditable の答え）、要素が
// 見えている（自分と祖先が visible）。掴めない要素も枠の線は出す（Inspector の数字で直す）。
inline bool frameGrabbable(bool editable, const LayoutNode& node) {
    return editable && node.visible;
}

// 大きさを変える 8 つのつまみ（当たりは角を先に見るので、この並びで調べる）。
inline constexpr FrameHandle kResizeHandles[] = {
    FrameHandle::TopLeft, FrameHandle::TopRight, FrameHandle::BottomLeft,
    FrameHandle::BottomRight, FrameHandle::Left, FrameHandle::Right,
    FrameHandle::Top, FrameHandle::Bottom};

// つまみの中心（辺の -1 / +1 は矩形の端、0 は真ん中）。
inline void handleCenter(const Rect& r, FrameHandle h, double& x, double& y) {
    const HandleEdges e = handleEdges(h);
    auto at = [](double start, double len, int edge) {
        return edge < 0 ? start : edge == 1 ? start + len : start + len * 0.5;
    };
    x = at(r.x, r.w, e.x);
    y = at(r.y, r.h, e.y);
}

// 点 (px, py) が当たるつまみ。half = つまみの当たりの一辺の半分（設計の画素。
// handleHitHalfDesign）。角 → 辺 → 枠の中（外へ half だけはみ出した所 = 線の上も含む）の
// 順で、どれにも当たらなければ None（枠の外の押下は、後ろの 3D の物・別の UI の選択へ流す）。
// つまみの当たりは要素の短い辺の 1/4 までに抑える（唯一の決め所）: 小さい要素でも真ん中に
// 動かす場所（Body）が残る。描く大きさは抑えない。
inline FrameHandle hitHandle(const Rect& r, double px, double py, double half) {
    const double handleHalf = std::min(half, std::min(r.w, r.h) * 0.25);
    for (const FrameHandle h : kResizeHandles) {
        double cx = 0.0, cy = 0.0;
        handleCenter(r, h, cx, cy);
        if (std::fabs(px - cx) <= handleHalf && std::fabs(py - cy) <= handleHalf) return h;
    }
    const bool insideX = px >= r.x - half && px <= r.x + r.w + half;
    const bool insideY = py >= r.y - half && py <= r.y + r.h + half;
    return insideX && insideY ? FrameHandle::Body : FrameHandle::None;
}

// ドラッグの結果（要素の性質の pos と size）。
struct Placement {
    double pos[2] = {0.0, 0.0};
    double size[2] = {0.0, 0.0};
    bool operator==(const Placement& o) const {
        return pos[0] == o.pos[0] && pos[1] == o.pos[1] && size[0] == o.size[0] &&
               size[1] == o.size[1];
    }
    bool operator!=(const Placement& o) const { return !(*this == o); }
};

// 1 軸ぶん。解き方は solveRect と同じ AxisLayout（UiLayout.h）で、動かした辺について
// 逆に解く（長さ → size、始点 → pos）。動かした辺は親の始点からの刻み（step、0 以下 =
// 刻まない）に揃える。edge は HandleEdges の値。
inline void dragAxis(const AxisLayout& axis, double parentStart, double pos, double size,
                     int edge, double d, double step, double& outPos, double& outSize) {
    outPos = pos;
    outSize = size;
    if (edge == 0) return;
    const double len0 = axis.length(size);
    const double start0 = axis.start(pos, len0);
    const double end0 = start0 + len0;
    auto snap = [parentStart, step](double v) {
        return step > 0.0 ? parentStart + std::round((v - parentStart) / step) * step : v;
    };
    const double lower = std::min(kMinElementPx, len0);
    double start = start0, len = len0;
    if (edge == 2) {
        start = snap(start0 + d);
    } else if (edge > 0) {
        len = std::max(lower, snap(end0 + d) - start0);
    } else {
        len = std::max(lower, end0 - snap(start0 + d));
        start = end0 - len;
    }
    outSize = axis.sizeFor(len);
    outPos = axis.posFor(start, len);
}

// 開始時の性質 start（Anchor / Pivot / Pos / Size）と親の矩形から、つまみ h を
// (dx, dy)（設計の画素）だけ動かしたときの pos / size。step は動かした辺の刻み。
inline Placement dragPlacement(const Rect& parent, const PropSet& start, FrameHandle h,
                               double dx, double dy, double step) {
    const PropValue& pos = start.get(Prop::Pos);
    const PropValue& size = start.get(Prop::Size);
    const HandleEdges e = handleEdges(h);
    Placement out;
    dragAxis(axisLayoutOf(parent, start, 0), parent.x, pos.v[0], size.v[0], e.x, dx, step,
             out.pos[0], out.size[0]);
    dragAxis(axisLayoutOf(parent, start, 1), parent.y, pos.v[1], size.v[1], e.y, dy, step,
             out.pos[1], out.size[1]);
    return out;
}

// 選択枠の形（設計の画素の四角の並び）: lines = 枠の線 4 本（辺を中心に太さ line）、
// handles = 8 つのつまみ（一辺 2 × half）。
struct FrameShape {
    std::vector<Rect> lines;
    std::vector<Rect> handles;
};
inline FrameShape frameShape(const Rect& r, double line, double half) {
    FrameShape s;
    const double t = line * 0.5;
    s.lines = {Rect{r.x - t, r.y - t, r.w + line, line},
               Rect{r.x - t, r.y + r.h - t, r.w + line, line},
               Rect{r.x - t, r.y - t, line, r.h + line},
               Rect{r.x + r.w - t, r.y - t, line, r.h + line}};
    for (const FrameHandle h : kResizeHandles) {
        double cx = 0.0, cy = 0.0;
        handleCenter(r, h, cx, cy);
        s.handles.push_back(Rect{cx - half, cy - half, half * 2.0, half * 2.0});
    }
    return s;
}

// 選択枠のメッシュ（設計の画素。テクスチャ無し = 白に色を掛ける）。scale =
// designPerScreenPx。handles = false ならつまみを描かない（動かせない要素の枠）。
inline UiMeshData buildFrameMesh(const Rect& r, double scale, bool handles) {
    UiMeshData mesh;
    const UiTextureLookup none;
    MeshSink sink(mesh, none);
    const double line = kFrameLineScreenPx * scale;
    const FrameShape shape = frameShape(r, line, handleHalfDesign(scale));
    auto put = [&sink](const Rect& rect, const float color[4]) {
        UiQuad q;
        q.rect = rect;
        for (int k = 0; k < 4; ++k) q.color[k] = color[k];
        sink.quad(q, rect);
    };
    for (const Rect& l : shape.lines) put(l, kFrameLineColor);
    if (!handles) return mesh;
    for (const Rect& h : shape.handles) {
        put(Rect{h.x - line * 0.5, h.y - line * 0.5, h.w + line, h.h + line}, kHandleEdgeColor);
        put(h, kHandleFillColor);
    }
    return mesh;
}

}  // namespace ui
}  // namespace wizengine
