// Renderer の線（グラブ線・ジョイント線・太線バッチ・細線セット）。
#include "render/RendererInternal.h"

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

namespace {

// 太線 1 本ぶんの頂点数と三角形の頂点インデックス数。板 2 枚 = 4 三角形。
constexpr std::size_t kTubeVertices = 8;
constexpr std::size_t kTubeIndices = 12;

// 線分 a→b を「直交する 2 枚の板」に展開して out[0..7] へ書く。
// 板 1 は u 方向、板 2 は v 方向に幅を持つ。どちらか一方は必ずカメラに対して
// 開くので、ビューごとに作り直さなくても太く見える。
void buildTube(filament::math::float3* out, const filament::math::float3& a,
               const filament::math::float3& b, float half) {
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-9f || half <= 0.0f) {
        // 長さ 0 の線分（＝バッチの余り）は面積 0 の三角形にして消す。
        for (std::size_t i = 0; i < kTubeVertices; ++i) out[i] = a;
        return;
    }
    dx /= len;
    dy /= len;
    dz /= len;

    // 線と最も平行でない軸を基準に、直交する 2 方向を作る。
    const bool useY = std::abs(dy) < 0.9f;
    const float rx = useY ? 0.0f : 1.0f;
    const float ry = useY ? 1.0f : 0.0f;
    const float rz = 0.0f;

    float ux = dy * rz - dz * ry;
    float uy = dz * rx - dx * rz;
    float uz = dx * ry - dy * rx;
    const float ul = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (ul < 1e-9f) {
        for (std::size_t i = 0; i < kTubeVertices; ++i) out[i] = a;
        return;
    }
    ux = ux / ul * half;
    uy = uy / ul * half;
    uz = uz / ul * half;

    // v = dir x u（u と dir が直交かつ単位なので、これも長さ half になる）
    const float vx = dy * uz - dz * uy;
    const float vy = dz * ux - dx * uz;
    const float vz = dx * uy - dy * ux;

    out[0] = {a.x - ux, a.y - uy, a.z - uz};
    out[1] = {a.x + ux, a.y + uy, a.z + uz};
    out[2] = {b.x + ux, b.y + uy, b.z + uz};
    out[3] = {b.x - ux, b.y - uy, b.z - uz};
    out[4] = {a.x - vx, a.y - vy, a.z - vz};
    out[5] = {a.x + vx, a.y + vy, a.z + vz};
    out[6] = {b.x + vx, b.y + vy, b.z + vz};
    out[7] = {b.x - vx, b.y - vy, b.z - vz};
}

}  // namespace

namespace wizengine {

bool Renderer::ensureLineMaterial() {
    if (lineMaterial_) return true;
    const auto pkg = readFile(assetPath("line.filamat"));
    if (pkg.empty()) {
        LOGW("render", "line.filamat not found - lines disabled");
        return false;
    }
    lineMaterial_ =
        Material::Builder().package(pkg.data(), pkg.size()).build(*engine_);
    return lineMaterial_ != nullptr;
}

Renderer::LineEntity Renderer::createLine(const filament::math::float3& color) {
    LineEntity line;
    // Two vertices, rewritten every frame; the index buffer never changes.
    line.vb = VertexBuffer::Builder()
                  .vertexCount(2)
                  .bufferCount(1)
                  .attribute(VertexAttribute::POSITION, 0,
                             VertexBuffer::AttributeType::FLOAT3, 0,
                             sizeof(float) * 3)
                  .build(*engine_);
    static const uint16_t kIndices[2] = {0, 1};
    line.ib = IndexBuffer::Builder()
                  .indexCount(2)
                  .bufferType(IndexBuffer::IndexType::USHORT)
                  .build(*engine_);
    line.ib->setBuffer(
        *engine_,
        IndexBuffer::BufferDescriptor(kIndices, sizeof(kIndices), nullptr));

    line.mi = lineMaterial_->createInstance();
    line.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                          float4{color.x, color.y, color.z, 1.0f});

    line.entity = EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{-1000.0f, -1000.0f, -1000.0f},
                      {1000.0f, 1000.0f, 1000.0f}})  // never culled
        .geometry(0, RenderableManager::PrimitiveType::LINES, line.vb, line.ib,
                  0, 2)
        .material(0, line.mi)
        .culling(false)
        .castShadows(false)
        .receiveShadows(false)
        .build(*engine_, line.entity);
    return line;  // added to the scene only while visible
}

void Renderer::updateLine(LineEntity& line, const filament::math::float3& from,
                          const filament::math::float3& to, bool visible) {
    if (!visible) {
        if (line.inScene) {
            scene_->remove(line.entity);
            line.inScene = false;
        }
        return;
    }

    // The buffer must outlive the call: Filament copies it asynchronously, so
    // hand over heap memory and free it from the completion callback.
    auto* verts = new float[6]{from.x, from.y, from.z, to.x, to.y, to.z};
    line.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            verts, sizeof(float) * 6,
            [](void* buffer, size_t, void*) {
                delete[] static_cast<float*>(buffer);
            }));

    if (!line.inScene) {
        scene_->addEntity(line.entity);
        line.inScene = true;
    }
}

void Renderer::destroyLine(LineEntity& line) {
    if (line.inScene) scene_->remove(line.entity);
    engine_->destroy(line.entity);
    EntityManager::get().destroy(line.entity);
    engine_->destroy(line.vb);
    engine_->destroy(line.ib);
    engine_->destroy(line.mi);
    line = LineEntity{};
}

void Renderer::configureGrabLines(
    const std::vector<filament::math::float3>& colors) {
    if (!ensureLineMaterial()) return;
    for (const auto& c : colors) grabLines_.push_back(createLine(c));
}

void Renderer::setGrabLine(std::size_t index,
                           const filament::math::float3& from,
                           const filament::math::float3& to, bool visible) {
    if (index >= grabLines_.size()) return;
    updateLine(grabLines_[index], from, to, visible);
}

void Renderer::setJointLineCount(std::size_t count) {
    if (count == jointLines_.size()) return;
    if (count > jointLines_.size()) {
        if (!ensureLineMaterial()) return;
        while (jointLines_.size() < count) {
            // 色は setJointLine で毎回入れ直すので、ここでは白で作る。
            jointLines_.push_back(createLine({1.0f, 1.0f, 1.0f}));
        }
        return;
    }
    while (jointLines_.size() > count) {
        destroyLine(jointLines_.back());
        jointLines_.pop_back();
    }
}

void Renderer::setJointLine(std::size_t index,
                            const filament::math::float3& from,
                            const filament::math::float3& to,
                            const filament::math::float3& color, bool visible) {
    if (index >= jointLines_.size()) return;
    LineEntity& line = jointLines_[index];
    if (visible && line.mi) {
        line.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                              float4{color.x, color.y, color.z, 1.0f});
    }
    updateLine(line, from, to, visible);
}

void Renderer::configureLineBatches(
    const std::vector<filament::math::float3>& colors,
    std::size_t maxSegments) {
    if (!ensureLineMaterial() || maxSegments == 0) return;
    lineBatchCapacity_ = maxSegments;

    // 三角形の並びは固定（線分あたり 8 頂点 / 12 インデックス）。毎フレーム
    // 書き換えるのは頂点座標だけで、ジオメトリの構成には触らない。
    const std::size_t vertexCount = maxSegments * kTubeVertices;
    const std::size_t indexCount = maxSegments * kTubeIndices;
    for (const auto& c : colors) {
        LineBatch batch;
        batch.vb = VertexBuffer::Builder()
                       .vertexCount(uint32_t(vertexCount))
                       .bufferCount(1)
                       .attribute(VertexAttribute::POSITION, 0,
                                  VertexBuffer::AttributeType::FLOAT3, 0,
                                  sizeof(float) * 3)
                       .build(*engine_);

        auto* indices = new uint16_t[indexCount];
        for (std::size_t s = 0; s < maxSegments; ++s) {
            const uint16_t base = uint16_t(s * kTubeVertices);
            uint16_t* out = indices + s * kTubeIndices;
            // 板 1（u 方向）と板 2（v 方向）で 2 三角形ずつ。
            const uint16_t quad[2][4] = {
                {uint16_t(base + 0), uint16_t(base + 1), uint16_t(base + 2),
                 uint16_t(base + 3)},
                {uint16_t(base + 4), uint16_t(base + 5), uint16_t(base + 6),
                 uint16_t(base + 7)}};
            for (int q = 0; q < 2; ++q) {
                out[q * 6 + 0] = quad[q][0];
                out[q * 6 + 1] = quad[q][1];
                out[q * 6 + 2] = quad[q][2];
                out[q * 6 + 3] = quad[q][0];
                out[q * 6 + 4] = quad[q][2];
                out[q * 6 + 5] = quad[q][3];
            }
        }
        batch.ib = IndexBuffer::Builder()
                       .indexCount(uint32_t(indexCount))
                       .bufferType(IndexBuffer::IndexType::USHORT)
                       .build(*engine_);
        batch.ib->setBuffer(
            *engine_,
            IndexBuffer::BufferDescriptor(
                indices, sizeof(uint16_t) * indexCount,
                [](void* p, size_t, void*) {
                    delete[] static_cast<uint16_t*>(p);
                }));

        batch.mi = lineMaterial_->createInstance();
        batch.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                               float4{c.x, c.y, c.z, 1.0f});

        batch.entity = EntityManager::get().create();
        RenderableManager::Builder(1)
            .boundingBox({{-1000.0f, -1000.0f, -1000.0f},
                          {1000.0f, 1000.0f, 1000.0f}})  // never culled
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, batch.vb,
                      batch.ib, 0, indexCount)
            .material(0, batch.mi)
            .culling(false)
            .castShadows(false)
            .receiveShadows(false)
            .build(*engine_, batch.entity);

        // エディタ専用レイヤへ。これでバッチはシーンに入っていても、
        // setViewEditorLayerVisible で選んだビューにしか映らない。
        auto& rm = engine_->getRenderableManager();
        rm.setLayerMask(rm.getInstance(batch.entity), 0xFF, kLayerEditorOnly);

        lineBatches_.push_back(std::move(batch));
    }
}

void Renderer::setLineBatch(std::size_t index,
                            const std::vector<BatchShape>& shapes) {
    if (index >= lineBatches_.size()) return;
    LineBatch& batch = lineBatches_[index];

    if (shapes.empty()) {
        if (batch.inScene) {
            scene_->remove(batch.entity);
            batch.inScene = false;
        }
        batch.current.clear();
        return;
    }

    // 中身が前回と同じなら転送しない。エディタで手が止まっているあいだ、
    // 毎フレーム同じ数十 KB を GPU に送り続けても意味がない。
    if (batch.current == shapes) return;
    batch.current = shapes;

    // 余りは面積 0 の三角形で埋める。プリミティブ数を変えずに見た目だけ
    // 消せるので、ジオメトリを組み直す必要がない。
    const std::size_t vertexCount = lineBatchCapacity_ * kTubeVertices;
    auto* verts = new float3[vertexCount];
    const std::size_t used = std::min(shapes.size(), lineBatchCapacity_);
    for (std::size_t s = 0; s < used; ++s) {
        const BatchShape& shape = shapes[s];
        float3* out = verts + s * kTubeVertices;
        if (shape.width > 0.0f) {
            buildTube(out, shape.a, shape.b, shape.width * 0.5f);
            continue;
        }
        // 塗りつぶしの四角は前半（頂点 0-3 = 三角形 2 枚）だけを使い、
        // 後半は 1 点に潰して消す。
        out[0] = shape.a;
        out[1] = shape.b;
        out[2] = shape.c;
        out[3] = shape.d;
        for (std::size_t i = 4; i < kTubeVertices; ++i) out[i] = shape.a;
    }
    const float3 pad = used > 0 ? shapes[used - 1].a : float3{0.0f};
    for (std::size_t s = used; s < lineBatchCapacity_; ++s) {
        for (std::size_t i = 0; i < kTubeVertices; ++i) {
            verts[s * kTubeVertices + i] = pad;
        }
    }

    batch.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            verts, sizeof(float3) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));

    if (!batch.inScene) {
        scene_->addEntity(batch.entity);
        batch.inScene = true;
    }
}

std::size_t Renderer::addLineSet(const filament::math::float3& color) {
    LineSet set;
    if (ensureLineMaterial()) {
        set.mi = lineMaterial_->createInstance();
        set.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                             float4{color.x, color.y, color.z, 1.0f});
    }
    // エンティティとバッファは最初の setLineSet で本数が決まってから作る。
    lineSets_.push_back(set);
    return lineSets_.size() - 1;
}

void Renderer::setLineSet(std::size_t id,
                          const std::vector<filament::math::float3>& points) {
    if (id >= lineSets_.size()) return;
    LineSet& set = lineSets_[id];
    if (!set.mi) return;  // line.filamat が無い環境では線は無効

    const std::size_t lines = points.size() / 2;
    if (lines == 0) {
        if (set.inScene) {
            scene_->remove(set.entity);
            set.inScene = false;
        }
        return;
    }

    // 本数が変わったらレンダラブルごと作り直す。頂点はぴったりの数だけ
    // 確保するので、余りを埋める工夫（面積 0 の三角形）も要らない。
    if (lines != set.lineCount) {
        if (set.lineCount > 0) {
            if (set.inScene) scene_->remove(set.entity);
            engine_->destroy(set.entity);
            EntityManager::get().destroy(set.entity);
            engine_->destroy(set.vb);
            engine_->destroy(set.ib);
            set.inScene = false;
        }
        set.lineCount = lines;
        const std::size_t vertexCount = lines * 2;

        set.vb = VertexBuffer::Builder()
                     .vertexCount(uint32_t(vertexCount))
                     .bufferCount(1)
                     .attribute(VertexAttribute::POSITION, 0,
                                VertexBuffer::AttributeType::FLOAT3, 0,
                                sizeof(float) * 3)
                     .build(*engine_);

        auto* indices = new uint16_t[vertexCount];
        for (std::size_t i = 0; i < vertexCount; ++i) indices[i] = uint16_t(i);
        set.ib = IndexBuffer::Builder()
                     .indexCount(uint32_t(vertexCount))
                     .bufferType(IndexBuffer::IndexType::USHORT)
                     .build(*engine_);
        set.ib->setBuffer(
            *engine_,
            IndexBuffer::BufferDescriptor(
                indices, sizeof(uint16_t) * vertexCount,
                [](void* p, size_t, void*) {
                    delete[] static_cast<uint16_t*>(p);
                }));

        set.entity = EntityManager::get().create();
        RenderableManager::Builder(1)
            .boundingBox({{-1000.0f, -1000.0f, -1000.0f},
                          {1000.0f, 1000.0f, 1000.0f}})  // never culled
            .geometry(0, RenderableManager::PrimitiveType::LINES, set.vb,
                      set.ib, 0, vertexCount)
            .material(0, set.mi)
            .culling(false)
            .castShadows(false)
            .receiveShadows(false)
            .build(*engine_, set.entity);
        auto& rm = engine_->getRenderableManager();
        rm.setLayerMask(rm.getInstance(set.entity), 0xFF, kLayerEditorOnly);
    }

    const std::size_t vertexCount = lines * 2;
    auto* verts = new float3[vertexCount];
    std::memcpy(verts, points.data(), sizeof(float3) * vertexCount);
    set.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            verts, sizeof(float3) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));

    if (!set.inScene) {
        scene_->addEntity(set.entity);
        set.inScene = true;
    }
}

}  // namespace wizengine
