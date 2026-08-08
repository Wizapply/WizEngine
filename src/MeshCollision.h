#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <chrono/core/ChVector3.h>

namespace chrono {
class ChTriangleMeshConnected;
}

namespace wizengine {

// CPU-side geometry readers for collision shapes. The renderer's glTF path
// (gltfio) streams vertices straight to the GPU, so physics gets its own
// small reader built on cgltf - the same single-header parser gltfio uses
// internally. Both functions resolve `name` through assetPath() like every
// other loader, apply each node's world transform and the given uniform
// scale, and log what they produced. They return empty/false instead of
// throwing: mesh collision is an optional extra on top of a scene that
// already renders, so a bad file degrades (with a warning) rather than
// killing the run.

// Every triangle of a .glb/.gltf as one connected mesh, for a *static*
// triangle-mesh collision body (terrain, obstacles). Dynamic bodies should
// use the convex hull below instead - mesh-vs-mesh dynamic contact is slow
// and fragile in any engine.
bool loadCollisionMesh(const std::string& name, double scale,
                       chrono::ChTriangleMeshConnected& out);

// The model's vertices decimated to at most maxPoints (grid quantisation),
// ready for ChBodyEasyConvexHull. A few hundred points is plenty for a hull;
// feeding it every vertex of a dense model only slows hull construction.
std::vector<chrono::ChVector3d> loadCollisionPoints(
    const std::string& name, double scale, std::size_t maxPoints = 512);

}  // namespace wizengine
