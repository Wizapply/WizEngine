// Renderer の立方体・球・円柱のメッシュ生成。
#include "render/RendererInternal.h"

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

namespace {

// 24 vertices: 4 per face, so each face can carry its own normal (needed for
// lit shading). Faces are wound CCW when viewed from outside.
const float3 kCubePos[24] = {
    // front (+Z)
    {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
    // back (-Z)
    {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
    // left (-X)
    {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f},
    // right (+X)
    {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f},
    // top (+Y)
    {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
    // bottom (-Y)
    {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f},
};

// Per-face normals, in the same order as kCubePos (front, back, left, right,
// top, bottom). Used to build the tangent frames a lit material needs.
const float3 kCubeNrm[24] = {
    {0, 0, 1},  {0, 0, 1},  {0, 0, 1},  {0, 0, 1},   // front (+Z)
    {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},  // back (-Z)
    {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0},  // left (-X)
    {1, 0, 0},  {1, 0, 0},  {1, 0, 0},  {1, 0, 0},   // right (+X)
    {0, 1, 0},  {0, 1, 0},  {0, 1, 0},  {0, 1, 0},   // top (+Y)
    {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0},  // bottom (-Y)
};

// Two triangles per face: (0,1,2) (2,3,0), offset by 4*face.
const uint16_t kCubeIdx[36] = {
    0, 1, 2, 2, 3, 0,        4, 5, 6, 6, 7, 4,
    8, 9, 10, 10, 11, 8,     12, 13, 14, 14, 15, 12,
    16, 17, 18, 18, 19, 16,  20, 21, 22, 22, 23, 20,
};

// UV 球の分割数。エディタで置く球はたいてい小さいので、これで十分に丸い。
constexpr int kSphereRings = 16;    // 緯度方向
constexpr int kSphereSectors = 24;  // 経度方向
// M_PI は MSVC だと _USE_MATH_DEFINES が要るので、自前で持つ。
constexpr float kPi = 3.14159265358979323846f;

}  // namespace

namespace wizengine {

void Renderer::buildCubeMesh() {
    // Shared cube mesh for boxes: positions + tangent frames (for lit shading).
    auto* cubeOrient = filament::geometry::SurfaceOrientation::Builder()
                           .vertexCount(24)
                           .normals(kCubeNrm)
                           .build();
    cubeOrient->getQuats(cubeTangents_, 24);
    delete cubeOrient;

    vb_ = VertexBuffer::Builder()
              .vertexCount(24)
              .bufferCount(2)
              .attribute(VertexAttribute::POSITION, 0,
                         VertexBuffer::AttributeType::FLOAT3)
              .attribute(VertexAttribute::TANGENTS, 1,
                         VertexBuffer::AttributeType::FLOAT4)
              .build(*engine_);
    vb_->setBufferAt(*engine_, 0,
                     VertexBuffer::BufferDescriptor(kCubePos, sizeof(kCubePos)));
    vb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(cubeTangents_, sizeof(cubeTangents_)));

    ib_ = IndexBuffer::Builder()
              .indexCount(36)
              .bufferType(IndexBuffer::IndexType::USHORT)
              .build(*engine_);
    ib_->setBuffer(*engine_,
                   IndexBuffer::BufferDescriptor(kCubeIdx, sizeof(kCubeIdx)));
}

void Renderer::ensureSphereMesh() {
    if (sphereVb_) return;

    // 半径 0.5 の単位球。箱と同じく「サイズ 1 で直径 1」になるので、
    // Scene 側は形が変わってもスケール行列を同じ考え方で組める。
    constexpr int rings = kSphereRings;
    constexpr int sectors = kSphereSectors;
    // 個数は最初から size_t で持つ。int を受け口でキャストすると
    // `std::vector<float3> normals(std::size_t(vertexCount));` が
    // 「関数の宣言」に解釈されてしまう（most vexing parse）。
    const std::size_t vertexCount =
        std::size_t(rings + 1) * std::size_t(sectors + 1);
    const std::size_t indexCount = std::size_t(rings) * std::size_t(sectors) * 6;

    auto* positions = new float3[vertexCount];
    std::vector<float3> normals(vertexCount);
    for (int r = 0; r <= rings; ++r) {
        const float theta = kPi * float(r) / float(rings);
        const float sinT = std::sin(theta);
        const float cosT = std::cos(theta);
        for (int s = 0; s <= sectors; ++s) {
            const float phi = 2.0f * kPi * float(s) / float(sectors);
            const float3 n{sinT * std::cos(phi), cosT, sinT * std::sin(phi)};
            const int i = r * (sectors + 1) + s;
            normals[std::size_t(i)] = n;
            positions[i] = float3{n.x * 0.5f, n.y * 0.5f, n.z * 0.5f};
        }
    }

    // 法線から接空間へ。頂点数が可変なので、立方体のように固定長メンバへは
    // 置けない（この配列は VertexBuffer へ渡したあと解放コールバックで消す）。
    auto* tangents = new quatf[vertexCount];
    auto* orient = filament::geometry::SurfaceOrientation::Builder()
                       .vertexCount(vertexCount)
                       .normals(normals.data())
                       .build();
    orient->getQuats(tangents, vertexCount);
    delete orient;

    auto* indices = new uint16_t[indexCount];
    int k = 0;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            const uint16_t a = uint16_t(r * (sectors + 1) + s);
            const uint16_t b = uint16_t(a + sectors + 1);
            // 外向きが表になる巻き方（a, a+1, b）／（b, a+1, b+1）。
            indices[k++] = a;
            indices[k++] = uint16_t(a + 1);
            indices[k++] = b;
            indices[k++] = b;
            indices[k++] = uint16_t(a + 1);
            indices[k++] = uint16_t(b + 1);
        }
    }

    sphereVb_ = VertexBuffer::Builder()
                    .vertexCount(uint32_t(vertexCount))
                    .bufferCount(2)
                    .attribute(VertexAttribute::POSITION, 0,
                               VertexBuffer::AttributeType::FLOAT3)
                    .attribute(VertexAttribute::TANGENTS, 1,
                               VertexBuffer::AttributeType::FLOAT4)
                    .build(*engine_);
    sphereVb_->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            positions, sizeof(float3) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));
    sphereVb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(
            tangents, sizeof(quatf) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));

    sphereIb_ = IndexBuffer::Builder()
                    .indexCount(uint32_t(indexCount))
                    .bufferType(IndexBuffer::IndexType::USHORT)
                    .build(*engine_);
    sphereIb_->setBuffer(
        *engine_,
        IndexBuffer::BufferDescriptor(
            indices, sizeof(uint16_t) * indexCount,
            [](void* p, size_t, void*) { delete[] static_cast<uint16_t*>(p); }));
    sphereIndexCount_ = uint32_t(indexCount);
}

void Renderer::ensureCylinderMesh() {
    if (cylinderVb_) return;

    // 軸が X、半径 0.5、長さ 1 の単位円柱（x = -0.5 .. +0.5）。側面は
    // 2 つのリング、両端は中心 + リングの扇で塞ぐ。法線は側面が放射方向、
    // 蓋が ±X なので、蓋とリングの頂点は側面と共有しない（角が丸まらない）。
    constexpr int sectors = kSphereSectors;
    const std::size_t ring = std::size_t(sectors + 1);
    // 側面 2 リング + 蓋 2 枚（中心 1 + リング）。
    const std::size_t vertexCount = ring * 2 + (ring + 1) * 2;
    const std::size_t indexCount =
        std::size_t(sectors) * 6 + std::size_t(sectors) * 3 * 2;

    auto* positions = new float3[vertexCount];
    std::vector<float3> normals(vertexCount);
    std::size_t v = 0;
    for (int side = 0; side < 2; ++side) {
        const float x = side == 0 ? -0.5f : 0.5f;
        for (int s = 0; s <= sectors; ++s) {
            const float phi = 2.0f * kPi * float(s) / float(sectors);
            const float3 n{0.0f, std::cos(phi), std::sin(phi)};
            normals[v] = n;
            positions[v] = float3{x, n.y * 0.5f, n.z * 0.5f};
            ++v;
        }
    }
    const std::size_t capStart[2] = {v, v + ring + 1};
    for (int side = 0; side < 2; ++side) {
        const float x = side == 0 ? -0.5f : 0.5f;
        const float3 n{x < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
        normals[v] = n;
        positions[v] = float3{x, 0.0f, 0.0f};
        ++v;
        for (int s = 0; s <= sectors; ++s) {
            const float phi = 2.0f * kPi * float(s) / float(sectors);
            normals[v] = n;
            positions[v] = float3{x, 0.5f * std::cos(phi), 0.5f * std::sin(phi)};
            ++v;
        }
    }

    auto* tangents = new quatf[vertexCount];
    auto* orient = filament::geometry::SurfaceOrientation::Builder()
                       .vertexCount(vertexCount)
                       .normals(normals.data())
                       .build();
    orient->getQuats(tangents, vertexCount);
    delete orient;

    auto* indices = new uint16_t[indexCount];
    std::size_t k = 0;
    for (int s = 0; s < sectors; ++s) {
        // 側面: 左リング a, a+1 と右リング b, b+1。外向き（放射方向）が表に
        // なる巻き方 (a, a+1, b) / (a+1, b+1, b)。
        const uint16_t a = uint16_t(s);
        const uint16_t b = uint16_t(ring + std::size_t(s));
        indices[k++] = a;
        indices[k++] = uint16_t(a + 1);
        indices[k++] = b;
        indices[k++] = uint16_t(a + 1);
        indices[k++] = uint16_t(b + 1);
        indices[k++] = b;
    }
    for (int side = 0; side < 2; ++side) {
        const uint16_t c = uint16_t(capStart[side]);
        for (int s = 0; s < sectors; ++s) {
            const uint16_t p = uint16_t(capStart[side] + 1 + std::size_t(s));
            indices[k++] = c;
            if (side == 0) {  // -X の蓋は外から見て逆回り
                indices[k++] = uint16_t(p + 1);
                indices[k++] = p;
            } else {
                indices[k++] = p;
                indices[k++] = uint16_t(p + 1);
            }
        }
    }

    cylinderVb_ = VertexBuffer::Builder()
                      .vertexCount(uint32_t(vertexCount))
                      .bufferCount(2)
                      .attribute(VertexAttribute::POSITION, 0,
                                 VertexBuffer::AttributeType::FLOAT3)
                      .attribute(VertexAttribute::TANGENTS, 1,
                                 VertexBuffer::AttributeType::FLOAT4)
                      .build(*engine_);
    cylinderVb_->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            positions, sizeof(float3) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));
    cylinderVb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(
            tangents, sizeof(quatf) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));

    cylinderIb_ = IndexBuffer::Builder()
                      .indexCount(uint32_t(indexCount))
                      .bufferType(IndexBuffer::IndexType::USHORT)
                      .build(*engine_);
    cylinderIb_->setBuffer(
        *engine_,
        IndexBuffer::BufferDescriptor(
            indices, sizeof(uint16_t) * indexCount,
            [](void* p, size_t, void*) { delete[] static_cast<uint16_t*>(p); }));
    cylinderIndexCount_ = uint32_t(indexCount);
}

}  // namespace wizengine
