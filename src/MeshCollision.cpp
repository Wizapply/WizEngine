#include "MeshCollision.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

#include <chrono/geometry/ChTriangleMeshConnected.h>

// Filament's gltfio library carries its own cgltf implementation with
// external linkage, so a plain CGLTF_IMPLEMENTATION here collides with it at
// link time (LNK2005). An anonymous namespace does not help either: cgltf's
// declarations sit in an extern "C" block, which keeps C linkage regardless
// of the namespace. So this copy renames every public cgltf function to a
// wiz_-prefixed symbol via macros - the macros rewrite the declarations, the
// implementation AND our call sites below consistently, and the resulting
// symbols cannot clash with gltfio's. The list is generated from cgltf.h
// v1.14's declaration section; regenerate it if the pinned version changes.
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

#include "AssetError.h"
#include "Log.h"

using chrono::ChTriangleMeshConnected;
using chrono::ChVector3d;
using chrono::ChVector3i;

namespace wizengine {

namespace {

// Applies a cgltf world matrix (column-major float[16]) to a point.
ChVector3d transformPoint(const float* m, float x, float y, float z) {
    return ChVector3d(m[0] * x + m[4] * y + m[8] * z + m[12],
                      m[1] * x + m[5] * y + m[9] * z + m[13],
                      m[2] * x + m[6] * y + m[10] * z + m[14]);
}

// Every triangle of a .glb/.gltf as one connected mesh. いまは凸包
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
    // Loads the binary chunk of a .glb / the external buffers of a .gltf
    // (relative to the file, like the renderer's loader).
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

std::vector<ChVector3d> loadCollisionPoints(const std::string& name,
                                            double scale,
                                            std::size_t maxPoints) {
    ChTriangleMeshConnected mesh;
    if (!loadCollisionMesh(name, scale, mesh)) return {};
    std::vector<ChVector3d> pts = mesh.GetCoordsVertices();

    // Decimate by snapping points to a grid and keeping one per cell,
    // halving the grid resolution until the count fits. The hull of the
    // decimated set is within one cell of the true hull - far below the
    // collision envelope at these resolutions.
    ChVector3d lo = pts[0], hi = pts[0];
    for (const auto& p : pts) {
        lo = ChVector3d(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()),
                        std::min(lo.z(), p.z()));
        hi = ChVector3d(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()),
                        std::max(hi.z(), p.z()));
    }
    const ChVector3d span = hi - lo;
    const double maxSpan =
        std::max({span.x(), span.y(), span.z(), 1e-9});

    const std::size_t original = pts.size();
    for (int res = 64; res >= 4 && pts.size() > maxPoints; res /= 2) {
        std::unordered_set<std::uint64_t> cells;
        std::vector<ChVector3d> kept;
        kept.reserve(maxPoints);
        const double cell = maxSpan / res;
        for (const auto& p : pts) {
            const std::uint64_t cx = std::uint64_t((p.x() - lo.x()) / cell);
            const std::uint64_t cy = std::uint64_t((p.y() - lo.y()) / cell);
            const std::uint64_t cz = std::uint64_t((p.z() - lo.z()) / cell);
            const std::uint64_t key = (cx << 42) | (cy << 21) | cz;
            if (cells.insert(key).second) kept.push_back(p);
        }
        pts = std::move(kept);
    }

    LOGI("collision", "%s: hull points %zu (from %zu vertices)", name.c_str(),
         pts.size(), original);
    return pts;
}

}  // namespace wizengine
