// 種類 image: 要素の矩形に四角 1 枚（画像のパスが空なら白 = 色の板）。
#include "ui/UiImage.h"

#include "ui/UiKinds.h"
#include "ui/UiMesh.h"

namespace wizengine {
namespace ui {

void addImageQuad(const ResolvedElement& e, MeshSink& sink) {
    UiQuad q;
    q.rect = e.rect;
    elementColor(e, Prop::Color, q.color);
    const std::string& file = e.props.text(Prop::File);
    q.textureId = file.empty() ? kUiNoTexture : sink.imageTexture(file);
    q.materialId = kUiMaterialImage;
    sink.quad(q, e.rect);
}

namespace {

void buildImage(const ResolvedElement& e, FontProvider&, MeshSink& sink) { addImageQuad(e, sink); }

}  // namespace

const ElementKindInfo kImageKind = {"image", "▣", kKindImage, Interaction::None, &buildImage};

}  // namespace ui
}  // namespace wizengine
