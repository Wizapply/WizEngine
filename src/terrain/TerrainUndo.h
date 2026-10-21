#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "terrain/HeightField.h"

// テレインの編集の記録と取り消し（コマンド）。
//
// 1 回の編集（筆の 1 ストローク・ノイズ生成など）の前に、書き換える範囲の今の
// 値を captureRegion で写しておき、取り消すときは applyEdit で書き戻す。
// applyEdit は書き戻す直前の値を逆操作として返すので、Redo を足すときは
// それを別のスタックに積むだけでよい（いまは Redo を作らない）。
// Chrono も Filament も知らない純粋な層（tests/terrain が単体で回す）。
namespace wizengine {
namespace terrain {

// 書き換えた矩形とその範囲の値。
//
// 単位の約束: rect は**セル単位**（[i0, i1) x [j0, j1)、0〜resolution に
// 切り詰めたもの）の 1 つだけを持ち、高さと穴の両方をそこから導く。
//   * 穴 = rect のセルそのもの（(i1 - i0) x (j1 - j0) 個）。
//   * 高さ = rect のセルの角の格子点（[i0, i1] x [j0, j1] = 1 辺 +1 個）。
// サンプル 1 つを書き換えると周りの最大 4 セルの見た目が変わり、セルの角は
// 必ずそのセルの範囲の格子点に入るので、「セルの矩形 1 つ」で両方が漏れなく
// 包める（矩形を 2 つ持つと、片方だけ広げ忘れる食い違いが起きうる）。
// サンプル (k) を書き換えた編集は、そのサンプルを角に持つセル（k - 1 と k、
// 切り詰め済み）を rect に入れる。
struct TerrainEdit {
    int resolution = 0;            // 記録した格子の resolution（違う格子へは当てない）
    GridRect rect;                 // セル単位。空 = 何もしない編集
    std::vector<float> heights;    // 行優先、1 行 (i1 - i0 + 1) 個
    std::vector<uint8_t> holes;    // 行優先、1 行 (i1 - i0) 個。1 = 穴

    bool empty() const { return rect.empty(); }
    // 記録に使っているおおよそのメモリ（バイト）。上限の判定に使う。
    std::size_t bytes() const;
};

// field の rect（セル単位。範囲外は切り詰める）の今の値を写す。空の格子・
// 切り詰めて空になった矩形は空の編集を返す。
TerrainEdit captureRegion(const HeightField& field, const GridRect& rect);

// edit の値を field へ書き戻し、書き戻す直前の値を逆操作として返す。
// resolution が違う格子・空の編集には何もせず空の編集を返す。
// 共有中の格子（shared_ptr<const>）には呼べない = コピーに対して呼ぶ。
TerrainEdit applyEdit(HeightField& field, const TerrainEdit& edit);

constexpr std::size_t kUndoMaxEntries = 32;
constexpr std::size_t kUndoMaxBytes = std::size_t(64) * 1024 * 1024;

// 取り消しの記録の積み場。上限（段数・バイト数）を超えたら古いものから
// 捨てる。ただし積んだばかりの 1 段だけは、単独で上限を超えていても残す
// （大きな格子のノイズ生成も 1 段は戻せるように）。
class TerrainUndoStack {
public:
    // 上限は既定で 32 段 / 64 MB（テストは小さな値を渡す）。
    explicit TerrainUndoStack(std::size_t maxEntries = kUndoMaxEntries,
                              std::size_t maxBytes = kUndoMaxBytes);

    // 空の編集は積まない。
    void push(TerrainEdit edit);
    // 最後に積んだものを取り出す。空なら空の編集。
    TerrainEdit pop();
    void clear();
    bool empty() const { return edits_.empty(); }
    std::size_t size() const { return edits_.size(); }
    std::size_t bytes() const { return bytes_; }

private:
    std::size_t maxEntries_;
    std::size_t maxBytes_;
    std::deque<TerrainEdit> edits_;  // 後ろが新しい
    std::size_t bytes_ = 0;
};

}  // namespace terrain
}  // namespace wizengine
