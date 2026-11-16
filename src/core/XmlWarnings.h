#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "core/Xml.h"

// XML の読み取りの警告の流儀（シーン文書・UI の文書・車両・計算式が共用する唯一の定義）。
namespace wizengine {
namespace xml {

// ---- 警告の集積 -------------------------------------------------------------
// 「読めるけれど意図と違うかもしれない」内容を文で集める。読み取りを止めない
// （既定値へ落として続行する）ので、代わりにここへ痕跡を残す。out が無ければ
// 何もしない。壊れた文書ほど警告が湧くので、件数に上限を置く。上限は文書全体で
// 1 つ: 入れ子の部品（車両・計算式）は親の Warn から作った子の Warn（文の先頭に
// prefix を付けて親へ流す）を使い、自分では一覧も上限も持たない。
class Warn {
public:
    explicit Warn(std::vector<std::string>* out) : out_(out) {}
    // 親と同じ一覧・同じ上限へ、文の先頭に prefix を付けて流す。親より長く使わない。
    Warn(Warn& parent, std::string prefix)
        : out_(nullptr), parent_(&parent), prefix_(std::move(prefix)) {}
    void operator()(std::string msg) {
        if (parent_) {
            (*parent_)(prefix_ + msg);
            return;
        }
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
    Warn* parent_ = nullptr;
    std::string prefix_;
    bool overflowNoted_ = false;
};

// 名前が語彙に無いか。FromName 系は未知の名前で fallback をそのまま返すので、
// 別々の fallback で 2 回呼んで結果が食い違えば「知らない名前」と分かる
// （語彙の文字列一覧をここへ複製して同期漏れを作らないための判定）。
template <typename T, typename F>
bool unknownName(const F& fromName, const std::string& s, T a, T b) {
    return fromName(s, a) != fromName(s, b);
}

// 親の子要素のうち「知らない名前」を警告する。known は呼ぶ側が持つ一覧で、
// 節を増やしたら呼ぶ側がそこへ足す。prefix は文の先頭に付ける文字列（車両の
// "<vehicle>: " のように、どの文書の警告かを示す。既定は無し）。
inline void warnUnknownChildren(const Element& parent,
                                std::initializer_list<const char*> known,
                                const char* where, Warn& warn,
                                const char* prefix = "") {
    for (const auto& c : parent.children()) {
        bool ok = false;
        for (const char* k : known) {
            if (c.name() == k) { ok = true; break; }
        }
        if (!ok) {
            warn(std::string(prefix) + "unsupported <" + c.name() + "> in " + where +
                 " - ignored");
        }
    }
}

}  // namespace xml
}  // namespace wizengine
