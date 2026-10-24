// テレインの編集の記録と取り消し。宣言と単位の約束は TerrainUndo.h。
#include "terrain/TerrainUndo.h"

#include <utility>

namespace wizengine {
namespace terrain {
namespace {

// TerrainEdit の並び（TerrainUndo.h の約束）: 高さは 1 行 (i1 - i0 + 1) 個、
// 穴は 1 行 (i1 - i0) 個の行優先。数と添字はここだけで出す。
std::size_t heightCount(const GridRect& r) {
    return std::size_t(r.i1 - r.i0 + 1) * std::size_t(r.j1 - r.j0 + 1);
}
std::size_t holeCount(const GridRect& r) {
    return std::size_t(r.i1 - r.i0) * std::size_t(r.j1 - r.j0);
}
std::size_t heightIndex(const GridRect& r, int i, int j) {
    return std::size_t(j - r.j0) * std::size_t(r.i1 - r.i0 + 1) + std::size_t(i - r.i0);
}
std::size_t holeIndex(const GridRect& r, int i, int j) {
    return std::size_t(j - r.j0) * std::size_t(r.i1 - r.i0) + std::size_t(i - r.i0);
}

// 記録が自分の矩形と合っているか（矩形が 0〜resolution の中、値の数が
// 矩形どおり）。壊れた記録を当てない・混ぜないための検査。
bool wellFormed(const TerrainEdit& e) {
    const GridRect& r = e.rect;
    if (r.empty() || r.i0 < 0 || r.j0 < 0 || r.i1 > e.resolution || r.j1 > e.resolution)
        return false;
    return e.heights.size() == heightCount(r) && e.holes.size() == holeCount(r);
}

}  // namespace

std::size_t TerrainEdit::bytes() const {
    return sizeof(TerrainEdit) + heights.size() * sizeof(float) + holes.size();
}

TerrainEdit captureRegion(const HeightField& field, const GridRect& rect) {
    TerrainEdit out;
    if (!field.valid()) return out;
    const GridRect r = rect.clamped(field.resolution());
    if (r.empty()) return out;
    out.resolution = field.resolution();
    out.rect = r;
    // 高さはセルの角 = 1 辺 +1 個（r は resolution 以下なので i1 / j1 も格子点）。
    out.heights.resize(heightCount(r));
    for (int j = r.j0; j <= r.j1; ++j)
        for (int i = r.i0; i <= r.i1; ++i)
            out.heights[heightIndex(r, i, j)] = field.sample(i, j);
    out.holes.resize(holeCount(r));
    for (int j = r.j0; j < r.j1; ++j)
        for (int i = r.i0; i < r.i1; ++i)
            out.holes[holeIndex(r, i, j)] = field.holeAt(i, j) ? 1 : 0;
    return out;
}

TerrainEdit applyEdit(HeightField& field, const TerrainEdit& edit) {
    if (edit.empty() || !field.valid() || edit.resolution != field.resolution()) return {};
    // 記録が矩形と合わない（壊れた記録）なら当てない。
    if (!wellFormed(edit)) return {};
    const GridRect& r = edit.rect;

    TerrainEdit inverse = captureRegion(field, r);
    for (int j = r.j0; j <= r.j1; ++j)
        for (int i = r.i0; i <= r.i1; ++i)
            field.setSample(i, j, edit.heights[heightIndex(r, i, j)]);
    for (int j = r.j0; j < r.j1; ++j) {
        for (int i = r.i0; i < r.i1; ++i) {
            const bool hole = edit.holes[holeIndex(r, i, j)] != 0;
            // 変わるときだけ書く（穴の無い格子にマスクを確保させない）。
            if (field.holeAt(i, j) != hole) field.setHole(i, j, hole);
        }
    }
    return inverse;
}

TerrainEdit growEdit(const HeightField& before, const TerrainEdit& acc, const GridRect& rect) {
    if (acc.empty()) return captureRegion(before, rect);
    // 別の格子の記録・壊れた記録は混ぜない（acc の「変更前」を失わない）。
    if (!before.valid() || acc.resolution != before.resolution() || !wellFormed(acc)) return acc;
    const GridRect& a = acc.rect;
    // 今回の範囲が空か acc の矩形に収まるなら、広げる所が無い = 写さない。
    const GridRect grown = a.united(rect.clamped(before.resolution()));
    if (grown.i0 == a.i0 && grown.j0 == a.j0 && grown.i1 == a.i1 && grown.j1 == a.j1)
        return acc;
    // acc.rect は 0〜resolution の中（wellFormed）なので、切り詰めても必ず包む。
    TerrainEdit out = captureRegion(before, grown);
    const GridRect& f = out.rect;
    // acc の矩形の中は acc の値で上書きする（高さはセルの角 = 1 辺 +1 個）。
    for (int j = a.j0; j <= a.j1; ++j)
        for (int i = a.i0; i <= a.i1; ++i)
            out.heights[heightIndex(f, i, j)] = acc.heights[heightIndex(a, i, j)];
    for (int j = a.j0; j < a.j1; ++j)
        for (int i = a.i0; i < a.i1; ++i)
            out.holes[holeIndex(f, i, j)] = acc.holes[holeIndex(a, i, j)];
    return out;
}

TerrainUndoStack::TerrainUndoStack(std::size_t maxEntries, std::size_t maxBytes)
    : maxEntries_(maxEntries), maxBytes_(maxBytes) {}

void TerrainUndoStack::push(TerrainEdit edit) {
    if (edit.empty()) return;
    bytes_ += edit.bytes();
    edits_.push_back(std::move(edit));
    // 古いものから捨てる。積んだばかりの 1 段は残す（ヘッダの注意）。
    while (edits_.size() > 1 && (edits_.size() > maxEntries_ || bytes_ > maxBytes_)) {
        bytes_ -= edits_.front().bytes();
        edits_.pop_front();
    }
}

TerrainEdit TerrainUndoStack::pop() {
    if (edits_.empty()) return {};
    TerrainEdit out = std::move(edits_.back());
    edits_.pop_back();
    bytes_ -= out.bytes();
    return out;
}

void TerrainUndoStack::clear() {
    edits_.clear();
    bytes_ = 0;
}

}  // namespace terrain
}  // namespace wizengine
