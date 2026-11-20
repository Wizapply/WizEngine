// 種類 button: 背景の四角（image と同じ = color と file）と、その上の字（text と同じ並べ方。
// 字の色は textColor、行の塊は縦に中央）。押せる（Interaction::Click）: ポインタの上・押下の
// 色（hover / pressed）は重ね順の最後の段（ui::resolve の pointer）が color に重ねる。
#include "ui/UiImage.h"
#include "ui/UiKinds.h"
#include "ui/UiText.h"

namespace wizengine {
namespace ui {
namespace {

void buildButton(const ResolvedElement& e, FontProvider& fonts, MeshSink& sink) {
    addImageQuad(e, sink);
    addTextQuads(e, Prop::TextColor, true, fonts, sink);
}

}  // namespace

const ElementKindInfo kButtonKind = {"button", "▢", kKindButton, Interaction::Click,
                                     &buildButton};

}  // namespace ui
}  // namespace wizengine
