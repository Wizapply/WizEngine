#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui/FontProvider.h"
#include "ui/UiSdfParams.h"  // kSdfBasePx

// 字の置き場（SDF のアトラス）。字を「輪郭までの距離」の 1 チャンネル画像
// （R8）にして、stb_rect_pack で 1 枚に詰める。字は stb_truetype で白黒の絵として塗り、
// 距離は ui/UiSdf の自前の計算で出す（stb の距離の関数は 3 次の曲線 = CFF の字を扱えない）。
//
// 使い方（1 フレームぶん）:
//   atlas.beginUse();             // この組み立てで使う字の印を消す
//   ...layoutText / buildCanvasMesh が glyph() を呼ぶ...
//   if (atlas.needsRebuild()) {   // 置き場が溢れた
//       const auto r = atlas.rebuild();   // 使っている字だけで組み直す
//       if (r.missing > 0) ...警告...     // それでも入らない字は豆腐
//       ...メッシュを組み直す...        // 字の位置が変わったので
//   }
//   while (atlas.takeDirtyRect(rect)) ...その矩形をテクスチャへ送る...
//
// フォントのバイト列は呼び側から受ける（ファイルを読まない）。ログも出さない
// （純粋な層。溢れは結果で知らせる）。Chrono / Filament / Scene は知らない。
namespace wizengine {
namespace ui {

// SDF の作り方（基準の字の大きさ・にじみ幅・輪郭の値・距離の求め方）は UiSdf.h。
// 置き場の 1 辺（画素）。
inline constexpr int kFontAtlasSize = 2048;

// 送る必要のある範囲（画素）。
struct AtlasRect {
    int page = 0;
    int x = 0, y = 0, w = 0, h = 0;
};

// 組み直しの結果。
struct AtlasRebuildResult {
    int placed = 0;   // 置き場に入った字の数（豆腐を除く）
    int missing = 0;  // 入りきらずに豆腐になった字の数
};

class FontAtlas : public FontProvider {
public:
    explicit FontAtlas(int size = kFontAtlasSize);
    ~FontAtlas() override;
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    // フォントを足す（TrueType / OpenType のバイト列）。読めなければ -1。
    // 字が無いフォントは、足した順に後ろのフォントで探す（日本語の補い）。
    int addFont(const std::string& name, std::vector<std::uint8_t> bytes);
    int fontCount() const { return int(fonts_.size()); }

    // 名前 → フォントの番号の対応（文書の <font name> の名前）。fontIndex は
    // まずこの対応を見て、無ければ addFont の名前、それも無ければ既定（0）。
    // 呼ぶたびに全部入れ替える（同じファイルを別の名前で宣言しても 1 回だけ
    // 読み、名前は別名として付ける）。
    void setFontNames(std::unordered_map<std::string, int> names) { names_ = std::move(names); }

    // FontProvider
    int fontIndex(const std::string& name) const override;
    GlyphInfo glyph(int font, char32_t codepoint) override;
    float kerning(int font, char32_t a, char32_t b) const override;
    FontMetrics metrics(int font) const override;
    float basePx() const override { return kSdfBasePx; }

    // この組み立てで使う字の印を消す（組み直しで残す字を決めるため）。
    void beginUse();
    // 置き場が溢れて豆腐で代わった字がある。
    bool needsRebuild() const { return overflow_; }
    // 使っている字（beginUse 以降に glyph で引いた字）だけで詰め直す。
    // 字の位置が変わるので、呼び側はメッシュを組み直す。
    AtlasRebuildResult rebuild();
    // 組み直しの回数（位置の変わった世代。メッシュの作り直しの目印）。
    int generation() const { return generation_; }

    // 前に取ってから書き換わった範囲（無ければ false）。
    bool takeDirtyRect(AtlasRect& out);
    int size() const { return size_; }
    int pageCount() const { return int(pages_.size()); }
    const std::vector<std::uint8_t>& pixels(int page) const { return pages_[std::size_t(page)]; }

private:
    struct Font;
    struct Record;
    struct Packer;

    bool place(Record& r);
    void blit(const Record& r);
    void markDirty(int x, int y, int w, int h);
    void resetPage();
    Record makeTofu() const;
    GlyphInfo infoOf(const Record& r) const;

    int size_;
    std::vector<std::unique_ptr<Font>> fonts_;
    std::unordered_map<std::string, int> names_;  // setFontNames
    std::vector<std::vector<std::uint8_t>> pages_;
    std::unique_ptr<Packer> packer_;
    std::unordered_map<std::uint64_t, Record> records_;
    std::unique_ptr<Record> tofu_;
    bool overflow_ = false;
    int generation_ = 0;
    bool dirty_ = false;
    AtlasRect dirtyRect_;
};

}  // namespace ui
}  // namespace wizengine
