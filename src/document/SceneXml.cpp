#include "document/SceneXml.h"

#include "core/Clamp.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>

namespace wizengine {
namespace xml {
namespace {

// 属性値に入れてよい形へ。テキストノードを持たないので、エスケープが要るのは
// 属性の中だけ。
std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            // 改行・CR・タブは文字参照で書く（UI の文字の改行を保存で残すため）。
            // 生のまま書くと、XML の属性値の正規化でほかの読み手は空白に
            // 変えてしまう（この DOM の読み込みは素通しだが、ファイルは人や
            // ほかの道具も読む）。読み込みの unescape は &#10; などを戻せる。
            case '\n': out += "&#10;"; break;
            case '\r': out += "&#13;"; break;
            case '\t': out += "&#9;"; break;
            default:
                // ほかの制御文字は XML に入れられない。落として続ける（保存名や
                // オブジェクト名に紛れ込んでも壊れたファイルにしないため）。
                if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
                break;
        }
    }
    return out;
}

// 実体参照を戻す。知らない実体はそのまま（& を落とさない）。
std::string unescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] != '&') {
            out.push_back(in[i++]);
            continue;
        }
        const std::size_t end = in.find(';', i + 1);
        if (end == std::string::npos || end - i > 12) {
            out.push_back(in[i++]);
            continue;
        }
        const std::string name = in.substr(i + 1, end - i - 1);
        if (name == "amp") out.push_back('&');
        else if (name == "lt") out.push_back('<');
        else if (name == "gt") out.push_back('>');
        else if (name == "quot") out.push_back('"');
        else if (name == "apos") out.push_back('\'');
        else if (name.size() > 1 && name[0] == '#') {
            const int base = (name[1] == 'x' || name[1] == 'X') ? 16 : 10;
            const char* digits = name.c_str() + (base == 16 ? 2 : 1);
            const long code = std::strtol(digits, nullptr, base);
            // ASCII の範囲だけ復元する。マルチバイトは UTF-8 のまま
            // 通っている（この DOM はバイト列を素通しする）ので、ここで
            // 変換が要るのは & < > " ' に相当する数値参照だけ。
            if (code > 0 && code < 128) out.push_back(char(code));
        } else {
            out.push_back('&');
            out += name;
            out.push_back(';');
        }
        i = end + 1;
    }
    return out;
}

bool isNameStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == ':';
}
bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == ':' || c == '-' || c == '.';
}
bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 属性が多い要素は 1 行に詰めず、要素名の下へ揃えて折り返す（MJCF の
// 見た目に近く、差分も読みやすい）。
constexpr std::size_t kWrapColumn = 96;

void writeElement(const Element& e, int depth, std::string& out) {
    const std::string indent(std::size_t(depth) * 2, ' ');
    out += indent;
    out += '<';
    out += e.name();
    // 折り返したときの継ぎ足し位置 = 要素名の直後に揃える。
    const std::string contIndent(indent.size() + 1 + e.name().size() + 1, ' ');
    std::size_t column = indent.size() + 1 + e.name().size();
    for (const auto& a : e.attributes()) {
        std::string text = a.first + "=\"" + escape(a.second) + '"';
        if (column + 1 + text.size() > kWrapColumn &&
            column > contIndent.size()) {
            out += '\n';
            out += contIndent;
            column = contIndent.size();
        } else {
            out += ' ';
            ++column;
        }
        column += text.size();
        out += text;
    }
    if (e.children().empty()) {
        out += "/>\n";
        return;
    }
    out += ">\n";
    for (const auto& c : e.children()) writeElement(c, depth + 1, out);
    out += indent;
    out += "</";
    out += e.name();
    out += ">\n";
}

// 1 本の文字列を舐めるだけのパーサ。位置と行番号を持ち、失敗は error に
// 「行番号 + 理由」で残す（手で書き換えた XML の直しやすさを優先）。
class Parser {
public:
    Parser(const std::string& text, std::string& error)
        : s_(text), error_(error) {}

    bool run(Element& out) {
        skipProlog();
        if (!expectElement(out)) return false;
        skipSpaceAndComments();
        // ルートの後ろに残りがあっても読み飛ばす（末尾のコメント等）。
        return true;
    }

private:
    void fail(const char* what) {
        if (!error_.empty()) return;  // 最初の失敗だけを残す
        error_ = "line " + std::to_string(line()) + ": " + what;
    }
    std::size_t line() const {
        std::size_t n = 1;
        for (std::size_t k = 0; k < i_ && k < s_.size(); ++k) {
            if (s_[k] == '\n') ++n;
        }
        return n;
    }
    bool eof() const { return i_ >= s_.size(); }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    bool starts(const char* lit) const {
        // i_ が末尾を越えていても投げない（compare は out_of_range を投げる）。
        return i_ <= s_.size() && s_.compare(i_, std::strlen(lit), lit) == 0;
    }

    void skipSpace() {
        while (!eof() && isSpace(s_[i_])) ++i_;
    }
    void skipSpaceAndComments() {
        for (;;) {
            skipSpace();
            if (starts("<!--")) {
                const std::size_t end = s_.find("-->", i_ + 4);
                i_ = (end == std::string::npos) ? s_.size() : end + 3;
                continue;
            }
            return;
        }
    }
    // 先頭の <?xml ...?> / DOCTYPE / コメントを読み飛ばす。
    void skipProlog() {
        for (;;) {
            skipSpaceAndComments();
            if (starts("<?")) {
                const std::size_t end = s_.find("?>", i_ + 2);
                i_ = (end == std::string::npos) ? s_.size() : end + 2;
                continue;
            }
            if (starts("<!DOCTYPE") || starts("<!")) {
                const std::size_t end = s_.find('>', i_ + 2);
                i_ = (end == std::string::npos) ? s_.size() : end + 1;
                continue;
            }
            return;
        }
    }

    std::string readName() {
        const std::size_t start = i_;
        if (!eof() && isNameStart(s_[i_])) {
            ++i_;
            while (!eof() && isNameChar(s_[i_])) ++i_;
        }
        return s_.substr(start, i_ - start);
    }

    bool expectElement(Element& out) {
        skipSpaceAndComments();
        if (peek() != '<') {
            fail("expected an element ('<')");
            return false;
        }
        ++i_;
        const std::string name = readName();
        if (name.empty()) {
            fail("cannot read an element name");
            return false;
        }
        out.setName(name);

        // 属性列。
        for (;;) {
            skipSpace();
            if (eof()) {
                fail("element is not closed");
                return false;
            }
            if (starts("/>")) {
                i_ += 2;
                return true;
            }
            if (peek() == '>') {
                ++i_;
                break;
            }
            const std::string key = readName();
            if (key.empty()) {
                fail("cannot read an attribute name");
                return false;
            }
            skipSpace();
            if (peek() != '=') {
                fail("attribute is missing '='");
                return false;
            }
            ++i_;
            skipSpace();
            const char quote = peek();
            if (quote != '"' && quote != '\'') {
                fail("attribute value is not quoted");
                return false;
            }
            ++i_;
            const std::size_t start = i_;
            while (!eof() && s_[i_] != quote) ++i_;
            if (eof()) {
                fail("attribute value is not closed");
                return false;
            }
            out.set(key.c_str(), unescape(s_.substr(start, i_ - start)));
            ++i_;
        }

        // 子要素（テキストは読み飛ばす）。
        for (;;) {
            skipSpaceAndComments();
            if (eof()) {
                fail("missing closing tag");
                return false;
            }
            if (starts("</")) {
                i_ += 2;
                const std::string close = readName();
                skipSpace();
                if (peek() != '>') {
                    fail("closing tag is not terminated");
                    return false;
                }
                ++i_;
                if (close != name) {
                    fail("closing tag name does not match");
                    return false;
                }
                return true;
            }
            if (peek() == '<') {
                if (starts("<?") || starts("<!")) {
                    const std::size_t end = s_.find('>', i_ + 2);
                    i_ = (end == std::string::npos) ? s_.size() : end + 1;
                    continue;
                }
                Element child;
                if (!expectElement(child)) return false;
                out.append(std::move(child));
                continue;
            }
            // テキストノードは使わないので、次の '<' まで捨てる。
            const std::size_t next = s_.find('<', i_);
            i_ = (next == std::string::npos) ? s_.size() : next;
        }
    }

    const std::string& s_;
    std::string& error_;
    std::size_t i_ = 0;
};

}  // namespace

bool Element::has(const char* key) const {
    for (const auto& a : attributes_) {
        if (a.first == key) return true;
    }
    return false;
}

std::string Element::attr(const char* key, const char* fallback) const {
    for (const auto& a : attributes_) {
        if (a.first == key) return a.second;
    }
    return std::string(fallback);
}

std::string formatNumber(double value, int precision) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
    // "-0" は読みづらいだけなので 0 に寄せる。
    if (std::strcmp(buf, "-0") == 0) return "0";
    return std::string(buf);
}

std::string formatBool(bool value) { return value ? "true" : "false"; }

std::string formatInt(long long value) { return std::to_string(value); }

std::string formatNumbers(const double* values, std::size_t count, int precision) {
    std::string out;
    for (std::size_t k = 0; k < count; ++k) {
        if (k) out.push_back(' ');
        out += formatNumber(values[k], precision);
    }
    return out;
}

std::size_t parseNumbers(const std::string& text, double* out, std::size_t count) {
    std::size_t got = 0;
    const char* p = text.c_str();
    while (got < count) {
        char* end = nullptr;
        const double parsed = std::strtod(p, &end);
        if (end == p) break;  // 数値として読めない（空白と符号だけ等）
        out[got++] = parsed;
        p = end;
    }
    return got;
}

bool parseBool(const std::string& text, bool& out) {
    if (text == "true" || text == "1" || text == "yes" || text == "on") {
        out = true;
        return true;
    }
    if (text == "false" || text == "0" || text == "no" || text == "off") {
        out = false;
        return true;
    }
    return false;
}

double Element::number(const char* key, double fallback) const {
    double v = fallback;
    return numbers(key, &v, 1) == 1 ? v : fallback;
}

int Element::integer(const char* key, int fallback) const {
    return roundToInt(number(key, double(fallback)), fallback);
}

bool Element::boolean(const char* key, bool fallback) const {
    if (!has(key)) return fallback;
    bool v = fallback;
    return parseBool(attr(key), v) ? v : fallback;
}

std::size_t Element::numbers(const char* key, double* out,
                             std::size_t count) const {
    if (!has(key) || count == 0) return 0;
    return parseNumbers(attr(key), out, count);
}

void Element::set(const char* key, std::string value) {
    for (auto& a : attributes_) {
        if (a.first == key) {
            a.second = std::move(value);
            return;
        }
    }
    attributes_.emplace_back(key, std::move(value));
}

bool Element::erase(const char* key) {
    for (auto it = attributes_.begin(); it != attributes_.end(); ++it) {
        if (it->first == key) {
            attributes_.erase(it);
            return true;
        }
    }
    return false;
}

void Element::setNumber(const char* key, double value, int precision) {
    set(key, formatNumber(value, precision));
}

void Element::setInt(const char* key, long long value) {
    set(key, formatInt(value));
}

void Element::setBool(const char* key, bool value) {
    set(key, formatBool(value));
}

void Element::setNumbers(const char* key, const double* values,
                         std::size_t count, int precision) {
    set(key, formatNumbers(values, count, precision));
}

Element& Element::add(std::string name) {
    children_.emplace_back(std::move(name));
    return children_.back();
}

const Element* Element::first(const char* name) const {
    for (const auto& c : children_) {
        if (c.name() == name) return &c;
    }
    return nullptr;
}

std::vector<const Element*> Element::all(const char* name) const {
    std::vector<const Element*> out;
    for (const auto& c : children_) {
        if (c.name() == name) out.push_back(&c);
    }
    return out;
}

std::string write(const Element& root, bool declaration) {
    std::string out;
    if (declaration) out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    writeElement(root, 0, out);
    return out;
}

bool parse(const std::string& text, Element& out, std::string& error) {
    error.clear();
    Element root;
    Parser p(text, error);
    if (!p.run(root)) {
        if (error.empty()) error = "not readable as XML";
        return false;
    }
    out = std::move(root);
    return true;
}

}  // namespace xml
}  // namespace wizengine
