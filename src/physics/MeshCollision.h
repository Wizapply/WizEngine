#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <chrono/core/ChVector3.h>

namespace wizengine {

// CPU-side geometry reader for collision shapes. The renderer's glTF path
// (gltfio) streams vertices straight to the GPU, so physics gets its own
// small reader built on cgltf - the same single-header parser gltfio uses
// internally. Resolves `name` through assetPath() like every other loader,
// applies each node's world transform and the given uniform scale, and logs
// what it produced. Returns empty instead of throwing: hull collision is an
// optional extra on top of a scene that already renders, so a bad file
// degrades (with a warning) rather than killing the run.

// The model's vertices decimated to at most maxPoints (grid quantisation),
// ready for ChBodyEasyConvexHull. A few hundred points is plenty for a hull;
// feeding it every vertex of a dense model only slows hull construction.
// Scene の <asset><mesh/>（凸包の当たり判定）がこれを使う。
std::vector<chrono::ChVector3d> loadCollisionPoints(
    const std::string& name, double scale, std::size_t maxPoints = 512);

// 点を格子に寄せて 1 セル 1 点に間引く。格子を粗くしながら maxPoints 以下に
// なるまで。外形は 1 セル以内に保たれる。点の順は元の順のまま（pointId を
// 安定させるため）。格子 2 でも多い（同じ点が大量など）ときは等間隔に抜く。
// loadCollisionPoints（凸包の入力）とテレインの接触の形の記録が使う。
std::vector<chrono::ChVector3d> decimatePoints(std::vector<chrono::ChVector3d> pts,
                                               std::size_t maxPoints);

// 三角形をそのまま（頂点と 3 つ組の添字）。凹形状の当たり判定
// （<geom collision="trimesh">、PhysicsWorld::addTriangleMesh）用。
// 読めなければ両方空。
struct CollisionTriangles {
    std::vector<chrono::ChVector3d> vertices;
    std::vector<std::array<int, 3>> triangles;
};
CollisionTriangles loadCollisionTriangles(const std::string& name, double scale);

}  // namespace wizengine