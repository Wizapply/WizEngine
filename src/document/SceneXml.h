#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// 依存の無い最小の XML DOM（読み書き）。
//
// なぜ自前か: シーン文書の保存形式を MuJoCo(MJCF) 風の XML にするために要る
// のは「要素・属性・入れ子」だけで、名前空間も DTD も実体宣言も使わない。
// 外部依存の追加は最小限に、という既存の方針（CLAUDE.md）に合わせて、その
// 部分集合だけをここに置く。テキストノードは持たない（MJCF 同様、値は全部
// 属性に入る）。
//
// 使う側の約束:
//   * 属性は「書いた順」で出る（保存ファイルの差分を安定させるため）。
//   * 数値は "x y z" のように空白区切りで並べる（MuJoCo と同じ書き方）。
//   * 読み取りは失敗しても投げない。欠けた属性・型違いは fallback を返す
//     ので、手で書き換えた XML でも落ちずに読める（/input の jsonNumber と
//     同じ考え方）。
namespace wizengine {
namespace xml {

class Element {
public:
    Element() = default;
    explicit Element(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    // ---- 属性の読み取り ---------------------------------------------------
    bool has(const char* key) const;
    std::string attr(const char* key, const char* fallback = "") const;
    double number(const char* key, double fallback) const;
    int integer(const char* key, int fallback) const;
    bool boolean(const char* key, bool fallback) const;
    // 空白区切りの数値列。読めた個数を返し、読めた分だけ out を上書きする
    // （足りない分は呼び出し側の初期値のまま = 部分指定を許す）。
    std::size_t numbers(const char* key, double* out, std::size_t count) const;

    // ---- 属性の書き込み ---------------------------------------------------
    // 同じ key を二度書いたら上書き（順番は最初に書いた位置のまま）。
    void set(const char* key, std::string value);
    // precision は %g の有効桁。既定 10 は double をそのまま読み戻せる程度に
    // 細かく、かつ 0.1 が "0.1" と出る程度に丸い。float 由来の値は 6 を渡す
    // （0.8f を 10 桁で出すと "0.8000000119" になるため）。
    void setNumber(const char* key, double value, int precision = 10);
    void setInt(const char* key, long long value);
    void setBool(const char* key, bool value);
    void setNumbers(const char* key, const double* values, std::size_t count,
                    int precision = 10);

    // ---- 子要素 -----------------------------------------------------------
    void append(Element child) { children_.push_back(std::move(child)); }
    // 子を足してその参照を返す。返した参照は「次に append / add を呼ぶまで」
    // しか有効でない（vector の再確保）。入れ子を組むときは局所の Element を
    // 作ってから append する方が安全。
    Element& add(std::string name);

    const std::vector<Element>& children() const { return children_; }
    const Element* first(const char* name) const;
    std::vector<const Element*> all(const char* name) const;

    const std::vector<std::pair<std::string, std::string>>& attributes() const {
        return attributes_;
    }

private:
    std::string name_;
    std::vector<std::pair<std::string, std::string>> attributes_;
    std::vector<Element> children_;
};

// 整形して 1 本の文字列に。declaration = true で <?xml ...?> 行を先頭に付ける。
std::string write(const Element& root, bool declaration = true);

// 読み込み。成功したら true。失敗時は error に「行番号付きの理由」（英語、
// コンソールログに出る診断はすべて英語という規約）を入れる。
// 対応するのは要素・属性・自己閉じ・コメント・XML 宣言・DOCTYPE の読み飛ばし
// と、定義済み実体（&amp; &lt; &gt; &quot; &apos;）＋数値文字参照。
bool parse(const std::string& text, Element& out, std::string& error);

}  // namespace xml
}  // namespace wizengine
