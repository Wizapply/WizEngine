#include "terrain/TerrainScatter.h"

#include <algorithm>
#include <cmath>

#include "terrain/TerrainMath.h"

namespace wizengine {
namespace terrain {

double scatterLocalX(const HeightField& field, double u) {
    return double(field.minX()) + u * (field.maxX() - double(field.minX()));
}

double scatterLocalZ(const HeightField& field, double v) {
    return double(field.minZ()) + v * (field.maxZ() - double(field.minZ()));
}

bool scatterUvAt(const HeightField& field, double x, double z, double& u, double& v) {
    if (!field.contains(x, z)) return false;  // 空の格子・NaN・無限もここで弾く
    const double x0 = double(field.minX());
    const double z0 = double(field.minZ());
    // 境界の丸めで 0〜1 をわずかに外れないよう丸める（範囲の判定は contains）。
    u = std::clamp((x - x0) / (field.maxX() - x0), 0.0, 1.0);
    v = std::clamp((z - z0) / (field.maxZ() - z0), 0.0, 1.0);
    return true;
}

ScatterPrototype clampScatterPrototype(ScatterPrototype p) {
    const ScatterPrototype d;
    p.scale = float(clampOr(p.scale, kScatterScaleMin, kScatterScaleMax, d.scale));
    p.trunkRadius = float(clampOr(p.trunkRadius, 0.0, kScatterTrunkMax, d.trunkRadius));
    p.trunkHeight = float(clampOr(p.trunkHeight, 0.0, kScatterTrunkMax, d.trunkHeight));
    return p;
}

std::size_t pruneScatterInstances(TerrainScatter& s) {
    const std::size_t before = s.instances.size();
    auto unit = [](float t) { return std::isfinite(t) && t >= 0.0f && t <= 1.0f; };
    s.instances.erase(
        std::remove_if(s.instances.begin(), s.instances.end(),
                       [&](const ScatterInstance& i) {
                           const bool usable =
                               unit(i.u) && unit(i.v) && std::isfinite(i.scale) &&
                               i.scale > 0.0f && std::isfinite(i.yaw) &&
                               int(i.prototype) < kMaxScatterPrototypes;
                           return !usable;
                       }),
        s.instances.end());
    if (s.instances.size() > std::size_t(kMaxScatterInstances)) {
        s.instances.resize(std::size_t(kMaxScatterInstances));
    }
    return before - s.instances.size();
}

}  // namespace terrain
}  // namespace wizengine
