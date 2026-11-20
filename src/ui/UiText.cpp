#include "ui/UiText.h"

#include <algorithm>
#include <cstddef>

#include "core/Utf8.h"
#include "ui/UiKinds.h"
#include "ui/UiMesh.h"
#include "ui/UiValue.h"

namespace wizengine {
namespace ui {
namespace {

// 漢字・かな・全角記号など、字の前後で折り返してよい字の始まり（CJK 部首補助以降）。
constexpr char32_t kCjkStart = 0x2E80;
constexpr char32_t kIdeographicSpace = 0x3000;

bool isSpace(char32_t cp) { return cp == U' ' || cp == U'\t' || cp == kIdeographicSpace; }
bool isCjk(char32_t cp) { return cp >= kCjkStart && cp != kIdeographicSpace; }

struct Item {
    char32_t cp;
    GlyphInfo glyph;
    float kern = 0.0f;  // 同じ行の前の字とのカーニング（行の頭は 0。引くのは 1 組 1 回）
};
using Line = std::vector<Item>;

// 行の幅（基準の画素）。trimTrailing で行末の空白を除く。
float lineWidth(const Line& line, bool trimTrailing) {
    std::size_t end = line.size();
    while (trimTrailing && end > 0 && isSpace(line[end - 1].cp)) --end;
    float w = 0.0f;
    for (std::size_t k = 0; k < end; ++k) w += line[k].kern + line[k].glyph.advance;
    return w;
}

// 字 1 個。空白として扱う字（タブなど）で、フォントに字が無い・字送りが 0 のものは、
// 豆腐ではなく半角の空白と同じ字送りの空の字にする（空白は絵を持たない。半角の空白
// そのものも同じ = フォントが無いときも豆腐にしない）。
Item makeItem(char32_t cp, int font, FontProvider& fonts) {
    Item item{cp, fonts.glyph(font, cp)};
    const bool unusableSpace = item.glyph.tofu || !(item.glyph.advance > 0.0f);
    if (isSpace(cp) && unusableSpace) {
        const GlyphInfo space = fonts.glyph(font, U' ');
        item.glyph = GlyphInfo{};
        item.glyph.advance = space.advance;
        item.glyph.empty = true;
    }
    return item;
}

// 字を行に分ける（基準の画素で測る。limit は折り返す幅、0 以下 = 折り返さない）。
std::vector<Line> breakLines(const std::string& utf8, int font, float limit,
                             FontProvider& fonts) {
    std::vector<Line> lines(1);
    float width = 0.0f;     // 今の行の幅
    std::size_t breakAt = 0;  // 今の行で次の行を始められる位置（0 = まだ無い）
    for (const char32_t cp : decodeUtf8(utf8)) {
        if (cp == U'\r') continue;
        if (cp == U'\n') {
            lines.emplace_back();
            width = 0.0f;
            breakAt = 0;
            continue;
        }
        Item item = makeItem(cp, font, fonts);
        const bool space = isSpace(cp);
        const bool cjk = isCjk(cp);
        if (cjk) breakAt = lines.back().size();  // 漢字の前で折り返せる
        // 前の字とのカーニング（この 1 回だけ引き、字に持たせる）。
        item.kern = lines.back().empty() ? 0.0f
                                         : fonts.kerning(font, lines.back().back().cp, cp);
        const float next = width + item.kern + item.glyph.advance;
        // 折り返すのは: 折り返しが有効・空白でない（空白ははみ出してよい）・
        // 行に字がある・幅を超える、のすべて。
        const bool wrapping = limit > 0.0f;
        const bool overflows = next > limit;
        const bool lineHasGlyphs = !lines.back().empty();
        if (wrapping && !space && lineHasGlyphs && overflows) {
            Line& cur = lines.back();
            const std::size_t at = breakAt > 0 ? breakAt : cur.size();
            Line rest(cur.begin() + std::ptrdiff_t(at), cur.end());
            cur.resize(at);
            // 折り返した行の頭の空白は捨てる。
            const auto firstInk = std::find_if(rest.begin(), rest.end(),
                                               [](const Item& i) { return !isSpace(i.cp); });
            rest.erase(rest.begin(), firstInk);
            if (!rest.empty()) rest.front().kern = 0.0f;  // 行の頭になった
            lines.push_back(std::move(rest));
            width = lineWidth(lines.back(), false);
            breakAt = 0;
            // 送った語の最後の字は前の行の最後の字と同じなので、カーニングはそのまま。
            // 送った語が空なら今の字が行の頭。
            if (lines.back().empty()) item.kern = 0.0f;
            // 送った語（幅に収まっている）に今の字を足すと超えるなら、送った語を
            // 1 行にして今の字から次の行を始める（字の境での切断と同じ）。
            const bool restOverflows = !lines.back().empty() &&
                                       width + item.kern + item.glyph.advance > limit;
            if (restOverflows) {
                lines.emplace_back();
                width = 0.0f;
                item.kern = 0.0f;
            }
            lines.back().push_back(item);
            width += item.kern + item.glyph.advance;
        } else {
            lines.back().push_back(item);
            width = next;
        }
        if (space || cjk) breakAt = lines.back().size();  // 空白・漢字の後で折り返せる
    }
    return lines;
}

}  // namespace

TextLayout layoutText(const std::string& utf8, const TextStyle& style, FontProvider& fonts) {
    TextLayout out;
    const float base = fonts.basePx();
    if (!(base > 0.0f) || !(style.fontSize > 0.0)) return out;
    const double scale = style.fontSize / base;
    const bool wrapping = style.wrap && style.width > 0.0;
    const float limit = wrapping ? float(style.width / scale) : 0.0f;
    const std::vector<Line> lines = breakLines(utf8, style.font, limit, fonts);

    const FontMetrics m = fonts.metrics(style.font);
    const double lineHeight = (m.ascent - m.descent + m.lineGap) * scale;
    std::vector<double> widths;
    for (const Line& line : lines) {
        widths.push_back(lineWidth(line, true) * scale);
        out.width = std::max(out.width, widths.back());
    }
    // 揃えの基準の幅: 要素の幅（無ければいちばん長い行）。
    const double box = style.width > 0.0 ? style.width : out.width;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Line& line = lines[i];
        double pen = 0.0;
        if (style.align == TextAlign::Center) pen = (box - widths[i]) * 0.5;
        if (style.align == TextAlign::Right) pen = box - widths[i];
        const double baseline = double(i) * lineHeight + m.ascent * scale;
        for (std::size_t k = 0; k < line.size(); ++k) {
            pen += line[k].kern * scale;
            const GlyphInfo& g = line[k].glyph;
            if (!g.empty) {
                PlacedGlyph p;
                p.rect = {pen + g.xoff * scale, baseline + g.yoff * scale, g.w * scale,
                          g.h * scale};
                p.glyph = g;
                out.glyphs.push_back(p);
            }
            pen += g.advance * scale;
        }
    }
    out.lines = int(lines.size());
    out.height = double(lines.size()) * lineHeight;
    return out;
}

TextStyle textStyleOf(const PropSet& props, double width, FontProvider& fonts) {
    TextStyle style;
    style.font = fonts.fontIndex(props.text(Prop::Font));
    style.fontSize = props.number(Prop::FontSize);
    style.width = width;
    style.wrap = props.flag(Prop::Wrap);
    style.align = TextAlign(enumIndex(props.number(Prop::Align), kAlignNames, 0));
    return style;
}

void addTextQuads(const ResolvedElement& e, Prop colorProp, bool centerVertically,
                  FontProvider& fonts, MeshSink& sink) {
    const TextStyle style = textStyleOf(e.props, e.rect.w, fonts);
    const TextLayout layout = layoutText(e.props.text(Prop::Text), style, fonts);
    // 縦に中央へ寄せるときは、行の塊を矩形の高さの真ん中に置く（はみ出すときも中央）。
    const double top = e.rect.y + (centerVertically ? (e.rect.h - layout.height) * 0.5 : 0.0);
    UiQuad q;
    elementColor(e, colorProp, q.color);
    q.materialId = kUiMaterialSdf;
    // 縁取りの幅（em に対する割合）→ にじみ幅に対する割合（頂点の outline）と色。
    q.outline = float(std::clamp(e.props.number(Prop::Outline) / kSdfOutlineMaxEm, 0.0, 1.0));
    elementColor(e, Prop::OutlineColor, q.outlineColor);
    for (const PlacedGlyph& p : layout.glyphs) {
        q.rect = {e.rect.x + p.rect.x, top + p.rect.y, p.rect.w, p.rect.h};
        q.u0 = p.glyph.u0;
        q.v0 = p.glyph.v0;
        q.u1 = p.glyph.u1;
        q.v1 = p.glyph.v1;
        q.textureId = sink.fontPageTexture(p.glyph.page);
        sink.quad(q, e.rect);
    }
}

namespace {

void buildText(const ResolvedElement& e, FontProvider& fonts, MeshSink& sink) {
    addTextQuads(e, Prop::Color, false, fonts, sink);
}

}  // namespace

const ElementKindInfo kTextKind = {"text", "T", kKindText, Interaction::None, &buildText};

}  // namespace ui
}  // namespace wizengine
