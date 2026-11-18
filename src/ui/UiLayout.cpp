#include "ui/UiLayout.h"

#include <unordered_map>

namespace wizengine {
namespace ui {

Rect canvasRect(double designHeight, double viewWidth, double viewHeight) {
    const bool hasAspect = viewWidth > 0.0 && viewHeight > 0.0;
    const double aspect = hasAspect ? viewWidth / viewHeight : 1.0;
    return Rect{0.0, 0.0, designHeight * aspect, designHeight};
}

void ndcToCanvas(const Rect& canvas, double x, double y, double& px, double& py) {
    px = canvas.x + (x + 1.0) * 0.5 * canvas.w;
    py = canvas.y + (1.0 - y) * 0.5 * canvas.h;
}

AxisLayout axisLayout(double parentStart, double parentLength, double anchorMin,
                      double anchorMax, double pivot) {
    AxisLayout a;
    a.lo = parentStart + parentLength * anchorMin;
    a.hi = parentStart + parentLength * anchorMax;
    a.pivot = pivot;
    a.ref = a.lo + (a.hi - a.lo) * pivot;
    return a;
}

AxisLayout axisLayoutOf(const Rect& parent, const PropSet& props, int axis) {
    const PropValue& anchor = props.get(Prop::Anchor);
    const PropValue& pivot = props.get(Prop::Pivot);
    const std::size_t k = axis == 0 ? 0 : 1;
    const double start = k == 0 ? parent.x : parent.y;
    const double length = k == 0 ? parent.w : parent.h;
    return axisLayout(start, length, anchor.v[k], anchor.v[k + 2], pivot.v[k]);
}

Rect solveRect(const Rect& parent, const PropSet& props) {
    const PropValue& pos = props.get(Prop::Pos);
    const PropValue& size = props.get(Prop::Size);
    const AxisLayout ax = axisLayoutOf(parent, props, 0);
    const AxisLayout ay = axisLayoutOf(parent, props, 1);
    Rect r;
    r.w = ax.length(size.v[0]);
    r.h = ay.length(size.v[1]);
    r.x = ax.start(pos.v[0], r.w);
    r.y = ay.start(pos.v[1], r.h);
    return r;
}

std::vector<int> parentIndices(const std::vector<ElementDesc>& elements) {
    std::unordered_map<int, int> indexOfId;
    std::vector<int> out(elements.size(), -1);
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const auto it = indexOfId.find(elements[i].parent);
        // まだ登録していない = 自分より前に無い（自分自身・後ろ・存在しない）。
        if (elements[i].parent >= 0 && it != indexOfId.end()) out[i] = it->second;
        indexOfId.emplace(elements[i].id, int(i));
    }
    return out;
}

const PropSet& elementProps(const std::vector<ElementDesc>& elements,
                            const std::vector<PropSet>& resolved, std::size_t i) {
    return i < resolved.size() ? resolved[i] : elements[i].props;
}

bool elementVisible(const std::vector<ElementDesc>& elements,
                    const std::vector<PropSet>& resolved, const std::vector<int>& parents,
                    std::size_t i) {
    // 親は必ず前にある（parentIndices）ので、たどれば必ず根で止まる。
    for (int k = int(i); k >= 0; k = parents[std::size_t(k)]) {
        if (!elementProps(elements, resolved, std::size_t(k)).flag(Prop::Visible)) return false;
    }
    return true;
}

std::vector<LayoutNode> layoutTree(const std::vector<ElementDesc>& elements,
                                   const std::vector<PropSet>& props,
                                   const Rect& canvas) {
    const std::vector<int> parents = parentIndices(elements);
    std::vector<LayoutNode> out(elements.size());
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const PropSet& p = elementProps(elements, props, i);
        LayoutNode& n = out[i];
        n.parent = parents[i];
        // 親は必ず前にあるので、もう解けている。
        const bool hasParent = n.parent >= 0;
        const Rect& parentRect = hasParent ? out[std::size_t(n.parent)].rect : canvas;
        n.rect = solveRect(parentRect, p);
        n.visible = elementVisible(elements, props, parents, i);
    }
    return out;
}

int hitTest(const std::vector<LayoutNode>& nodes, double x, double y,
            const std::function<bool(int)>& accept) {
    // 後ろほど手前なので、後ろから見て最初に当たったもの。
    for (std::size_t k = nodes.size(); k-- > 0;) {
        const LayoutNode& n = nodes[k];
        if (!n.visible || !n.rect.contains(x, y)) continue;
        if (accept && !accept(int(k))) continue;
        return int(k);
    }
    return -1;
}

}  // namespace ui
}  // namespace wizengine
