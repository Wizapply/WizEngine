// テレインの編集の記録と取り消し。宣言と単位の約束は TerrainUndo.h。
#include "terrain/TerrainUndo.h"

#include <utility>

namespace wizengine {
namespace terrain {

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
    out.heights.reserve(std::size_t(r.i1 - r.i0 + 1) * std::size_t(r.j1 - r.j0 + 1));
    for (int j = r.j0; j <= r.j1; ++j)
        for (int i = r.i0; i <= r.i1; ++i) out.heights.push_back(field.sample(i, j));
    out.holes.reserve(std::size_t(r.i1 - r.i0) * std::size_t(r.j1 - r.j0));
    for (int j = r.j0; j < r.j1; ++j)
        for (int i = r.i0; i < r.i1; ++i) out.holes.push_back(field.holeAt(i, j) ? 1 : 0);
    return out;
}

TerrainEdit applyEdit(HeightField& field, const TerrainEdit& edit) {
    if (edit.empty() || !field.valid() || edit.resolution != field.resolution()) return {};
    const GridRect& r = edit.rect;
    const std::size_t sampleCount =
        std::size_t(r.i1 - r.i0 + 1) * std::size_t(r.j1 - r.j0 + 1);
    const std::size_t cellCount = std::size_t(r.i1 - r.i0) * std::size_t(r.j1 - r.j0);
    // 記録が矩形と合わない（壊れた記録）なら当てない。
    const int res = field.resolution();
    if (r.i0 < 0 || r.j0 < 0 || r.i1 > res || r.j1 > res ||
        edit.heights.size() != sampleCount || edit.holes.size() != cellCount)
        return {};

    TerrainEdit inverse = captureRegion(field, r);
    std::size_t k = 0;
    for (int j = r.j0; j <= r.j1; ++j)
        for (int i = r.i0; i <= r.i1; ++i) field.setSample(i, j, edit.heights[k++]);
    k = 0;
    for (int j = r.j0; j < r.j1; ++j) {
        for (int i = r.i0; i < r.i1; ++i) {
            const bool hole = edit.holes[k++] != 0;
            // 変わるときだけ書く（穴の無い格子にマスクを確保させない）。
            if (field.holeAt(i, j) != hole) field.setHole(i, j, hole);
        }
    }
    return inverse;
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
