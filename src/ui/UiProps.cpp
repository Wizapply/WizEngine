#include "ui/UiProps.h"

#include "ui/UiSdfParams.h"  // 縁取りの上限（kSdfOutlineMaxEm）

#include <algorithm>
#include <cassert>
#include <utility>

#include "core/NearestName.h"

namespace wizengine {
namespace ui {
namespace {

// ---- 性質の表 ----------------------------------------------------------------
// 並びは Prop の順（propInfo が添字で引く。tests/ui が id との一致を検査する）。
// 位置・大きさの上限（設計上の画素。基準の高さ 1080 に対して十分に広い）。
constexpr double kCoordMax = 1.0e6;
// パスの上限（バイト）。Windows の従来のパスの長さ（MAX_PATH = 260）に合わせる:
// assets/ からの相対パスなので実際にはずっと短く、これを超えるのは誤りか悪意の値。
constexpr double kPathMaxBytes = 260.0;

constexpr KindMask kTextual = kKindText | kKindButton;

// hover / pressed を書いていないボタンの色の作り方（pointerColor の唯一の定数）: 背景の
// color を白へ 25% 寄せる（明るく）・黒へ 25% 寄せる（暗く）。
constexpr double kHoverLighten = 0.25;
constexpr double kPressedDarken = 0.25;

const PropInfo kProps[] = {
    {Prop::Anchor, "anchor", {PropType::Rect, numberValue(0, 0, 0, 0), 0.0, 1.0}, true, kKindAll, "アンカー"},
    {Prop::Pivot, "pivot", {PropType::Vec2, numberValue(0, 0), 0.0, 1.0}, true, kKindAll, "ピボット"},
    {Prop::Pos, "pos", {PropType::Vec2, numberValue(0, 0), -kCoordMax, kCoordMax}, true, kKindAll, "位置"},
    {Prop::Size, "size", {PropType::Vec2, numberValue(100, 100), -kCoordMax, kCoordMax}, true, kKindAll, "大きさ"},
    {Prop::Color, "color", {PropType::Color, numberValue(1, 1, 1, 1), 0.0, 1.0}, true, kKindAll, "色"},
    {Prop::Alpha, "alpha", {PropType::Number, numberValue(1), 0.0, 1.0}, true, kKindAll, "不透明度"},
    {Prop::Visible, "visible", {PropType::Bool, numberValue(1), 0.0, 1.0}, true, kKindAll, "表示"},
    {Prop::Text, "text", {PropType::Text, textValue(""), 0.0, double(kMaxTextBytes)}, true, kTextual, "文字"},
    {Prop::Font, "font", {PropType::Name, textValue(""), 0.0, 0.0}, false, kTextual, "フォント"},
    {Prop::FontSize, "fontSize", {PropType::Number, numberValue(32), 1.0, 1024.0}, true, kTextual, "文字の大きさ"},
    {Prop::Align, "align", {PropType::Enum, numberValue(0), 0.0, 0.0, kAlignNames}, false, kTextual, "揃え"},
    // 縁取りの幅は em（字の大きさ）に対する割合。上限は字の SDF のにじみ幅で決まる。
    {Prop::Outline, "outline", {PropType::Number, numberValue(0), 0.0, kSdfOutlineMaxEm}, true, kTextual, "縁取り"},
    {Prop::OutlineColor, "outlineColor", {PropType::Color, numberValue(0, 0, 0, 1), 0.0, 1.0}, true, kTextual, "縁取りの色"},
    {Prop::Wrap, "wrap", {PropType::Bool, numberValue(0), 0.0, 1.0}, false, kTextual, "折り返し"},
    // ボタンの字の色（ボタンの color は背景の色）。
    {Prop::TextColor, "textColor", {PropType::Color, numberValue(0.05, 0.05, 0.05, 1), 0.0, 1.0}, true, kKindButton, "文字の色"},
    {Prop::File, "file", {PropType::Path, textValue(""), 0.0, kPathMaxBytes}, false, kKindImage | kKindButton, "画像"},
    // ポインタが上にある間・押している間の背景の色（color を置き換える = resolve）。
    // 書かなければ背景の color から作る（pointerColor）ので、この行の既定値（白）は使わない
    // （defaultIsDerived: 保存は既定値と比べない・schema の既定値は null・ブラウザには要素ごとの
    // 「既定」= 背景から作った色を送る = scene/UiJson）。値の型を決めるためだけに置いてある。
    {Prop::Hover, "hover", {PropType::Color, numberValue(1, 1, 1, 1), 0.0, 1.0}, false, kKindButton, "ホバー時の色（既定: 背景を明るく）"},
    {Prop::Pressed, "pressed", {PropType::Color, numberValue(1, 1, 1, 1), 0.0, 1.0}, false, kKindButton, "押下時の色（既定: 背景を暗く）"},
    {Prop::Enabled, "enabled", {PropType::Bool, numberValue(1), 0.0, 1.0}, false, kKindButton, "有効"},
};
static_assert(sizeof(kProps) / sizeof(kProps[0]) == kPropCount,
              "kProps must have one row per Prop");

}  // namespace

// ---- 性質の表を引く ----------------------------------------------------------
const PropInfo& propInfo(Prop id) {
    const std::size_t index = static_cast<std::size_t>(id);
    assert(index < kPropCount && "propInfo: id out of range");
    return kProps[index < kPropCount ? index : 0];
}

PropMask propsOfKind(KindMask kind) {
    PropMask mask = 0;
    for (const auto& p : kProps) {
        if (p.kinds & kind) mask |= propBit(p.id);
    }
    return mask;
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
                PointerPhase pointer) {
    PropSet out = design;
    out.overlay(anim);
    out.overlay(node);
    if (pointer != PointerPhase::None && out.flag(Prop::Enabled)) {
        out.set(Prop::Color, pointerColor(out, pointer));
    }
    return out;
}

bool defaultIsDerived(Prop p) {
    for (const auto& c : kPointerColorProps) {
        if (c.prop == p) return true;
    }
    return false;
}

PropValue pointerColor(const PropSet& props, PointerPhase phase) {
    const PropValue& base = props.get(Prop::Color);
    const PointerColorProp* entry = nullptr;
    for (const auto& c : kPointerColorProps) {
        if (c.phase == phase) entry = &c;
    }
    if (!entry) return base;  // None
    if (props.has(entry->prop)) return props.get(entry->prop);
    // 書いていなければ背景から: 明るくは白へ寄せ、暗くは黒へ寄せる（不透明度はそのまま）。
    PropValue out = base;
    for (std::size_t k = 0; k < 3; ++k) {
        out.v[k] = phase == PointerPhase::Hover ? base.v[k] + (1.0 - base.v[k]) * kHoverLighten
                                                : base.v[k] * (1.0 - kPressedDarken);
    }
    return out;
}

}  // namespace ui
}  // namespace wizengine
