#include "ui/UiProps.h"

#include "ui/UiMeshFormat.h"  // 縁取りの上限（kSdfOutlineMaxEm）

#include <algorithm>
#include <utility>

namespace wizengine {
namespace ui {
namespace {

// ---- 性質の表 ----------------------------------------------------------------
// 並びは Prop の順（propInfo が添字で引く。tests/ui が id との一致を検査する）。
// 位置・大きさの上限（設計上の画素。基準の高さ 1080 に対して十分に広い）。
constexpr double kCoordMax = 1.0e6;
// 文字列の上限（バイト）。
constexpr double kTextMaxBytes = 4096.0;
constexpr double kPathMaxBytes = 260.0;

constexpr KindMask kTextual = kKindText | kKindButton;

const PropInfo kProps[] = {
    {Prop::Anchor, "anchor", {PropType::Rect, numberValue(0, 0, 0, 0), 0.0, 1.0}, true, kKindAll, "アンカー"},
    {Prop::Pivot, "pivot", {PropType::Vec2, numberValue(0, 0), 0.0, 1.0}, true, kKindAll, "ピボット"},
    {Prop::Pos, "pos", {PropType::Vec2, numberValue(0, 0), -kCoordMax, kCoordMax}, true, kKindAll, "位置"},
    {Prop::Size, "size", {PropType::Vec2, numberValue(100, 100), -kCoordMax, kCoordMax}, true, kKindAll, "大きさ"},
    {Prop::Color, "color", {PropType::Color, numberValue(1, 1, 1, 1), 0.0, 1.0}, true, kKindAll, "色"},
    {Prop::Alpha, "alpha", {PropType::Number, numberValue(1), 0.0, 1.0}, true, kKindAll, "不透明度"},
    {Prop::Visible, "visible", {PropType::Bool, numberValue(1), 0.0, 1.0}, true, kKindAll, "表示"},
    {Prop::Text, "text", {PropType::Text, textValue(""), 0.0, kTextMaxBytes}, true, kTextual, "文字"},
    {Prop::Font, "font", {PropType::Name, textValue(""), 0.0, 0.0}, false, kTextual, "フォント"},
    {Prop::FontSize, "fontSize", {PropType::Number, numberValue(32), 1.0, 1024.0}, true, kTextual, "文字の大きさ"},
    {Prop::Align, "align", {PropType::Enum, numberValue(0), 0.0, 0.0, kAlignNames}, false, kTextual, "揃え"},
    // 縁取りの幅は em（字の大きさ）に対する割合。上限は字の SDF のにじみ幅で決まる。
    {Prop::Outline, "outline", {PropType::Number, numberValue(0), 0.0, kSdfOutlineMaxEm}, true, kTextual, "縁取り"},
    {Prop::OutlineColor, "outlineColor", {PropType::Color, numberValue(0, 0, 0, 1), 0.0, 1.0}, true, kTextual, "縁取りの色"},
    {Prop::Wrap, "wrap", {PropType::Bool, numberValue(0), 0.0, 1.0}, false, kTextual, "折り返し"},
    {Prop::File, "file", {PropType::Path, textValue(""), 0.0, kPathMaxBytes}, false, kKindImage | kKindButton, "画像"},
    {Prop::Hover, "hover", {PropType::Color, numberValue(1, 1, 1, 1), 0.0, 1.0}, false, kKindButton, "ホバー時の色"},
    {Prop::Pressed, "pressed", {PropType::Color, numberValue(1, 1, 1, 1), 0.0, 1.0}, false, kKindButton, "押下時の色"},
    {Prop::Enabled, "enabled", {PropType::Bool, numberValue(1), 0.0, 1.0}, false, kKindButton, "有効"},
};
static_assert(sizeof(kProps) / sizeof(kProps[0]) == kPropCount,
              "kProps must have one row per Prop");

// 大文字小文字を無視した編集距離。
std::size_t editDistanceIcase(const std::string& a, const std::string& b) {
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    };
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

}  // namespace

// ---- 性質の表を引く ----------------------------------------------------------
const PropInfo& propInfo(Prop id) {
    return kProps[static_cast<std::size_t>(id) % kPropCount];
}

PropMask propsOfKind(KindMask kind) {
    PropMask mask = 0;
    for (const auto& p : kProps) {
        if (p.kinds & kind) mask |= propBit(p.id);
    }
    return mask;
}

const char* nearestName(const std::string& name, const char* const* names,
                        std::size_t count) {
    if (name.empty()) return nullptr;
    // 短い名前ほど打ち間違いの許し幅を狭める（"pos" に 2 文字違いを許すと
    // 無関係な名前まで候補になる）。
    const std::size_t maxDistance = name.size() <= 3 ? 1 : 2;
    const char* best = nullptr;
    std::size_t bestDistance = maxDistance + 1;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t d = editDistanceIcase(name, names[k]);
        if (d < bestDistance) {
            best = names[k];
            bestDistance = d;
        }
    }
    return best;
}

const PropInfo* propFromName(const std::string& name, const PropInfo** suggestion) {
    if (suggestion) *suggestion = nullptr;
    for (const auto& p : kProps) {
        if (name == p.name) return &p;
    }
    if (suggestion) {
        const char* names[kPropCount];
        for (std::size_t k = 0; k < kPropCount; ++k) names[k] = kProps[k].name;
        const char* near = nearestName(name, names, kPropCount);
        for (const auto& p : kProps) {
            if (near == p.name) *suggestion = &p;
        }
    }
    return nullptr;
}

// ---- PropSet -----------------------------------------------------------------
const PropValue& PropSet::get(Prop p) const {
    return has(p) ? v_[index(p)] : propInfo(p).spec.def;
}

void PropSet::set(Prop p, PropValue value) {
    v_[index(p)] = std::move(value);
    set_.set(index(p));
}

void PropSet::setNumber(Prop p, double v0, double v1, double v2, double v3) {
    set(p, numberValue(v0, v1, v2, v3));
}

void PropSet::setText(Prop p, std::string s) { set(p, textValue(std::move(s))); }

void PropSet::clear(Prop p) {
    v_[index(p)] = PropValue{};
    set_.reset(index(p));
}

void PropSet::overlay(const PropSet& upper) {
    for (std::size_t k = 0; k < kPropCount; ++k) {
        if (upper.set_.test(k)) {
            v_[k] = upper.v_[k];
            set_.set(k);
        }
    }
}

bool PropSet::parse(Prop p, const std::string& text) {
    PropValue v;
    if (!parseValue(propInfo(p).spec, text, v)) return false;
    set(p, std::move(v));
    return true;
}

std::string PropSet::format(Prop p) const {
    return formatValue(propInfo(p).spec, get(p));
}

nlohmann::json toJson(const PropSet& props) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& info : kProps) {
        if (props.has(info.id)) j[info.name] = valueToJson(info.spec, props.get(info.id));
    }
    return j;
}

std::vector<std::string> fromJson(const nlohmann::json& j, PropSet& props) {
    std::vector<std::string> rejected;
    if (!j.is_object()) return rejected;
    for (const auto& info : kProps) {
        if (!j.contains(info.name)) continue;
        PropValue v;
        if (valueFromJson(info.spec, j, info.name, v)) {
            props.set(info.id, std::move(v));
        } else {
            rejected.emplace_back(info.name);
        }
    }
    return rejected;
}

PropSet clampProps(PropSet props) {
    for (const auto& info : kProps) {
        if (props.has(info.id)) props.set(info.id, clampValue(info.spec, props.get(info.id)));
    }
    return props;
}

PropSet lerpProps(const PropSet& a, const PropSet& b, double t) {
    PropSet out = a;
    for (const auto& info : kProps) {
        if (!b.has(info.id)) continue;
        if (a.has(info.id)) {
            out.set(info.id, lerpValue(info.spec, a.get(info.id), b.get(info.id), t));
        } else {
            out.set(info.id, b.get(info.id));
        }
    }
    return out;
}

PropSet resolve(const PropSet& design, const PropSet& anim, const PropSet& node,
                const PropSet& pointer) {
    PropSet out = design;
    out.overlay(anim);
    out.overlay(node);
    out.overlay(pointer);
    return out;
}

}  // namespace ui
}  // namespace wizengine
