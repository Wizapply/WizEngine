#pragma once

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

}  // namespace wizengine