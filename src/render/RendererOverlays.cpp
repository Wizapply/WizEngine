// Renderer のオーバーレイ（エディタ専用レイヤに出す頂点色つき半透明メッシュ）。
#include "render/RendererInternal.h"

using namespace filament;
using namespace filament::math;
using namespace render_detail;

namespace wizengine {

namespace {

// USHORT のインデックスで指せる頂点数の上限。
constexpr std::size_t kMaxOverlayVertices = 65536;

// 頂点・色・インデックスの組が描ける形か。だめなら理由（ログ用）を返す。
const char* overlayMeshProblem(const std::vector<float3>& positions,
                               const std::vector<float4>& colors,
                               const std::vector<uint16_t>& indices) {
    if (colors.size() != positions.size()) return "colors and positions differ in count";
    if (positions.size() > kMaxOverlayVertices) return "too many vertices for 16-bit indices";
    if (indices.size() % 3 != 0) return "index count is not a multiple of 3";
    for (uint16_t i : indices) {
        if (i >= positions.size()) return "an index is out of range";
    }
    return nullptr;
}

}  // namespace

void Renderer::loadOverlayMaterial() {
    overlayMaterial_ =
        loadMaterialPackage(*engine_, "overlay.filamat", "overlay material");
}

std::size_t Renderer::addOverlayMesh() {
    OverlayMesh mesh;
    // 色は頂点が持つので、インスタンスにパラメータは無い。
    mesh.mi = overlayMaterial_->createInstance();
    // エンティティとバッファは最初の setOverlayMesh で数が決まってから作る。
    overlayMeshes_.push_back(mesh);
    return overlayMeshes_.size() - 1;
}

void Renderer::setOverlayMesh(std::size_t id,
                              const std::vector<filament::math::float3>& positions,
                              const std::vector<filament::math::float4>& colors,
                              const std::vector<uint16_t>& indices) {
    if (id >= overlayMeshes_.size()) return;
    OverlayMesh& mesh = overlayMeshes_[id];

    // 空は非表示（約束どおり）。形が壊れているときも、壊れたバッファを
    // 送らずに非表示にする - 呼び手の不具合なので 1 回だけ知らせる。
    const char* problem = nullptr;
    if (!positions.empty() && !indices.empty()) {
        problem = overlayMeshProblem(positions, colors, indices);
        if (problem && !overlayWarned_) {
            LOGW("render", "overlay mesh %zu hidden: %s", id, problem);
            overlayWarned_ = true;
        }
    }
    if (positions.empty() || indices.empty() || problem) {
        if (mesh.inScene) {
            scene_->remove(mesh.entity);
            mesh.inScene = false;
        }
        return;
    }

    const std::size_t vertexCount = positions.size();
    const std::size_t indexCount = indices.size();

    // 頂点数かインデックス数が変わったらレンダラブルごと作り直す（細線
    // セットと同じ理由: 実行中にプリミティブ数を変える API は版によって
    // 名前が違うので使わない）。
    if (vertexCount != mesh.vertexCount || indexCount != mesh.indexCount) {
        if (mesh.vertexCount > 0) {
            destroyRenderable(*engine_, *scene_, mesh.entity, mesh.vb, mesh.ib,
                              mesh.inScene);
        }
        mesh.vertexCount = vertexCount;
        mesh.indexCount = indexCount;

        // 位置（バッファ 0）と色（バッファ 1）を別々のバッファにする =
        // 呼び手の配列をそのまま写せる。
        mesh.vb = VertexBuffer::Builder()
                      .vertexCount(uint32_t(vertexCount))
                      .bufferCount(2)
                      .attribute(VertexAttribute::POSITION, 0,
                                 VertexBuffer::AttributeType::FLOAT3, 0,
                                 sizeof(float3))
                      .attribute(VertexAttribute::COLOR, 1,
                                 VertexBuffer::AttributeType::FLOAT4, 0,
                                 sizeof(float4))
                      .build(*engine_);
        mesh.ib = IndexBuffer::Builder()
                      .indexCount(uint32_t(indexCount))
                      .bufferType(IndexBuffer::IndexType::USHORT)
                      .build(*engine_);
        mesh.entity = buildEditorRenderable(
            *engine_, RenderableManager::PrimitiveType::TRIANGLES, mesh.vb,
            mesh.ib, indexCount, mesh.mi);
    }

    mesh.vb->setBufferAt(*engine_, 0,
                         copyToDescriptor(positions.data(), vertexCount));
    mesh.vb->setBufferAt(*engine_, 1,
                         copyToDescriptor(colors.data(), vertexCount));
    mesh.ib->setBuffer(*engine_, copyToDescriptor(indices.data(), indexCount));

    if (!mesh.inScene) {
        scene_->addEntity(mesh.entity);
        mesh.inScene = true;
    }
}

}  // namespace wizengine
