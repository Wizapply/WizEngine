#include "physics/MeshCollision.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

#include <chrono/geometry/ChTriangleMeshConnected.h>

// Filament の gltfio ライブラリは外部リンケージの cgltf の実装を自分で持つので、
// ここで素の CGLTF_IMPLEMENTATION を書くとリンク時にぶつかる（LNK2005）。
// 無名名前空間も効かない: cgltf の宣言は extern "C" のブロックの中にあり、
// 名前空間に関係なく C リンケージのまま。そこでこの写しは、公開されている
// cgltf の関数を全部マクロで wiz_ 付きの名前に変える - マクロは宣言・実装・
// 下の呼び出し側をそろって書き換えるので、できたシンボルは gltfio のものと
// ぶつからない。一覧は cgltf.h v1.14 の宣言部から作った。固定している版が
// 変わったら作り直す。
#define cgltf_accessor_index wiz_cgltf_accessor_index
#define cgltf_accessor_read_float wiz_cgltf_accessor_read_float
#define cgltf_accessor_read_index wiz_cgltf_accessor_read_index
#define cgltf_accessor_read_uint wiz_cgltf_accessor_read_uint
#define cgltf_accessor_unpack_floats wiz_cgltf_accessor_unpack_floats
#define cgltf_accessor_unpack_indices wiz_cgltf_accessor_unpack_indices
#define cgltf_animation_channel_index wiz_cgltf_animation_channel_index
#define cgltf_animation_index wiz_cgltf_animation_index
#define cgltf_animation_sampler_index wiz_cgltf_animation_sampler_index
#define cgltf_buffer_index wiz_cgltf_buffer_index
#define cgltf_buffer_view_data wiz_cgltf_buffer_view_data
#define cgltf_buffer_view_index wiz_cgltf_buffer_view_index
#define cgltf_calc_size wiz_cgltf_calc_size
#define cgltf_camera_index wiz_cgltf_camera_index
#define cgltf_component_size wiz_cgltf_component_size
#define cgltf_copy_extras_json wiz_cgltf_copy_extras_json
#define cgltf_decode_string wiz_cgltf_decode_string
#define cgltf_decode_uri wiz_cgltf_decode_uri
#define cgltf_free wiz_cgltf_free
#define cgltf_image_index wiz_cgltf_image_index
#define cgltf_light_index wiz_cgltf_light_index
#define cgltf_load_buffer_base64 wiz_cgltf_load_buffer_base64
#define cgltf_load_buffers wiz_cgltf_load_buffers
#define cgltf_material_index wiz_cgltf_material_index
#define cgltf_mesh_index wiz_cgltf_mesh_index
#define cgltf_node_index wiz_cgltf_node_index
#define cgltf_node_transform_local wiz_cgltf_node_transform_local
#define cgltf_node_transform_world wiz_cgltf_node_transform_world
#define cgltf_num_components wiz_cgltf_num_components
#define cgltf_parse wiz_cgltf_parse
#define cgltf_parse_file wiz_cgltf_parse_file
#define cgltf_result wiz_cgltf_result
#define cgltf_sampler_index wiz_cgltf_sampler_index
#define cgltf_scene_index wiz_cgltf_scene_index
#define cgltf_skin_index wiz_cgltf_skin_index
#define cgltf_texture_index wiz_cgltf_texture_index
#define cgltf_validate wiz_cgltf_validate
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "core/AssetError.h"
#include "core/Log.h"

using chrono::ChTriangleMeshConnected;
using chrono::ChVector3d;
using chrono::ChVector3i;

namespace wizengine {

namespace {

// cgltf のワールド行列（列優先の float[16]）を点に掛ける。
ChVector3d transformPoint(const float* m, float x, float y, float z) {
    return ChVector3d(m[0] * x + m[4] * y + m[8] * z + m[12],
                      m[1] * x + m[5] * y + m[9] * z + m[13],
                      m[2] * x + m[6] * y + m[10] * z + m[14]);
}

// .glb/.gltf の全三角形を 1 つのつながったメッシュとして読む。いまは凸包
// （loadCollisionPoints）の入力にだけ使う内部関数。
bool loadCollisionMesh(const std::string& name, double scale,
                       ChTriangleMeshConnected& out) {
    const std::string path = assetPath(name);

    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) !=
        cgltf_result_success) {
        LOGW("collision", "'%s' could not be parsed as glTF/GLB", path.c_str());
        return false;
    }
    // .glb のバイナリチャンク / .gltf の外部バッファを読む
    // （描画側の読み込みと同じく、ファイルからの相対）。
    if (cgltf_load_buffers(&options, data, path.c_str()) !=
        cgltf_result_success) {
        LOGW("collision", "'%s': mesh buffers could not be loaded",
             path.c_str());
        cgltf_free(data);
        return false;
    }

    auto& verts = out.GetCoordsVertices();
    auto& tris = out.GetIndicesVertexes();
    verts.clear();
    tris.clear();

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node* node = &data->nodes[n];
        if (!node->mesh) continue;
        float world[16];
        cgltf_node_transform_world(node, world);

        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive* prim = &node->mesh->primitives[p];
            if (prim->type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* pos = nullptr;
            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                if (prim->attributes[a].type == cgltf_attribute_type_position) {
                    pos = prim->attributes[a].data;
                    break;
                }
            }
            if (!pos) continue;

            const std::size_t base = verts.size();
            for (cgltf_size v = 0; v < pos->count; ++v) {
                float xyz[3] = {0, 0, 0};
                cgltf_accessor_read_float(pos, v, xyz, 3);
                verts.push_back(
                    transformPoint(world, xyz[0], xyz[1], xyz[2]) * scale);
            }

            if (prim->indices) {
                for (cgltf_size i = 0; i + 2 < prim->indices->count; i += 3) {
                    tris.push_back(ChVector3i(
                        int(base + cgltf_accessor_read_index(prim->indices, i)),
                        int(base +
                            cgltf_accessor_read_index(prim->indices, i + 1)),
                        int(base +
                            cgltf_accessor_read_index(prim->indices, i + 2))));
                }
            } else {
                for (cgltf_size i = 0; i + 2 < pos->count; i += 3) {
                    tris.push_back(ChVector3i(int(base + i), int(base + i + 1),
                                              int(base + i + 2)));
                }
            }
        }
    }
    cgltf_free(data);

    if (verts.empty() || tris.empty()) {
        LOGW("collision", "'%s' contains no triangle geometry", path.c_str());
        return false;
    }
    LOGI("collision", "%s: %zu vertices, %zu triangles (scale %.3f)",
         name.c_str(), verts.size(), tris.size(), scale);
    if (tris.size() > 5000) {
        LOGW("collision",
             "%s: %zu triangles is a lot for a collision mesh - the multicore "
             "collision system pays per triangle and the physics thread can "
             "stall. Use a decimated proxy mesh.",
             name.c_str(), tris.size());
    }
    return true;
}

}  // namespace

std::vector<ChVector3d> decimatePoints(std::vector<ChVector3d> pts, std::size_t maxPoints) {
    if (pts.size() <= maxPoints || pts.empty()) return pts;
    ChVector3d lo = pts[0], hi = pts[0];
    for (const auto& p : pts) {
        lo = ChVector3d(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
        hi = ChVector3d(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
    }
    const ChVector3d span = hi - lo;
    const double maxSpan = std::max({span.x(), span.y(), span.z(), 1e-9});
    for (int res = 64; res >= 2 && pts.size() > maxPoints; res /= 2) {
        std::unordered_set<std::uint64_t> cells;
        std::vector<ChVector3d> kept;
        kept.reserve(maxPoints);
        const double cell = maxSpan / res;
        for (const auto& p : pts) {
            const std::uint64_t cx = std::uint64_t((p.x() - lo.x()) / cell);
            const std::uint64_t cy = std::uint64_t((p.y() - lo.y()) / cell);
            const std::uint64_t cz = std::uint64_t((p.z() - lo.z()) / cell);
            if (cells.insert((cx << 42) | (cy << 21) | cz).second) kept.push_back(p);
        }
        pts = std::move(kept);
    }
    // 格子 2 でも多い（同じ点が大量など）ときは等間隔に抜く。
    if (pts.size() > maxPoints) {
        std::vector<ChVector3d> kept;
        kept.reserve(maxPoints);
        const double stride = double(pts.size()) / double(maxPoints);
        for (std::size_t k = 0; k < maxPoints; ++k) kept.push_back(pts[std::size_t(k * stride)]);
        pts = std::move(kept);
    }
    return pts;
}

std::vector<ChVector3d> loadCollisionPoints(const std::string& name,
                                            double scale,
                                            std::size_t maxPoints) {
    ChTriangleMeshConnected mesh;
    if (!loadCollisionMesh(name, scale, mesh)) return {};
    std::vector<ChVector3d> pts = mesh.GetCoordsVertices();

    // 間引いた点の凸包は本当の凸包から 1 セル以内に収まる - この解像度では
    // 衝突のエンベロープよりずっと小さい。
    const std::size_t original = pts.size();
    pts = decimatePoints(std::move(pts), maxPoints);

    LOGI("collision", "%s: hull points %zu (from %zu vertices)", name.c_str(),
         pts.size(), original);
    return pts;
}

CollisionTriangles loadCollisionTriangles(const std::string& name, double scale) {
    CollisionTriangles out;
    ChTriangleMeshConnected mesh;
    if (!loadCollisionMesh(name, scale, mesh)) return out;
    out.vertices = mesh.GetCoordsVertices();
    const auto& tris = mesh.GetIndicesVertexes();
    out.triangles.reserve(tris.size());
    for (const auto& t : tris) {
        out.triangles.push_back({int(t.x()), int(t.y()), int(t.z())});
    }
    return out;
}

}  // namespace wizengine
