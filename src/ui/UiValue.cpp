#include "ui/UiValue.h"

#include "core/SafeName.h"
#include "core/Utf8.h"
#include "document/EditorTypes.h"
#include "document/SceneXml.h"
#include "document/XmlValues.h"

#include <climits>
#include <limits>
#include <cmath>

namespace wizengine {
namespace ui {
namespace {

// ---- 文字列の読み書きの下請け -----------------------------------------------
// 数値の並び・真偽値の表記はシーン文書（xml::Element）と同じ規則で読み書き
// する。規則を 2 か所に書かないよう、一時的な要素の属性を通して使う。
constexpr const char* kTmpKey = "v";

// 空白区切りの数値を最大 count 個読む（読めた個数を返す）。
std::size_t readNumbers(const std::string& text, double* out, std::size_t count) {
    xml::Element e;
    e.set(kTmpKey, text);
    return e.numbers(kTmpKey, out, count);
}
// 真偽値の表記（true / 1 / yes / on …）。読めなければ false。
bool readBool(const std::string& text, bool& out) {
    xml::Element e;
    e.set(kTmpKey, text);
    // 読めない表記は fallback がそのまま返るので、2 通りで食い違えば不正。
    const bool t = e.boolean(kTmpKey, true);
    const bool f = e.boolean(kTmpKey, false);
    if (t != f) return false;
    out = t;
    return true;
}
std::string writeNumbers(const double* values, std::size_t count, int precision = 10) {
    xml::Element e;
    e.setNumbers(kTmpKey, values, count, precision);
    return e.attr(kTmpKey);
}
std::string writeBool(bool b) {
    xml::Element e;
    e.setBool(kTmpKey, b);
    return e.attr(kTmpKey);
}
std::string writeInt(int k) {
    xml::Element e;
    e.setInt(kTmpKey, k);
    return e.attr(kTmpKey);
}

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

// 先頭から maxBytes バイト以内で、字の途中で切らない長さ。
std::size_t utf8PrefixLength(const std::string& s, std::size_t maxBytes) {
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t next = i;
        nextCodepoint(s, next);
        if (next > maxBytes) break;
        i = next;
    }
    return i;
}

int defaultInt(const ValueSpec& spec) { return roundInt(spec.def.v[0], 0); }

}  // namespace

// ---- 部品 --------------------------------------------------------------------
int roundInt(double v, int fallback) {
    const double r = std::round(v);  // 0.5 は 0 から遠い方へ
    // NaN はどの比較も偽 = fallback。
    const bool inRange = r >= double(INT_MIN) && r <= double(INT_MAX);
    return inRange ? int(r) : fallback;
}

std::size_t enumNameCount(const char* const* names) {
    std::size_t n = 0;
    if (names) {
        while (names[n]) ++n;
    }
    return n;
}

int enumIndex(double v, const char* const* names, int fallback) {
    const int k = roundInt(v, -1);
    const bool valid = k >= 0 && std::size_t(k) < enumNameCount(names);
    return valid ? k : fallback;
}

std::uint32_t maskOf(double v) {
    // 32 ビットまでは double で正確に持てる。NaN はどの比較も偽 = 空。
    if (!(v >= 0.0) || v > double(std::numeric_limits<std::uint32_t>::max())) return 0;
    return static_cast<std::uint32_t>(v);
}

std::uint32_t indexMaskFromList(const std::vector<int>& indices) {
    std::uint32_t mask = 0;
    for (const int k : indices) {
        if (k >= 0 && k < kMaxIndexBits) mask |= 1u << unsigned(k);
    }
    return mask;
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

// ---- 表記と丸め --------------------------------------------------------------
bool parseValue(const ValueSpec& spec, const std::string& text, PropValue& out) {
    PropValue v = spec.def;
    switch (spec.type) {
        case PropType::Number:
            if (readNumbers(text, v.v, 1) < 1) return false;
            break;
        case PropType::Int:
            if (readNumbers(text, v.v, 1) < 1) return false;
            v.v[0] = roundInt(v.v[0], defaultInt(spec));
            break;
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Euler:
        case PropType::Rect: {
            const std::size_t n = componentCount(spec.type);
            if (readNumbers(text, v.v, n) < n) return false;
            break;
        }
        case PropType::Color:
            if (!editor::parseColorText(text, v.v)) return false;
            break;
        case PropType::Bool: {
            bool b = false;
            if (!readBool(text, b)) return false;
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
            double a[kMaxIndexBits];
            const std::size_t n = readNumbers(text, a, kMaxIndexBits);
            // 1 個も読めないのに空白以外の字があれば不正（空 = 空の集合は可）。
            const bool blank = text.find_first_not_of(" \t\r\n") == std::string::npos;
            if (n == 0 && !blank) return false;
            std::vector<int> indices;
            for (std::size_t k = 0; k < n; ++k) indices.push_back(roundInt(a[k], -1));
            v.v[0] = double(indexMaskFromList(indices));
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
            return writeNumbers(value.v, componentCount(spec.type));
        case PropType::Color:
            return writeNumbers(value.v, 4, editor::kColorPrecision);
        case PropType::Int:
            return writeInt(roundInt(value.v[0], defaultInt(spec)));
        case PropType::Bool:
            return writeBool(value.v[0] != 0.0);
        case PropType::Enum: {
            const int k = enumIndex(value.v[0], spec.enumNames, -1);
            return k >= 0 ? spec.enumNames[k] : "";
        }
        case PropType::Text:
        case PropType::Name:
        case PropType::Path:
            return value.s;
        case PropType::IndexList: {
            std::string out;
            for (const int k : indicesFromMask(maskOf(value.v[0]))) {
                if (!out.empty()) out.push_back(' ');
                out += writeInt(k);
            }
            return out;
        }
    }
    return {};
}

nlohmann::json valueToJson(const ValueSpec& spec, const PropValue& value) {
    switch (spec.type) {
        case PropType::Number:
            return value.v[0];
        case PropType::Int:
            return roundInt(value.v[0], defaultInt(spec));
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
        case PropType::IndexList:
            return indicesFromMask(maskOf(value.v[0]));
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
            v.v[0] = editor::jsonNumber(obj, key, 0.0);
            break;
        case PropType::Int:
            if (!j.is_number()) return false;
            v.v[0] = roundInt(editor::jsonNumber(obj, key, 0.0), defaultInt(spec));
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
                if (!editor::parseColorText(editor::jsonString(obj, key, ""), v.v)) return false;
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
            return parseValue(spec, editor::jsonString(obj, key, ""), out);
        case PropType::Text:
        case PropType::Name:
        case PropType::Path:
            if (!j.is_string()) return false;
            v.s = editor::jsonString(obj, key, "");
            break;
        case PropType::IndexList: {
            if (!j.is_array()) return false;
            std::vector<int> indices;
            for (const auto& e : j) {
                if (!e.is_number()) return false;
                indices.push_back(roundInt(e.get<double>(), -1));
            }
            v.v[0] = double(indexMaskFromList(indices));
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
                value.v[k] = editor::clampTerrainNumber(value.v[k], spec.lo, spec.hi,
                                                        spec.def.v[k]);
            }
            break;
        case PropType::Euler:
            for (std::size_t k = 0; k < 3; ++k) value.v[k] = editor::clampAngleDegrees(value.v[k]);
            break;
        case PropType::Int:
            value.v[0] = roundInt(
                editor::clampTerrainNumber(value.v[0], spec.lo, spec.hi, spec.def.v[0]),
                defaultInt(spec));
            break;
        case PropType::Bool:
            value.v[0] = (value.v[0] != 0.0 && value.v[0] == value.v[0]) ? 1.0 : 0.0;
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
            const bool allowed = value.s.empty() || editor::assetFileAllowed(value.s);
            if (tooLong || !allowed) value.s = spec.def.s;
            break;
        }
        case PropType::IndexList: {
            std::vector<int> kept;
            for (const int k : indicesFromMask(maskOf(value.v[0]))) {
                const bool inRange = double(k) >= spec.lo && double(k) <= spec.hi;
                if (inRange) kept.push_back(k);
            }
            value.v[0] = double(indexMaskFromList(kept));
            break;
        }
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
    if (spec.type == PropType::Int) v.v[0] = roundInt(v.v[0], roundInt(a.v[0], 0));
    return v;
}

bool sameValue(const ValueSpec& spec, const PropValue& a, const PropValue& b) {
    if (isString(spec.type)) return a.s == b.s;
    for (std::size_t k = 0; k < componentCount(spec.type); ++k) {
        if (a.v[k] != b.v[k]) return false;
    }
    return true;
}

}  // namespace ui
}  // namespace wizengine
