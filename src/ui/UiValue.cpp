#include "ui/UiValue.h"

#include "core/AssetFileRule.h"
#include "core/Clamp.h"
#include "core/ColorText.h"
#include "core/Json.h"
#include "core/SafeName.h"
#include "core/Utf8.h"
#include "core/Xml.h"

#include <charconv>
#include <limits>
#include <cmath>

namespace wizengine {
namespace ui {
namespace {

// ---- 文字列の読み書きの下請け -----------------------------------------------
// 数値の並び・真偽値の表記はシーン文書と同じ規則（core/Xml の
// parseNumbers / parseBool / formatNumbers / formatBool / formatInt）で読み書きする。
bool isNumeric(PropType t) {
    switch (t) {
        case PropType::Number:
        case PropType::Int:
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Euler:
        case PropType::Rect:
        case PropType::Color:
            return true;
        default:
            return false;
    }
}
bool isString(PropType t) {
    return t == PropType::Text || t == PropType::Name || t == PropType::Path;
}

int defaultInt(const ValueSpec& spec) { return roundToInt(spec.def.v[0], 0); }

// 空白区切りの字の並び。
std::vector<std::string> splitTokens(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = text.find_first_not_of(" \t\r\n", i);
        if (start == std::string::npos) break;
        const std::size_t end = text.find_first_of(" \t\r\n", start);
        out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        i = end == std::string::npos ? text.size() : end;
    }
    return out;
}

// 番号が spec の範囲に入るか（ビットとして持てる範囲も見る）。
bool indexInSpec(const ValueSpec& spec, int k) {
    const bool representable = k >= 0 && k < kMaxIndexBits;
    return representable && double(k) >= spec.lo && double(k) <= spec.hi;
}

// 添字の並びの値（規則は UiValue.h の firstInvalidIndex）。
// 正しくない並び（どれも選ばない）。書いた文字列を保つ（長さは kMaxTextBytes まで）。
// 長すぎるときは、途中で切ると正しくない番号が落ちて正しい並びに化けうるので、最初の
// 正しくない番号だけを残す。それでも正しい並びに読める・空なら "?"（空は「正しい
// 並び」の印）。
PropValue invalidIndexList(const ValueSpec& spec, const std::string& text) {
    PropValue v;
    if (utf8PrefixLength(text, kMaxTextBytes) == text.size()) {
        v.s = text;
    } else {
        const std::string bad = firstInvalidIndex(spec, text);
        v.s = bad.substr(0, utf8PrefixLength(bad, kMaxTextBytes));
    }
    if (v.s.empty() || firstInvalidIndex(spec, v.s).empty()) v.s = "?";
    return v;
}

// ビットの集合として壊れた値（NaN・負・小数 = プログラムの誤り）を、黙って全部 / 一部に
// 出さず「どれも選ばない」並びにする（Q20 の考え方）。clampValue・書き出し・JSON が
// 同じ扱いになるよう、どれもここを通してから読む。
PropValue checkedIndexList(const ValueSpec& spec, const PropValue& value) {
    const bool broken = indexListValid(value) && !maskValueValid(value.v[0]);
    return broken ? invalidIndexList(spec, xml::formatNumber(value.v[0])) : value;
}

PropValue indexListFromText(const ValueSpec& spec, const std::string& text) {
    PropValue v;
    if (!firstInvalidIndex(spec, text).empty()) return invalidIndexList(spec, text);
    std::vector<int> indices;
    for (const std::string& token : splitTokens(text)) {
        int k = 0;
        parseIntToken(token, k);
        indices.push_back(k);
    }
    v.v[0] = double(indexMaskFromList(indices));
    return v;
}

}  // namespace

// ---- 部品 --------------------------------------------------------------------
bool parseIntToken(const std::string& token, int& out) {
    const char* first = token.data();
    const char* last = first + token.size();
    if (first != last && *first == '+') {
        ++first;  // from_chars は + を読まない
        if (first != last && (*first == '+' || *first == '-')) return false;  // 符号は 1 つだけ
    }
    const auto r = std::from_chars(first, last, out);
    return r.ec == std::errc() && r.ptr == last && first != last;
}

bool parseIntText(const std::string& text, int& out) {
    const std::vector<std::string> tokens = splitTokens(text);
    return tokens.size() == 1 && parseIntToken(tokens[0], out);
}

std::size_t enumNameCount(const char* const* names) {
    std::size_t n = 0;
    if (names) {
        while (names[n]) ++n;
    }
    return n;
}

int enumIndex(double v, const char* const* names, int fallback) {
    const int k = roundToInt(v, -1);
    const bool valid = k >= 0 && std::size_t(k) < enumNameCount(names);
    return valid ? k : fallback;
}

// v[0] がビットの集合として持てる範囲か（0 以上で 32 ビットに入る）。maskOf と
// maskValueValid の範囲の確かめはこれだけ。32 ビットまでは double で正確に持てる。
// NaN はどの比較も偽 = 範囲外。
static bool maskInRange(double v) {
    return v >= 0.0 && v <= double(std::numeric_limits<std::uint32_t>::max());
}

std::uint32_t maskOf(double v) {
    return maskInRange(v) ? static_cast<std::uint32_t>(v) : 0;  // 小数は切り捨て
}

bool maskValueValid(double v) { return maskInRange(v) && std::floor(v) == v; }

std::uint32_t indexMaskFromList(const std::vector<int>& indices) {
    std::uint32_t mask = 0;
    for (const int k : indices) {
        if (k >= 0 && k < kMaxIndexBits) mask |= 1u << unsigned(k);
    }
    return mask;
}

std::string firstInvalidIndex(const ValueSpec& spec, const std::string& text) {
    for (const std::string& token : splitTokens(text)) {
        int k = 0;
        const bool readable = parseIntToken(token, k);
        if (!readable || !indexInSpec(spec, k)) return token;
    }
    return {};
}

// ビットの集合を並びの文字列に（正しい並びの書き出し。この .cpp だけで使う）。
static std::string indexListText(std::uint32_t mask) {
    std::string out;
    for (const int k : indicesFromMask(mask)) {
        if (!out.empty()) out.push_back(' ');
        out += xml::formatInt(k);
    }
    return out;
}

std::vector<int> indicesFromMask(std::uint32_t mask) {
    std::vector<int> out;
    for (int k = 0; k < kMaxIndexBits; ++k) {
        if (mask & (1u << unsigned(k))) out.push_back(k);
    }
    return out;
}

std::size_t componentCount(PropType type) {
    switch (type) {
        case PropType::Vec2: return 2;
        case PropType::Vec3:
        case PropType::Euler: return 3;
        case PropType::Rect:
        case PropType::Color: return 4;
        default: return 1;
    }
}

const char* propTypeName(PropType type) {
    switch (type) {
        case PropType::Number: return "number";
        case PropType::Int: return "int";
        case PropType::Vec2: return "vec2";
        case PropType::Vec3: return "vec3";
        case PropType::Euler: return "euler";
        case PropType::Rect: return "rect";
        case PropType::Color: return "color";
        case PropType::Bool: return "bool";
        case PropType::Enum: return "enum";
        case PropType::Text: return "text";
        case PropType::Name: return "name";
        case PropType::Path: return "path";
        case PropType::IndexList: return "indexList";
    }
    return "number";
}

// ---- 表記と丸め --------------------------------------------------------------
bool parseValue(const ValueSpec& spec, const std::string& text, PropValue& out) {
    PropValue v = spec.def;
    switch (spec.type) {
        case PropType::Number:
            if (xml::parseNumbers(text, v.v, 1) < 1) return false;
            break;
        case PropType::Int:
            if (xml::parseNumbers(text, v.v, 1) < 1) return false;
            v.v[0] = roundToInt(v.v[0], defaultInt(spec));
            break;
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Euler:
        case PropType::Rect: {
            const std::size_t n = componentCount(spec.type);
            if (xml::parseNumbers(text, v.v, n) < n) return false;
            break;
        }
        case PropType::Color:
            if (!parseColorText(text, v.v)) return false;
            break;
        case PropType::Bool: {
            bool b = false;
            if (!xml::parseBool(text, b)) return false;
            v.v[0] = b ? 1.0 : 0.0;
            break;
        }
        case PropType::Enum: {
            const std::size_t n = enumNameCount(spec.enumNames);
            std::size_t k = 0;
            while (k < n && text != spec.enumNames[k]) ++k;
            if (k == n) return false;
            v.v[0] = double(k);
            break;
        }
        case PropType::Text:
        case PropType::Name:
        case PropType::Path:
            v.s = text;
            break;
        case PropType::IndexList: {
            v = indexListFromText(spec, text);
            break;
        }
    }
    out = std::move(v);
    return true;
}

std::string formatValue(const ValueSpec& spec, const PropValue& value) {
    switch (spec.type) {
        case PropType::Number:
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Euler:
        case PropType::Rect:
            return xml::formatNumbers(value.v, componentCount(spec.type));
        case PropType::Color:
            return xml::formatNumbers(value.v, 4, kColorPrecision);
        case PropType::Int:
            return xml::formatInt(roundToInt(value.v[0], defaultInt(spec)));
        case PropType::Bool:
            return xml::formatBool(value.v[0] != 0.0);
        case PropType::Enum: {
            const int k = enumIndex(value.v[0], spec.enumNames, -1);
            return k >= 0 ? spec.enumNames[k] : "";
        }
        case PropType::Text:
        case PropType::Name:
        case PropType::Path:
            return value.s;
        case PropType::IndexList: {
            // 正しくない並びは書かれた文字列のまま戻す。
            const PropValue checked = checkedIndexList(spec, value);
            return indexListValid(checked) ? indexListText(maskOf(checked.v[0])) : checked.s;
        }
    }
    return {};
}

nlohmann::json valueToJson(const ValueSpec& spec, const PropValue& value) {
    switch (spec.type) {
        case PropType::Number:
            return value.v[0];
        case PropType::Int:
            return roundToInt(value.v[0], defaultInt(spec));
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Euler:
        case PropType::Rect:
        case PropType::Color: {
            nlohmann::json a = nlohmann::json::array();
            for (std::size_t k = 0; k < componentCount(spec.type); ++k) a.push_back(value.v[k]);
            return a;
        }
        case PropType::Bool:
            return value.v[0] != 0.0;
        case PropType::Enum:
            return formatValue(spec, value);
        case PropType::Text:
        case PropType::Name:
        case PropType::Path:
            return value.s;
        case PropType::IndexList: {
            // 正しくない並びは文字列（正しい並びは番号の配列）。
            const PropValue checked = checkedIndexList(spec, value);
            if (!indexListValid(checked)) return checked.s;
            return indicesFromMask(maskOf(checked.v[0]));
        }
    }
    return nullptr;
}

bool valueFromJson(const ValueSpec& spec, const nlohmann::json& obj, const char* key,
                   PropValue& out) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    if (it == obj.end()) return false;
    const nlohmann::json& j = *it;
    PropValue v = spec.def;
    // 数値の配列（count 個以上 maxCount 個以下、全部が数値）を v へ。
    auto numberArray = [&j, &v](std::size_t count, std::size_t maxCount) {
        if (!j.is_array() || j.size() < count || j.size() > maxCount) return false;
        for (std::size_t k = 0; k < j.size(); ++k) {
            if (!j[k].is_number()) return false;
        }
        for (std::size_t k = 0; k < j.size(); ++k) v.v[k] = j[k].get<double>();
        return true;
    };
    switch (spec.type) {
        case PropType::Number:
            if (!j.is_number()) return false;
            v.v[0] = jsonNumber(obj, key, 0.0);
            break;
        case PropType::Int:
            if (!j.is_number()) return false;
            v.v[0] = roundToInt(jsonNumber(obj, key, 0.0), defaultInt(spec));
            break;
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Euler:
        case PropType::Rect: {
            const std::size_t n = componentCount(spec.type);
            if (!numberArray(n, n)) return false;
            break;
        }
        case PropType::Color:
            if (j.is_string()) {
                // ブラウザの <input type="color"> の "#rrggbb"（不透明）など。
                if (!parseColorText(jsonString(obj, key, ""), v.v)) return false;
                break;
            }
            v.v[3] = 1.0;
            if (!numberArray(3, 4)) return false;
            break;
        case PropType::Bool:
            if (!j.is_boolean()) return false;
            v.v[0] = j.get<bool>() ? 1.0 : 0.0;
            break;
        case PropType::Enum:
            if (!j.is_string()) return false;
            return parseValue(spec, jsonString(obj, key, ""), out);
        case PropType::Text:
        case PropType::Name:
        case PropType::Path:
            if (!j.is_string()) return false;
            v.s = jsonString(obj, key, "");
            break;
        case PropType::IndexList: {
            // 数の配列か、XML と同じ空白区切りの文字列を受ける（それ以外の型は false）。
            // 中身の規則は XML と同じ（文字列の並びにして同じ関数で読む = 範囲外・小数・
            // 読めない番号があれば正しくない並び）。文字列を受けるのは、正しくない並びを
            // valueToJson が書かれた文字列のまま返すため（ブラウザが受けた "1 20" をそのまま
            // 送り返しても拒まない）。整数の値の数（1.0 など。ブラウザの数は double）は
            // 整数として受ける。
            if (j.is_string()) {
                v = indexListFromText(spec, jsonString(obj, key, ""));
                break;
            }
            if (!j.is_array()) return false;
            std::string text;
            for (const auto& e : j) {
                if (!e.is_number()) return false;
                if (!text.empty()) text.push_back(' ');
                const double d = e.get<double>();
                // 整数の判定は roundToInt と同じ int の範囲（入らない値は fallback の 0 が
                // 返って一致しない = 字のまま読んで正しくない並びになる）。
                const int k = roundToInt(d, 0);
                const bool integral = double(k) == d;
                text += integral ? xml::formatInt(k) : e.dump();
            }
            v = indexListFromText(spec, text);
            break;
        }
    }
    out = std::move(v);
    return true;
}

PropValue clampValue(const ValueSpec& spec, PropValue value) {
    switch (spec.type) {
        case PropType::Number:
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Rect:
        case PropType::Color:
            for (std::size_t k = 0; k < componentCount(spec.type); ++k) {
                value.v[k] = clampOrFallback(value.v[k], spec.lo, spec.hi,
                                                        spec.def.v[k]);
            }
            break;
        case PropType::Euler:
            for (std::size_t k = 0; k < 3; ++k) value.v[k] = clampAngleDegrees(value.v[k]);
            break;
        case PropType::Int:
            value.v[0] = roundToInt(
                clampOrFallback(value.v[0], spec.lo, spec.hi, spec.def.v[0]),
                defaultInt(spec));
            break;
        case PropType::Bool:
            // 有限でない値（NaN と ±∞）は既定値（ヘッダの約束）。それ以外は 0 でなければ真。
            if (!std::isfinite(value.v[0])) value.v[0] = spec.def.v[0];
            value.v[0] = value.v[0] != 0.0 ? 1.0 : 0.0;
            break;
        case PropType::Enum:
            value.v[0] = enumIndex(value.v[0], spec.enumNames,
                                   enumIndex(spec.def.v[0], spec.enumNames, 0));
            break;
        case PropType::Text:
            value.s.resize(utf8PrefixLength(value.s, std::size_t(spec.hi)));
            break;
        case PropType::Name:
            value.s = sanitizeSafeName(value.s);
            break;
        case PropType::Path: {
            const bool tooLong = value.s.size() > std::size_t(spec.hi);
            const bool allowed = value.s.empty() || assetFileAllowed(value.s);
            if (tooLong || !allowed) value.s = spec.def.s;
            break;
        }
        case PropType::IndexList:
            // 正しくない並びはそのまま。正しい並びも範囲の外の番号を含めば、同じ
            // 規則で正しくない並びになる（一部だけを残さない）。
            // ビットの集合として壊れた値も checkedIndexList が正しくない並びにする。
            value = checkedIndexList(spec, value);
            if (indexListValid(value)) value = indexListFromText(spec, indexListText(maskOf(value.v[0])));
            break;
    }
    return value;
}

PropValue lerpValue(const ValueSpec& spec, const PropValue& a, const PropValue& b,
                    double t) {
    if (!isNumeric(spec.type)) return t < 1.0 ? a : b;
    PropValue v = a;
    for (std::size_t k = 0; k < componentCount(spec.type); ++k) {
        v.v[k] = a.v[k] + (b.v[k] - a.v[k]) * t;
    }
    if (spec.type == PropType::Int) v.v[0] = roundToInt(v.v[0], roundToInt(a.v[0], 0));
    return v;
}

bool sameValue(const ValueSpec& spec, const PropValue& a, const PropValue& b) {
    if (isString(spec.type)) return a.s == b.s;
    if (spec.type == PropType::IndexList && a.s != b.s) return false;  // 正しくない並び
    for (std::size_t k = 0; k < componentCount(spec.type); ++k) {
        if (a.v[k] != b.v[k]) return false;
    }
    return true;
}

}  // namespace ui
}  // namespace wizengine
