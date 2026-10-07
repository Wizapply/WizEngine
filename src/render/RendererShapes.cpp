// Renderer の形状スロット・ソフト形状・色と材質・ハイライト。
#include "render/RendererInternal.h"

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

namespace wizengine {

// 材質をマテリアルインスタンスへ。自己発光は baseColor を使い回すのではなく
// 白で足す（色は baseColor 側で付く）。w = 1 は「露出の影響を受ける」の意味で、
// これを立てておかないと露出を変えたときに発光だけ取り残される。
void Renderer::applyMaterialParams(filament::MaterialInstance* mi,
                                   const ShapeMaterial& m) const {
    if (!mi) return;
    mi->setParameter("roughness", m.roughness);
    mi->setParameter("metallic", m.metallic);
    mi->setParameter("reflectance", m.reflectance);
    mi->setParameter("clearCoat", m.clearCoat);
    mi->setParameter("clearCoatRoughness", m.clearCoatRoughness);
    mi->setParameter("emissiveScale", m.emissive);
}

std::size_t Renderer::addShape(ShapeMesh mesh) {
    if (mesh == ShapeMesh::Sphere) ensureSphereMesh();
    if (mesh == ShapeMesh::Cylinder) ensureCylinderMesh();

    // 空きスロットがあれば再利用。エディタで置いては消してを繰り返しても
    // エンティティ番号が無限に伸びない。
    std::size_t id;
    if (!freeShapes_.empty()) {
        id = freeShapes_.back();
        freeShapes_.pop_back();
    } else {
        shapes_.push_back(ShapeSlot{});
        id = shapes_.size() - 1;
    }

    ShapeSlot& slot = shapes_[id];
    slot.mi = nullptr;      // 色は共有インスタンス（setShapeColor で個別化）
    slot.highlight = -1;
    slot.used = true;
    // 前にこの席を使っていた物の色と材質を引き継がない。
    slot.color = float3{0.80f, 0.36f, 0.18f};
    slot.material = ShapeMaterial{};

    VertexBuffer* meshVb = vb_;
    IndexBuffer* meshIb = ib_;
    uint32_t meshIndices = 36;
    if (mesh == ShapeMesh::Sphere) {
        meshVb = sphereVb_;
        meshIb = sphereIb_;
        meshIndices = sphereIndexCount_;
    } else if (mesh == ShapeMesh::Cylinder) {
        meshVb = cylinderVb_;
        meshIb = cylinderIb_;
        meshIndices = cylinderIndexCount_;
    }
    slot.entity = EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {1, 1, 1}})
        .material(0, matInstance_)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, meshVb, meshIb,
                  0, meshIndices)
        .culling(true)  // frustum-cull off-screen boxes (matters at high counts)
        .castShadows(true)
        .receiveShadows(true)
        .build(*engine_, slot.entity);
    scene_->addEntity(slot.entity);
    return id;
}

void Renderer::removeShape(std::size_t id) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    // 番号は空きリストへ戻すので、次の addShape が同じスロットを使う。
    ShapeSlot& slot = shapes_[id];
    scene_->remove(slot.entity);
    engine_->destroy(slot.entity);
    utils::EntityManager::get().destroy(slot.entity);
    slot.entity = utils::Entity();
    if (slot.mi) {
        engine_->destroy(slot.mi);
        slot.mi = nullptr;
    }
    // ソフトボディの自前バッファ（Filament の destroy は GPU が使い終わる
    // まで遅延するので、前フレームが参照していても構わない）。
    if (slot.vb) {
        engine_->destroy(slot.vb);
        slot.vb = nullptr;
    }
    if (slot.ib) {
        engine_->destroy(slot.ib);
        slot.ib = nullptr;
    }
    slot.vertexCount = 0;
    slot.highlight = -1;
    slot.used = false;
    freeShapes_.push_back(id);
}

std::size_t Renderer::addSoftShape(std::size_t vertexCount,
                                   const std::vector<uint32_t>& indices) {
    std::size_t id;
    if (!freeShapes_.empty()) {
        id = freeShapes_.back();
        freeShapes_.pop_back();
    } else {
        shapes_.push_back(ShapeSlot{});
        id = shapes_.size() - 1;
    }
    ShapeSlot& slot = shapes_[id];
    // 変形メッシュは両面を描く（法線は頂点から作るので、たまたま裏を向いた
    // 三角形や、まだ閉じていない面が背面カリングで抜けないように）。専用の
    // マテリアルインスタンスをここで作っておけば setShapeColor はそれを使う。
    slot.mi = material_->createInstance();
    slot.mi->setParameter("baseColor", RgbType::LINEAR, float3{0.80f, 0.36f, 0.18f});
    slot.mi->setCullingMode(MaterialInstance::CullingMode::NONE);
    slot.highlight = -1;
    slot.used = true;
    slot.vertexCount = uint32_t(std::max<std::size_t>(vertexCount, 1));
    const std::size_t indexCount = std::max<std::size_t>(indices.size(), 3);

    // 頂点は毎フレーム setSoftShapeVertices が書き換える。最初は原点に
    // 潰しておく（未初期化のバッファを描かせない）。
    slot.vb = VertexBuffer::Builder()
                  .vertexCount(slot.vertexCount)
                  .bufferCount(2)
                  .attribute(VertexAttribute::POSITION, 0,
                             VertexBuffer::AttributeType::FLOAT3)
                  .attribute(VertexAttribute::TANGENTS, 1,
                             VertexBuffer::AttributeType::FLOAT4)
                  .build(*engine_);
    {
        auto* pos = new float3[slot.vertexCount];
        auto* tan = new quatf[slot.vertexCount];
        for (uint32_t i = 0; i < slot.vertexCount; ++i) {
            pos[i] = float3{0.0f};
            tan[i] = quatf{1.0f, 0.0f, 0.0f, 0.0f};  // (w, x, y, z) = 単位
        }
        slot.vb->setBufferAt(
            *engine_, 0,
            VertexBuffer::BufferDescriptor(
                pos, sizeof(float3) * slot.vertexCount,
                [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));
        slot.vb->setBufferAt(
            *engine_, 1,
            VertexBuffer::BufferDescriptor(
                tan, sizeof(quatf) * slot.vertexCount,
                [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));
    }
    // インデックスは固定（表面の位相は変わらない）。頂点数は小さいが
    // 一般性のために 32 ビットにしておく。
    {
        auto* idx = new uint32_t[indexCount];
        for (std::size_t i = 0; i < indexCount; ++i) {
            idx[i] = i < indices.size() ? std::min<uint32_t>(indices[i], slot.vertexCount - 1)
                                        : 0u;
        }
        slot.ib = IndexBuffer::Builder()
                      .indexCount(uint32_t(indexCount))
                      .bufferType(IndexBuffer::IndexType::UINT)
                      .build(*engine_);
        slot.ib->setBuffer(
            *engine_,
            IndexBuffer::BufferDescriptor(
                idx, sizeof(uint32_t) * indexCount,
                [](void* p, size_t, void*) { delete[] static_cast<uint32_t*>(p); }));
    }

    slot.entity = EntityManager::get().create();
    // フラスタムカリングは切る: 頂点は毎フレーム書き換わり、AABB も追いかけて
    // 更新するが、車輪のように速く動く物が境界で一瞬消えるのを避ける
    // （数個の物なので描画コストの差は無い。AABB は影の範囲のために保つ）。
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {1, 1, 1}})  // setSoftShapeVertices が毎回更新
        .material(0, slot.mi)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, slot.vb, slot.ib,
                  0, indexCount)
        .culling(false)
        .castShadows(true)
        .receiveShadows(true)
        .build(*engine_, slot.entity);
    scene_->addEntity(slot.entity);
    return id;
}

void Renderer::setSoftShapeVertices(std::size_t id, const float* positions,
                                    const float* normals, std::size_t vertexCount) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    ShapeSlot& slot = shapes_[id];
    if (!slot.vb || !positions || !normals) return;
    const std::size_t n = slot.vertexCount;
    if (vertexCount != n) return;  // 位相が変わったら作り直す約束（Scene 側）

    // 位置。バッファは Filament が非同期にコピーするのでヒープに置き、
    // 完了コールバックで解放する（線バッチと同じ流儀）。
    auto* pos = new float3[n];
    float3 lo{1e30f}, hi{-1e30f};
    for (std::size_t i = 0; i < n; ++i) {
        pos[i] = float3{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
        lo = min(lo, pos[i]);
        hi = max(hi, pos[i]);
    }
    slot.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            pos, sizeof(float3) * n,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));

    // 法線 → 接空間の四元数（lit マテリアルは TANGENTS で法線を受ける）。
    std::vector<float3> nrm(n);
    for (std::size_t i = 0; i < n; ++i) {
        nrm[i] = float3{normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]};
    }
    auto* tan = new quatf[n];
    auto* orient = filament::geometry::SurfaceOrientation::Builder()
                       .vertexCount(n)
                       .normals(nrm.data())
                       .build();
    orient->getQuats(tan, n);
    delete orient;
    slot.vb->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(
            tan, sizeof(quatf) * n,
            [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));

    // バウンディングボックスは頂点から（影の範囲とカリングがこれを見る）。
    auto& rm = engine_->getRenderableManager();
    const float3 center = (lo + hi) * 0.5f;
    const float3 half = max((hi - lo) * 0.5f, float3{0.01f});
    rm.setAxisAlignedBoundingBox(rm.getInstance(slot.entity),
                                 filament::Box{center, half});
}

filament::MaterialInstance* Renderer::ensureShapeInstance(ShapeSlot& slot) {
    if (!slot.mi) {
        slot.mi = material_->createInstance();
        slot.mi->setParameter("baseColor", RgbType::LINEAR, slot.color);
        applyMaterialParams(slot.mi, slot.material);
    }
    return slot.mi;
}

void Renderer::setShapeColor(std::size_t id, const float3& color) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    ShapeSlot& slot = shapes_[id];
    slot.color = color;
    ensureShapeInstance(slot)->setParameter("baseColor", RgbType::LINEAR, color);
    // ハイライト中なら、掴んでいる色を上書きしない（離したときに戻る）。
    if (slot.highlight >= 0) return;
    auto& rm = engine_->getRenderableManager();
    rm.setMaterialInstanceAt(rm.getInstance(slot.entity), 0, slot.mi);
}

void Renderer::setShapeMaterial(std::size_t id, const ShapeMaterial& material) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    ShapeSlot& slot = shapes_[id];
    slot.material = material;
    applyMaterialParams(ensureShapeInstance(slot), material);
    if (slot.highlight >= 0) return;
    auto& rm = engine_->getRenderableManager();
    rm.setMaterialInstanceAt(rm.getInstance(slot.entity), 0, slot.mi);
}

void Renderer::configureHighlightColors(
    const std::vector<filament::math::float3>& colors) {
    for (auto* mi : highlightInstances_) engine_->destroy(mi);
    highlightInstances_.clear();
    // One instance per camera: same material, different baseColor. Swapping a
    // box between them is just a parameter change for the renderer.
    for (const auto& c : colors) {
        auto* mi = material_->createInstance();
        mi->setParameter("baseColor", RgbType::LINEAR, c);
        applyMaterialParams(mi, ShapeMaterial{});
        highlightInstances_.push_back(mi);
    }
}

void Renderer::setBoxHighlighted(std::size_t id, int styleIndex) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    ShapeSlot& slot = shapes_[id];
    if (styleIndex >= int(highlightInstances_.size())) styleIndex = -1;
    if (slot.highlight == styleIndex) return;
    slot.highlight = styleIndex;

    // 解除したときは、そのオブジェクト自身の色（無ければ共有色）に戻す。
    MaterialInstance* mi = (styleIndex < 0)
                               ? (slot.mi ? slot.mi : matInstance_)
                               : highlightInstances_[styleIndex];
    auto& rm = engine_->getRenderableManager();
    rm.setMaterialInstanceAt(rm.getInstance(slot.entity), 0, mi);
}

void Renderer::setBoxTransform(std::size_t i, const filament::math::mat4f& transform) {
    if (i >= shapes_.size() || !shapes_[i].used) return;
    auto& tcm = engine_->getTransformManager();
    tcm.setTransform(tcm.getInstance(shapes_[i].entity), transform);
}

}  // namespace wizengine
