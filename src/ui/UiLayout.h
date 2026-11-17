#pragma once

#include <functional>
#include <vector>

#include "ui/UiProps.h"
#include "ui/UiTypes.h"

// UI の矩形の解き方と当たり判定（座標系の唯一の定義）。
//
// 座標は**左上が原点で y は下向き**、単位は Canvas の設計上の画素（基準の高さ
// = CanvasDesc::height に対する値）。要素の矩形は親の矩形の中で、アンカー・
// ピボット・位置・大きさから決まる:
//   アンカー (minX minY maxX maxY) は親の矩形の中の 0..1 の範囲。min = max なら
//   1 点、違えば親と一緒に伸び縮みする範囲。
//   幅 = 親の幅 × (maxX − minX) + size.x（高さも同じ。負にはしない）。
//   要素のピボット（自分の矩形の中の 0..1 の点）が、アンカーの範囲の中の
//   同じ割合の点 + pos に来る。
// 描く順は文書順で、後ろの要素ほど手前。
namespace wizengine {
namespace ui {

struct Rect {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    bool contains(double px, double py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Canvas 全体の矩形（設計上の画素）。高さは基準の高さ、幅は映像の縦横比に
// 合わせる。viewWidth / viewHeight が 0 以下なら正方形。
Rect canvasRect(double designHeight, double viewWidth, double viewHeight);

// 1 軸ぶんの解き方（solveRect の式の唯一の定義。映像の上のドラッグ = ui/UiHandles.h は
// 同じ式を逆に使う）。lo / hi = アンカーの範囲（親の中の位置）、ref = ピボットが来る点
// から pos を引いた点。
struct AxisLayout {
    double lo = 0.0, hi = 0.0, pivot = 0.0, ref = 0.0;
    // 大きさ size のときの長さ（負にはしない）。
    double length(double size) const {
        const double len = hi - lo + size;
        return len > 0.0 ? len : 0.0;
    }
    // 位置 pos・長さ len のときの始点。
    double start(double pos, double len) const { return ref + pos - len * pivot; }
    // 逆向き: 長さ len（0 以上）になる size と、始点 start・長さ len になる pos。
    double sizeFor(double len) const { return len - (hi - lo); }
    double posFor(double startAt, double len) const { return startAt - ref + len * pivot; }
};
// 親の始点・長さと、アンカーの min / max・ピボットから 1 軸ぶんの解き方を作る。
AxisLayout axisLayout(double parentStart, double parentLength, double anchorMin,
                      double anchorMax, double pivot);
// 親の矩形と性質（Anchor / Pivot）から、軸 axis（0 = x、1 = y）の解き方を作る（アンカーの
// 成分の並び = x の min / max が 0 / 2、y が 1 / 3 の唯一の読み手）。pos / size の成分は
// axis 番目。
AxisLayout axisLayoutOf(const Rect& parent, const PropSet& props, int axis);

// 親の矩形の中で要素の矩形を解く（性質は Anchor / Pivot / Pos / Size を使う）。
Rect solveRect(const Rect& parent, const PropSet& props);

// 解いた要素 1 個。並びは要素の並び（文書順）と同じ。
struct LayoutNode {
    Rect rect;
    int parent = -1;       // 親の要素の添字（-1 = Canvas の直下）
    bool visible = true;   // 自分と祖先が全部 visible
};

// 要素の親（id）を添字に直す。親が見つからない・自分より後ろにある（文書順で
// 親は子より前）・自分自身なら -1（Canvas の直下として扱う）。
std::vector<int> parentIndices(const std::vector<ElementDesc>& elements);

// i 番目の要素の性質: 重ね済みの性質（resolve の結果、要素の並びと同じ順）が
// あればそれ、無ければ（resolved が空・短い）設計値。UI の組み立てはどこも
// これを通して読む。
const PropSet& elementProps(const std::vector<ElementDesc>& elements,
                            const std::vector<PropSet>& resolved, std::size_t i);

// 木を文書順にたどって全要素の矩形と見えるかを解く（性質は elementProps）。
std::vector<LayoutNode> layoutTree(const std::vector<ElementDesc>& elements,
                                   const std::vector<PropSet>& props,
                                   const Rect& canvas);

// 点 (x, y) にある最も手前の要素の添字（無ければ -1）。見えない要素は当たらない。
// accept を渡せば、それが true を返す要素だけを候補にする（ボタンだけ等）。
int hitTest(const std::vector<LayoutNode>& nodes, double x, double y,
            const std::function<bool(int)>& accept = {});

}  // namespace ui
}  // namespace wizengine
