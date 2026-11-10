#include "ui/UiLayout.h"

#include <unordered_map>

namespace wizengine {
namespace ui {

Rect canvasRect(double designHeight, double viewWidth, double viewHeight) {
    const bool hasAspect = viewWidth > 0.0 && viewHeight > 0.0;
    const double aspect = hasAspect ? viewWidth / viewHeight : 1.0;
    return Rect{0.0, 0.0, designHeight * aspect, designHeight};
}

Rect solveRect(const Rect& parent, const PropSet& props) {
    const PropValue& anchor = props.get(Prop::Anchor);
    const PropValue& pivot = props.get(Prop::Pivot);
    const PropValue& pos = props.get(Prop::Pos);
    const PropValue& size = props.get(Prop::Size);
    // 1 軸ぶん: 親の始点と長さ、アンカーの min / max、ピボット、位置、大きさ。
    auto axis = [](double start, double length, double aMin, double aMax, double pv,
                   double p, double s, double& outStart, double& outLength) {
        const double lo = start + length * aMin;
        const double hi = start + length * aMax;
        const double len = hi - lo + s;
        outLength = len > 0.0 ? len : 0.0;
        const double ref = lo + (hi - lo) * pv + p;
        outStart = ref - outLength * pv;
    };
    Rect r;
    axis(parent.x, parent.w, anchor.v[0], anchor.v[2], pivot.v[0], pos.v[0], size.v[0],
         r.x, r.w);
    axis(parent.y, parent.h, anchor.v[1], anchor.v[3], pivot.v[1], pos.v[1], size.v[1],
         r.y, r.h);
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

std::vector<LayoutNode> layoutTree(const std::vector<ElementDesc>& elements,
                                   const std::vector<PropSet>& props,
                                   const Rect& canvas) {
    const std::vector<int> parents = parentIndices(elements);
    std::vector<LayoutNode> out(elements.size());
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const PropSet& p = i < props.size() ? props[i] : elements[i].props;
        LayoutNode& n = out[i];
        n.parent = parents[i];
        // 親は必ず前にあるので、もう解けている。
        const bool hasParent = n.parent >= 0;
        const Rect& parentRect = hasParent ? out[std::size_t(n.parent)].rect : canvas;
        const bool parentVisible = hasParent ? out[std::size_t(n.parent)].visible : true;
        n.rect = solveRect(parentRect, p);
        n.visible = parentVisible && p.flag(Prop::Visible);
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
