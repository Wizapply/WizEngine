#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "document/EditorTypes.h"
#include "document/SceneXml.h"

// シーン文書の値の表記（属性文字列 ⇄ 値）と読み取りの警告のうち、シーン文書
// （SceneDocument.cpp）と UI（ui/UiValue.cpp・ui/UiXml.cpp）が共用するもの。
// 表記の規則と警告の流儀はここ 1 か所に書く。
namespace wizengine {
namespace editor {

// ---- 警告の集積 -------------------------------------------------------------
// 「読めるけれど意図と違うかもしれない」内容を文で集める。読み取りを止めない
// （既定値へ落として続行する）ので、代わりにここへ痕跡を残す。out が無ければ
// 何もしない。壊れた文書ほど警告が湧くので、件数に上限を置く。
class Warn {
public:
    explicit Warn(std::vector<std::string>* out) : out_(out) {}
    void operator()(std::string msg) {
        if (!out_) return;
        if (out_->size() >= kMax) {
            if (!overflowNoted_) {
                out_->push_back("(further warnings omitted)");
                overflowNoted_ = true;
            }
            return;
        }
        out_->push_back(std::move(msg));
    }

private:
    static constexpr std::size_t kMax = 40;
    std::vector<std::string>* out_;
    bool overflowNoted_ = false;
};

// 名前が語彙に無いか。FromName 系は未知の名前で fallback をそのまま返すので、
// 別々の fallback で 2 回呼んで結果が食い違えば「知らない名前」と分かる
// （語彙の文字列一覧をここへ複製して同期漏れを作らないための判定）。
template <typename T, typename F>
bool unknownName(const F& fromName, const std::string& s, T a, T b) {
    return fromName(s, a) != fromName(s, b);
}

// 親の子要素のうち「知らない名前」を警告する。節を増やしたら known へ足す
// （SceneDocument.h の「拡張の手順」）。
inline void warnUnknownChildren(const xml::Element& parent,
                                std::initializer_list<const char*> known,
                                const char* where, Warn& warn) {
    for (const auto& c : parent.children()) {
        bool ok = false;
        for (const char* k : known) {
            if (c.name() == k) { ok = true; break; }
        }
        if (!ok) {
            warn(std::string("unsupported <") + c.name() + "> in " + where +
                 " - ignored");
        }
    }
}

// ---- 属性の読み書き（Vec3 と色）--------------------------------------------
inline void setVec3(xml::Element& e, const char* key, const Vec3d& v) {
    const double a[3] = {v.x, v.y, v.z};
    e.setNumbers(key, a, 3);
}
inline Vec3d getVec3(const xml::Element& e, const char* key, const Vec3d& fallback) {
    double a[3] = {fallback.x, fallback.y, fallback.z};
    e.numbers(key, a, 3);
    return Vec3d{a[0], a[1], a[2]};
}

// 色を書くときの有効桁（float 由来なので 6 桁で十分）。
inline constexpr int kColorPrecision = 6;

// 色の表記の規則（唯一の定義）: 空白区切りの数値が 3 個以上なら r g b [a]
// （rgba のリニア値。a を書かなければ 1）、そうでなければブラウザ
// API と同じ "#rrggbb"（a = 1。手書きしやすい）。どちらでもなければ false で
// out は変えない。数値の読み方は xml::parseNumbers（Element の数値列と同じ）。
inline bool parseColorText(const std::string& text, double out[4]) {
    double a[4] = {0.0, 0.0, 0.0, 1.0};
    if (xml::parseNumbers(text, a, 4) < 3) {
        const Color3 bad{-1.0f, -1.0f, -1.0f};
        const Color3 c = colorFromHex(text, bad);
        if (c.r < 0.0f) return false;
        a[0] = c.r;
        a[1] = c.g;
        a[2] = c.b;
        a[3] = 1.0;
    }
    for (int k = 0; k < 4; ++k) out[k] = a[k];
    return true;
}

// 色は rgba="r g b a"（リニア値）で書く。a は今は使わないが、4 つ並べて
// 書くのが文書の色の書き方なので、1 を入れておく。
inline void setColor(xml::Element& e, const char* key, const Color3& c) {
    const double a[4] = {c.r, c.g, c.b, 1.0};
    e.setNumbers(key, a, 4, kColorPrecision);
}

inline Color3 getColor(const xml::Element& e, const char* key, const Color3& fallback) {
    double a[4];
    if (!e.has(key) || !parseColorText(e.attr(key), a)) return fallback;
    Color3 c;
    c.r = float(a[0]);
    c.g = float(a[1]);
    c.b = float(a[2]);
    return c;
}

}  // namespace editor
}  // namespace wizengine
