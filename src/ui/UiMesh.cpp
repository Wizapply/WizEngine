#include "ui/UiMesh.h"

#include "ui/FontProvider.h"
#include "ui/UiKinds.h"

namespace wizengine {
namespace ui {

void MeshSink::quad(const UiQuad& q, const Rect& element) {
    const auto first = static_cast<std::uint32_t>(out_.vertices.size());
    const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};  // 左上から時計回り
    for (const auto& c : corners) {
        UiVertex v;
        v.x = float(q.rect.x + q.rect.w * c[0]);
        v.y = float(q.rect.y + q.rect.h * c[1]);
        v.u0 = q.u0 + (q.u1 - q.u0) * c[0];
        v.v0 = q.v0 + (q.v1 - q.v0) * c[1];
        v.u1 = element.w > 0.0 ? float((v.x - element.x) / element.w) : 0.0f;
        v.v1 = element.h > 0.0 ? float((v.y - element.y) / element.h) : 0.0f;
        v.r = q.color[0];
        v.g = q.color[1];
        v.b = q.color[2];
        v.a = q.color[3];
        v.outline = q.outline;
        v.outlineR = q.outlineColor[0];
        v.outlineG = q.outlineColor[1];
        v.outlineB = q.outlineColor[2];
        v.outlineA = q.outlineColor[3];
        out_.vertices.push_back(v);
    }
    const auto firstIndex = static_cast<std::uint32_t>(out_.indices.size());
    for (const std::uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) out_.indices.push_back(first + k);
    // 前の run と同じテクスチャ・材質なら伸ばす（描画の回数を減らす）。
    const bool continues = !out_.runs.empty() && out_.runs.back().textureId == q.textureId &&
                           out_.runs.back().materialId == q.materialId;
    if (continues) {
        out_.runs.back().indexCount += 6;
    } else {
        out_.runs.push_back({q.textureId, q.materialId, firstIndex, 6});
    }
}

int MeshSink::fontPageTexture(int page) const {
    return textures_.fontPage ? textures_.fontPage(page) : kUiNoTexture;
}

int MeshSink::imageTexture(const std::string& file) const {
    return textures_.image ? textures_.image(file) : kUiNoTexture;
}

UiMeshData buildCanvasMesh(const std::vector<ElementDesc>& elements,
                           const std::vector<PropSet>& resolved, const Rect& canvas,
                           FontProvider& fonts, const UiTextureLookup& textures) {
    UiMeshData out;
    MeshSink sink(out, textures);
    const std::vector<LayoutNode> nodes = layoutTree(elements, resolved, canvas);
    std::vector<double> alpha(elements.size(), 1.0);
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const PropSet& props = elementProps(elements, resolved, i);
        // 親は前にあるので、不透明度はもう積まれている。
        const double parentAlpha = nodes[i].parent >= 0 ? alpha[std::size_t(nodes[i].parent)] : 1.0;
        alpha[i] = parentAlpha * props.number(Prop::Alpha);
        if (!nodes[i].visible) continue;
        const ResolvedElement e{elements[i], props, nodes[i].rect, alpha[i]};
        const ElementKindInfo* kind = kindInfo(elements[i].kind);
        if (kind && kind->build) kind->build(e, fonts, sink);
    }
    return out;
}

}  // namespace ui
}  // namespace wizengine
