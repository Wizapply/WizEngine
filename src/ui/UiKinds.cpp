#include "ui/UiKinds.h"

namespace wizengine {
namespace ui {
namespace {

// 種類の表（並び = 「もしかして」の候補の順）。PR3 で kButtonKind を足す。
const ElementKindInfo* const kKinds[] = {&kTextKind, &kImageKind};
constexpr std::size_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);

}  // namespace

const ElementKindInfo* kindInfo(const std::string& name) {
    for (const ElementKindInfo* k : kKinds) {
        if (name == k->name) return k;
    }
    return nullptr;
}

std::size_t kindCount() { return kKindCount; }

const ElementKindInfo& kindAt(std::size_t index) { return *kKinds[index % kKindCount]; }

void elementColor(const ResolvedElement& e, Prop colorProp, float out[4]) {
    const PropValue& c = e.props.get(colorProp);
    out[0] = float(c.v[0]);
    out[1] = float(c.v[1]);
    out[2] = float(c.v[2]);
    out[3] = float(c.v[3] * e.alpha);
}

}  // namespace ui
}  // namespace wizengine
