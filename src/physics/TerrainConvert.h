#pragma once

// Chrono の型 ⇄ テレインの接触（terrain/）の型の変換。

#include <chrono/core/ChMatrix33.h>
#include <chrono/core/ChVector3.h>

#include "terrain/TerrainContact.h"

namespace wizengine {
namespace physics {

inline terrain::Vec3 toTerrainVec(const chrono::ChVector3d& v) {
    return terrain::Vec3(v.x(), v.y(), v.z());
}

inline chrono::ChVector3d fromTerrainVec(const terrain::Vec3& v) {
    return chrono::ChVector3d(v.x(), v.y(), v.z());
}

inline terrain::Mat3 toTerrainMat(const chrono::ChMatrix33<>& r) {
    terrain::Mat3 m;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m(i, j) = r(i, j);
    }
    return m;
}

}  // namespace physics
}  // namespace wizengine
