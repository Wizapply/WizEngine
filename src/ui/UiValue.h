#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// UI の値の型と表記（XML の属性文字列・JSON ⇄ 値、丸め、補間）。
//
// 性質の表（UiProps）と Canvas の属性の表（UiTypes.h の kCanvasAttrs）が同じ
// ValueSpec を持ち、ここの関数で読み書きする。どの性質の値かは知らない
// （直すきっかけは「値の型や表記を足す・変える」だけ）。
//
// Chrono / Filament / Scene を知らない純粋な層（tests/ui で単体で検査する）。
namespace wizengine {
namespace ui {

// ---- 値の型 ------------------------------------------------------------------
enum class PropType : std::uint8_t {
    Number,     // v[0]
    Int,        // v[0]（core/Clamp.h の roundToInt で整数に丸める）
    Vec2,       // v[0..1]
    Vec3,       // v[0..2]
    Euler,      // v[0..2]（度。丸めは core/Clamp.h の clampAngleDegrees = ±360 の外は畳む）
    Rect,       // v[0..3]（minX minY maxX maxY）
    Color,      // v[0..3]（r g b a、リニア値）
    Bool,       // v[0] が 0 / 1
    Enum,       // v[0] が名前の一覧の添字
    Text,       // s（自由な文字列。hi は最大バイト数）
    Name,       // s（sanitizeSafeName で正規化する参照名）
    Path,       // s（assets/ からの相対パス。hi は最大バイト数）
    IndexList,  // v[0] が添字のビットの集合（0 = 空 = 「全部」の意味は使う側）。
                // 正しくない並び（範囲外・読めない番号を含む）は s に書かれた文字列を
                // 持ち、どの番号も選ばない（indexListValid）。
};

// 値。数値系は v、文字列系は s を使う（型は表が決める）。
struct PropValue {
    double v[4] = {0.0, 0.0, 0.0, 0.0};
    std::string s;
};
// 数値系 / 文字列系の値を作る（表の既定値と PropSet::setNumber / setText が使う）。
inline PropValue numberValue(double a, double b = 0.0, double c = 0.0, double d = 0.0) {
    PropValue v;
    v.v[0] = a;
    v.v[1] = b;
    v.v[2] = c;
    v.v[3] = d;
    return v;
}
inline PropValue textValue(std::string s) {
    PropValue v;
    v.s = std::move(s);
    return v;
}

// 値の型と既定値と範囲。
struct ValueSpec {
    PropType type = PropType::Number;
    PropValue def;
    double lo = 0.0;  // 数値系は各成分の下限、IndexList は添字の下限
    double hi = 0.0;  // 数値系は各成分の上限、IndexList は添字の上限、Text / Path は最大バイト数
    const char* const* enumNames = nullptr;  // Enum の名前（nullptr で終わる）
};

// ---- 部品 --------------------------------------------------------------------
// 文字列の値の上限（バイト。Text の性質と、正しくない添字の並びに保つ文字列）。
inline constexpr std::size_t kMaxTextBytes = 4096;

// 整数への丸めは core/Clamp.h の roundToInt（XML の整数属性と同じ規則）。

// 整数の字（空白を含まない 1 語）を読む唯一の口。全部が整数の字（先頭の + / - は 1 つ可）で
// int に入るときだけ true（"3abc" や "1.5" は false）。文書の番号（id / parent）と
// 添字の並びが使う。
bool parseIntToken(const std::string& token, int& out);
// 前後の空白を許して整数 1 つを読む（空白の決まりは添字の並びと同じ。中身は
// parseIntToken）。文書の番号（id / parent）が使う。
bool parseIntText(const std::string& text, int& out);

// Enum の名前の数（nullptr まで）。
std::size_t enumNameCount(const char* const* names);
// Enum の添字として使える値か（整数に丸めて 0..count-1）。使えなければ fallback。
int enumIndex(double v, const char* const* names, int fallback);

// 添字の集合（IndexList）。添字は 0..kMaxIndexBits-1 のビットで持つ。
inline constexpr int kMaxIndexBits = 32;
static_assert(kMaxIndexBits == 8 * sizeof(std::uint32_t), "one bit per index in a uint32_t");
// v[0] に入っているビットの集合。NaN・負・32 ビットに入らない値は空。
std::uint32_t maskOf(double v);
// v[0] がビットの集合として正しい値か（0 以上の整数で 32 ビットに入る）。NaN・負・
// 小数（プログラムの誤り）は正しくない = clampValue が「どれも選ばない」並びにする。
bool maskValueValid(double v);
// 添字の並び → ビットの集合（範囲外の添字は落とす）。
std::uint32_t indexMaskFromList(const std::vector<int>& indices);
// ビットの集合 → 添字の並び（小さい順）。
std::vector<int> indicesFromMask(std::uint32_t mask);

// 添字の並びの規則（唯一の定義）: 空白区切りの整数で、どれも spec の lo..hi
// （かつ 0..kMaxIndexBits-1）に入っていること。1 つでも外れる・整数として
// 読めない番号があれば、並び全体が「正しくない並び」になり、どの番号も選ばない
// （一部だけを残すことはしない）。正しくない並びは書かれた文字列を s に保ち、
// 書き出しでそのまま戻す（書いた人が自分の番号を見て直せる）。
// text の中で最初に正しくない番号（無ければ空 = 正しい並び）。
std::string firstInvalidIndex(const ValueSpec& spec, const std::string& text);
// 正しい並びか（正しくない並びは s に元の文字列を持つ）。
inline bool indexListValid(const PropValue& value) { return value.s.empty(); }

// 型の成分の数（数値系）。文字列系と Bool / Enum / IndexList は 1。
std::size_t componentCount(PropType type);

// ---- 表記と丸め --------------------------------------------------------------
// XML の属性文字列 → 値。型に合わなければ false で out は変えない。色は
// core/ColorText.h の parseColorText（"r g b [a]" と "#rrggbb"）。IndexList は失敗しない
// （正しくない並びも値。上の firstInvalidIndex の規則。JSON も同じ規則）。
bool parseValue(const ValueSpec& spec, const std::string& text, PropValue& out);
// 値 → XML の属性文字列（parseValue で読み戻せる形）。
std::string formatValue(const ValueSpec& spec, const PropValue& value);
// 値 ⇄ JSON。valueFromJson は obj[key] を読み、無い・型が合わなければ false で
// out は変えない（/input は誰でも叩けるので型を信じない）。色は [r,g,b,a] の
// 配列で返し、[r,g,b] と色の文字列（parseColorText）も受ける。
nlohmann::json valueToJson(const ValueSpec& spec, const PropValue& value);
bool valueFromJson(const ValueSpec& spec, const nlohmann::json& obj,
                   const char* key, PropValue& out);
// 範囲へ丸める（有限でない値（NaN と ±∞）は既定値、名前は正規化、許されないパスは既定値）。
PropValue clampValue(const ValueSpec& spec, PropValue value);
// 補間。数値系は成分ごとに線形、それ以外（文字列・Bool・Enum・IndexList）は
// 段（t < 1 の間は a、1 で b）。
PropValue lerpValue(const ValueSpec& spec, const PropValue& a, const PropValue& b,
                    double t);
// 型の意味で同じ値か（保存で既定値を書かないための比較）。
bool sameValue(const ValueSpec& spec, const PropValue& a, const PropValue& b);

}  // namespace ui
}  // namespace wizengine
