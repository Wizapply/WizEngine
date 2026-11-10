#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ui/UiValue.h"

// UI の要素の「性質」の表と、その値の入れ物。
//
// 性質（位置・大きさ・色・文字…）の知識 - XML / JSON の名前・型・既定値・
// 範囲・アニメできるか・どの種類の要素が持つか・Inspector に出す日本語の
// 表示名 - は、この表 1 か所にだけ書く。XML の読み書き・JSON の部分更新・
// 範囲の丸め・補間・重ね順は、どれも性質ごとの分岐を持たずに表を引く
// （性質を 1 つ足す = 表に 1 行）。値の型ごとの表記と丸めは UiValue。
//
// Chrono / Filament / Scene を知らない純粋な層（tests/ui で単体で検査する）。
namespace wizengine {
namespace ui {

// ---- 要素の種類のビット ------------------------------------------------------
// 性質の表が「どの種類の要素が持つ性質か」を書くためのビット。種類の表
// （UiKinds）はこの値を自分の id として持つ。種類を足したらここへ 1 行。
using KindMask = std::uint32_t;
inline constexpr KindMask kKindText = 1u << 0;
inline constexpr KindMask kKindImage = 1u << 1;
inline constexpr KindMask kKindButton = 1u << 2;
inline constexpr KindMask kKindAll = 0xFFFFFFFFu;

// ---- 性質の表 ----------------------------------------------------------------
enum class Prop : std::uint8_t {
    Anchor, Pivot, Pos, Size, Color, Alpha, Visible, Text, Font, FontSize,
    Align, Outline, Wrap, File, Hover, Pressed, Enabled, Count
};
inline constexpr std::size_t kPropCount = static_cast<std::size_t>(Prop::Count);

struct PropInfo {
    Prop id;
    const char* name;   // XML の属性名 = JSON のキー
    ValueSpec spec;
    bool animatable;    // アニメのキーに書けるか
    KindMask kinds;     // この性質を持つ種類
    const char* label;  // Inspector の表示名（日本語）
};

const PropInfo& propInfo(Prop id);
// 名前 → 性質。見つからなければ nullptr で、suggestion があればそこへ
// 「近い名前」（大文字小文字違い・2 文字以内の打ち間違い。無ければ nullptr）を
// 入れる（文書の読み込みの「もしかして」に使う）。
const PropInfo* propFromName(const std::string& name,
                             const PropInfo** suggestion = nullptr);
// names[0..count) のうち name に近いもの（大文字小文字を無視して一致、
// 無ければ編集距離 2 以内で最も近いもの）。無ければ nullptr。性質の名前・
// Canvas の属性名・種類の名前で同じ規則を使うための口。
const char* nearestName(const std::string& name, const char* const* names,
                        std::size_t count);

// ---- 性質の集まり ------------------------------------------------------------
// 立っている性質だけが意味を持つ（部分更新・重ね順の段・アニメのキー）。
// 立っていない性質を get すると表の既定値が返る。
class PropSet {
public:
    bool has(Prop p) const { return set_.test(index(p)); }
    bool empty() const { return set_.none(); }
    // 値（立っていなければ表の既定値）。
    const PropValue& get(Prop p) const;
    double number(Prop p, std::size_t component = 0) const {
        return get(p).v[component];
    }
    bool flag(Prop p) const { return get(p).v[0] != 0.0; }
    const std::string& text(Prop p) const { return get(p).s; }

    void set(Prop p, PropValue value);
    void setNumber(Prop p, double v0, double v1 = 0.0, double v2 = 0.0,
                   double v3 = 0.0);
    void setText(Prop p, std::string s);
    void clear(Prop p);
    // upper で立っている性質を上書きする（重ね順の 1 段）。
    void overlay(const PropSet& upper);

    // XML の属性文字列 ⇄ 値（型は表）。parse は型が合わなければ false で
    // 何も変えない。format は立っていなくても既定値を文字列にする。
    bool parse(Prop p, const std::string& text);
    std::string format(Prop p) const;

private:
    static std::size_t index(Prop p) { return static_cast<std::size_t>(p); }
    std::bitset<kPropCount> set_;
    std::array<PropValue, kPropCount> v_{};
};

// 立っている性質だけを {名前: 値} にする。
nlohmann::json toJson(const PropSet& props);
// JSON の部分更新。j の中の性質の名前のキーだけを見て、型の合うものを
// props へ書く。型の合わないキーは書かずに名前を返す（呼び手がステータスや
// 警告に出す）。性質でないキーは見ない（同じ JSON に他の欄があってよい）。
std::vector<std::string> fromJson(const nlohmann::json& j, PropSet& props);
// 立っている性質を表の範囲へ丸める。
PropSet clampProps(PropSet props);
// a → b の補間（t は 0..1）。両方に立っている性質は lerpValue、片方だけの
// 性質はその値のまま。
PropSet lerpProps(const PropSet& a, const PropSet& b, double t);
// 重ね順の唯一の定義: 設計値 → アニメ → ノード → ポインタ（hover / pressed）。
// 後の段ほど強い。
PropSet resolve(const PropSet& design, const PropSet& anim, const PropSet& node,
                const PropSet& pointer);

}  // namespace ui
}  // namespace wizengine
