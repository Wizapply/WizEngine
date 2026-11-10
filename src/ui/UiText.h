#pragma once

#include <string>
#include <vector>

#include "ui/FontProvider.h"
#include "ui/UiLayout.h"
#include "ui/UiProps.h"

// 字の並べ方（字送り・カーニング・折り返し・揃え・改行）。種類 text の組み立て
// もここ（kTextKind）。座標は要素の左上が原点の設計上の画素（y 下向き）。
namespace wizengine {
namespace ui {

// 並べ方の指定。字の大きさ・折り返し・揃えは性質の値なので、既定値も
// 性質の表から取る（textStyleOf。値を持つ欄に既定値を書かない）。
struct TextStyle {
    int font = -1;       // FontProvider::fontIndex の番号
    double fontSize;     // 1 em の大きさ（設計上の画素）
    double width = 0.0;  // 折り返す幅（0 以下 = 折り返さない）
    bool wrap;
    TextAlign align;
};

// 性質（Font / FontSize / Wrap / Align）から並べ方を作る。立っていない性質は
// 表の既定値。width は要素の幅。
TextStyle textStyleOf(const PropSet& props, double width, FontProvider& fonts);

struct PlacedGlyph {
    Rect rect;  // 絵の範囲（にじみ幅を含む）
    GlyphInfo glyph;
};

struct TextLayout {
    std::vector<PlacedGlyph> glyphs;  // 絵のある字だけ（空白は入らない）
    double width = 0.0;               // いちばん長い行の幅（行末の空白は除く）
    double height = 0.0;              // 行数 × 行の高さ
    int lines = 0;
};

// 折り返しの規則: 空白の後と、漢字・かななど（U+2E80 以上）の前後で折り返せる。
// 1 語が幅に収まらなければ字の境で切る。行末の空白は幅にも揃えにも数えず、
// 折り返した行の頭の空白は捨てる。"\n" で改行、"\r" は無視。
TextLayout layoutText(const std::string& utf8, const TextStyle& style, FontProvider& fonts);

}  // namespace ui
}  // namespace wizengine
