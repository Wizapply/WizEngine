#pragma once

#include <string>

// 字を並べる側（UiText）が字の置き場（FontAtlas）に聞くことの口。字送り・
// 折り返しの検査は本物のフォント無しで回したいので（tests/ui の固定幅の偽物）、
// 置き場の実体と並べ方をこの口で切り離す。値の単位は「基準の字の大きさ」
// （basePx）の画素で、使う側が fontSize / basePx を掛ける。
namespace wizengine {
namespace ui {

// 字 1 個の置き場と寸法。
struct GlyphInfo {
    int page = 0;                                 // 置き場のページ（今は 0 だけ）
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;  // ページ内の位置（0..1、v は下向き）
    float xoff = 0.0f, yoff = 0.0f;               // 字の左上（ペンの位置・ベースラインから。y 下向き）
    float w = 0.0f, h = 0.0f;                     // 絵の大きさ（にじみ幅を含む）
    float advance = 0.0f;                         // 字送り
    bool empty = false;                           // 絵が無い（空白）
    bool tofu = false;                            // 字が無いので豆腐で代わりに描く
};

// フォント全体の寸法（ascent は上向き正、descent は下向き負 = フォントの流儀）。
struct FontMetrics {
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineGap = 0.0f;
};

class FontProvider {
public:
    virtual ~FontProvider() = default;
    // 名前 → フォントの番号。空・知らない名前は既定のフォント（最初のもの）、
    // フォントが 1 つも無ければ -1（字は全部豆腐になる）。
    virtual int fontIndex(const std::string& name) const = 0;
    // 字の置き場。初めての字はここで置き場に入れる（だから const ではない）。
    virtual GlyphInfo glyph(int font, char32_t codepoint) = 0;
    // a の次に b を置くときの字送りの補正（無ければ 0）。
    virtual float kerning(int font, char32_t a, char32_t b) const = 0;
    virtual FontMetrics metrics(int font) const = 0;
    // 値の単位になる基準の字の大きさ（画素、1 em）。
    virtual float basePx() const = 0;
};

}  // namespace ui
}  // namespace wizengine
