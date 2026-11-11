#include "ui/FontAtlas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

// stb の実装はこの翻訳単位だけに置く（ほかで実装マクロを立てると重複定義）。
#define STB_RECT_PACK_IMPLEMENTATION
#include <stb_rect_pack.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace wizengine {
namespace ui {
namespace {

// 字と字の間の隙間（画素）。隣の字の端がバイリニアで混ざらないように。
constexpr int kGlyphGap = 1;
// 輪郭から 1 画素離れるごとの値の変化（にじみ幅の外で 0 になる）。
constexpr float kSdfDistScale = float(kSdfOnEdge) / float(kSdfSpreadPx);

// 豆腐（字が無いときの四角）の寸法（em に対する割合）。
constexpr float kTofuWidthEm = 0.55f;
constexpr float kTofuHeightEm = 0.7f;
constexpr float kTofuLeftEm = 0.05f;
constexpr float kTofuAdvanceEm = 0.65f;
constexpr float kTofuStrokeEm = 0.06f;

// フォントが 1 つも無いときの行の寸法（em に対する割合。豆腐が並べられるように）。
constexpr float kFallbackAscentEm = 0.8f;
constexpr float kFallbackDescentEm = -0.2f;

std::uint64_t keyOf(int font, char32_t cp) {
    return (std::uint64_t(std::uint32_t(font)) << 32) | std::uint64_t(cp);
}

}  // namespace

struct FontAtlas::Font {
    std::string name;
    std::vector<std::uint8_t> bytes;
    stbtt_fontinfo info{};
    float scale = 1.0f;  // フォント単位 → 基準の画素（1 em = kSdfBasePx）
    FontMetrics metrics;
};

struct FontAtlas::Record {
    std::vector<std::uint8_t> bitmap;
    int bw = 0, bh = 0;
    float xoff = 0.0f, yoff = 0.0f, advance = 0.0f;
    int x = 0, y = 0;
    bool placed = false;
    bool used = true;
    bool tofu = false;   // どのフォントにも字が無い
    bool empty = false;  // 絵の無い字（空白）
};

struct FontAtlas::Packer {
    stbrp_context context{};
    std::vector<stbrp_node> nodes;
};

FontAtlas::FontAtlas(int size)
    : size_(size), packer_(std::make_unique<Packer>()) {
    pages_.emplace_back();
    resetPage();
}

FontAtlas::~FontAtlas() = default;

int FontAtlas::addFont(const std::string& name, std::vector<std::uint8_t> bytes) {
    auto f = std::make_unique<Font>();
    f->name = name;
    f->bytes = std::move(bytes);
    if (f->bytes.empty()) return -1;
    const int offset = stbtt_GetFontOffsetForIndex(f->bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, f->bytes.data(), offset)) return -1;
    f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, kSdfBasePx);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &lineGap);
    f->metrics = {ascent * f->scale, descent * f->scale, lineGap * f->scale};
    fonts_.push_back(std::move(f));
    // 新しいフォントで見つかる字があるかもしれないので、豆腐の記録は捨てる。
    for (auto it = records_.begin(); it != records_.end();) {
        it = it->second.tofu ? records_.erase(it) : std::next(it);
    }
    return int(fonts_.size()) - 1;
}

int FontAtlas::fontIndex(const std::string& name) const {
    if (fonts_.empty()) return -1;
    const auto alias = names_.find(name);
    if (alias != names_.end() && alias->second >= 0 && alias->second < int(fonts_.size())) {
        return alias->second;
    }
    for (std::size_t k = 0; k < fonts_.size(); ++k) {
        if (fonts_[k]->name == name) return int(k);
    }
    return 0;
}

GlyphInfo FontAtlas::glyph(int font, char32_t codepoint) {
    const std::uint64_t key = keyOf(font, codepoint);
    const auto found = records_.find(key);
    if (found != records_.end()) {
        found->second.used = true;
        return infoOf(found->second);
    }
    Record r;
    // 頼まれたフォント → 足した順の残りのフォント、の順で字を探す。
    const Font* owner = nullptr;
    int glyphIndex = 0;
    const bool validFont = font >= 0 && font < int(fonts_.size());
    for (int k = -1; k < int(fonts_.size()) && !owner; ++k) {
        const int f = k < 0 ? (validFont ? font : -1) : k;
        if (f < 0) continue;
        const int g = stbtt_FindGlyphIndex(&fonts_[std::size_t(f)]->info, int(codepoint));
        if (g != 0) {
            owner = fonts_[std::size_t(f)].get();
            glyphIndex = g;
        }
    }
    if (!owner) {
        r.tofu = true;
    } else {
        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&owner->info, glyphIndex, &advance, &lsb);
        r.advance = advance * owner->scale;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* sdf =
            stbtt_GetGlyphSDF(&owner->info, owner->scale, glyphIndex, kSdfSpreadPx,
                              static_cast<unsigned char>(kSdfOnEdge), kSdfDistScale, &w,
                              &h, &xoff, &yoff);
        if (!sdf) {
            r.empty = true;
        } else {
            r.bitmap.assign(sdf, sdf + std::size_t(w) * std::size_t(h));
            stbtt_FreeSDF(sdf, nullptr);
            r.bw = w;
            r.bh = h;
            r.xoff = float(xoff);
            r.yoff = float(yoff);
            if (!place(r)) overflow_ = true;
        }
    }
    const auto inserted = records_.emplace(key, std::move(r)).first;
    return infoOf(inserted->second);
}

float FontAtlas::kerning(int font, char32_t a, char32_t b) const {
    if (font < 0 || font >= int(fonts_.size())) return 0.0f;
    const Font& f = *fonts_[std::size_t(font)];
    return stbtt_GetCodepointKernAdvance(&f.info, int(a), int(b)) * f.scale;
}

FontMetrics FontAtlas::metrics(int font) const {
    if (font < 0 || font >= int(fonts_.size())) {
        return {kFallbackAscentEm * kSdfBasePx, kFallbackDescentEm * kSdfBasePx, 0.0f};
    }
    return fonts_[std::size_t(font)]->metrics;
}

void FontAtlas::beginUse() {
    for (auto& entry : records_) entry.second.used = false;
}

AtlasRebuildResult FontAtlas::rebuild() {
    // 使っていない字は捨て、残りを一度に詰め直す（一度に渡すほうがよく詰まる）。
    for (auto it = records_.begin(); it != records_.end();) {
        it = it->second.used ? std::next(it) : records_.erase(it);
    }
    resetPage();
    std::vector<Record*> order;
    std::vector<stbrp_rect> rects;
    for (auto& entry : records_) {
        Record& r = entry.second;
        r.placed = false;
        if (r.tofu || r.empty) continue;
        stbrp_rect rr{};
        rr.id = int(order.size());
        rr.w = r.bw + kGlyphGap;
        rr.h = r.bh + kGlyphGap;
        rects.push_back(rr);
        order.push_back(&r);
    }
    AtlasRebuildResult result;
    if (!rects.empty()) stbrp_pack_rects(&packer_->context, rects.data(), int(rects.size()));
    for (const auto& rr : rects) {
        Record& r = *order[std::size_t(rr.id)];
        if (rr.was_packed) {
            r.x = rr.x;
            r.y = rr.y;
            r.placed = true;
            blit(r);
            ++result.placed;
        } else {
            ++result.missing;
        }
    }
    overflow_ = false;
    ++generation_;
    return result;
}

bool FontAtlas::takeDirtyRect(AtlasRect& out) {
    if (!dirty_) return false;
    out = dirtyRect_;
    dirty_ = false;
    return true;
}

bool FontAtlas::place(Record& r) {
    stbrp_rect rr{};
    rr.w = r.bw + kGlyphGap;
    rr.h = r.bh + kGlyphGap;
    stbrp_pack_rects(&packer_->context, &rr, 1);
    if (!rr.was_packed) return false;
    r.x = rr.x;
    r.y = rr.y;
    r.placed = true;
    blit(r);
    return true;
}

void FontAtlas::blit(const Record& r) {
    std::vector<std::uint8_t>& page = pages_[0];
    for (int row = 0; row < r.bh; ++row) {
        std::memcpy(&page[std::size_t(r.y + row) * std::size_t(size_) + std::size_t(r.x)],
                    &r.bitmap[std::size_t(row) * std::size_t(r.bw)], std::size_t(r.bw));
    }
    markDirty(r.x, r.y, r.bw, r.bh);
}

void FontAtlas::markDirty(int x, int y, int w, int h) {
    if (!dirty_) {
        dirtyRect_ = {0, x, y, w, h};
        dirty_ = true;
        return;
    }
    const int x0 = std::min(dirtyRect_.x, x);
    const int y0 = std::min(dirtyRect_.y, y);
    const int x1 = std::max(dirtyRect_.x + dirtyRect_.w, x + w);
    const int y1 = std::max(dirtyRect_.y + dirtyRect_.h, y + h);
    dirtyRect_ = {0, x0, y0, x1 - x0, y1 - y0};
}

void FontAtlas::resetPage() {
    pages_[0].assign(std::size_t(size_) * std::size_t(size_), 0);
    packer_->nodes.assign(std::size_t(size_), stbrp_node{});
    stbrp_init_target(&packer_->context, size_, size_, packer_->nodes.data(),
                      int(packer_->nodes.size()));
    // 豆腐は必ず置き場にある（溢れても豆腐で描けるように、いちばん先に置く）。
    tofu_ = std::make_unique<Record>(makeTofu());
    place(*tofu_);
    // 中身を全部消したので、ページ全体を送り直す。
    dirtyRect_ = {0, 0, 0, size_, size_};
    dirty_ = true;
}

FontAtlas::Record FontAtlas::makeTofu() const {
    // 中が空いた四角の輪郭を SDF で描く（内側が大きい値 = 字と同じ向き）。
    Record r;
    const float em = kSdfBasePx;
    const float boxW = kTofuWidthEm * em, boxH = kTofuHeightEm * em;
    const float stroke = kTofuStrokeEm * em;
    r.bw = int(std::ceil(boxW)) + 2 * kSdfSpreadPx;
    r.bh = int(std::ceil(boxH)) + 2 * kSdfSpreadPx;
    r.bitmap.resize(std::size_t(r.bw) * std::size_t(r.bh));
    const float cx = r.bw * 0.5f, cy = r.bh * 0.5f;
    for (int y = 0; y < r.bh; ++y) {
        for (int x = 0; x < r.bw; ++x) {
            // 四角の符号付き距離（内側が負）→ 線の太さぶんの帯への距離。
            const float qx = std::fabs(x + 0.5f - cx) - boxW * 0.5f;
            const float qy = std::fabs(y + 0.5f - cy) - boxH * 0.5f;
            const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
            const float box = outside + std::min(std::max(qx, qy), 0.0f);
            const float ring = std::fabs(box + stroke * 0.5f) - stroke * 0.5f;
            const float v = float(kSdfOnEdge) - ring * kSdfDistScale;
            r.bitmap[std::size_t(y) * std::size_t(r.bw) + std::size_t(x)] =
                static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
        }
    }
    r.xoff = kTofuLeftEm * em - float(kSdfSpreadPx);
    r.yoff = -boxH - float(kSdfSpreadPx);
    r.advance = kTofuAdvanceEm * em;
    return r;
}

GlyphInfo FontAtlas::infoOf(const Record& r) const {
    GlyphInfo g;
    if (r.empty) {
        g.empty = true;
        g.advance = r.advance;
        return g;
    }
    const bool useTofu = r.tofu || !r.placed;
    const Record& src = useTofu ? *tofu_ : r;
    const float inv = 1.0f / float(size_);
    g.page = 0;
    g.u0 = src.x * inv;
    g.v0 = src.y * inv;
    g.u1 = (src.x + src.bw) * inv;
    g.v1 = (src.y + src.bh) * inv;
    g.xoff = src.xoff;
    g.yoff = src.yoff;
    g.w = float(src.bw);
    g.h = float(src.bh);
    g.advance = src.advance;
    g.tofu = useTofu;
    return g;
}

}  // namespace ui
}  // namespace wizengine
