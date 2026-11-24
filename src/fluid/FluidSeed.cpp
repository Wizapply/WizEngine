#include "fluid/FluidSeed.h"

#include <algorithm>
#include <cmath>

namespace wizengine {
namespace fluid {

double particleVolume(double spacing) { return spacing * spacing * spacing / std::sqrt(2.0); }

double kernelRadius(double spacing) { return spacing / 0.9; }

FluidSeedResult seedFluid(const std::vector<FluidBlock>& blocks, double spacing,
                          std::size_t maxParticles) {
    FluidSeedResult out;
    if (!(spacing > 0.0)) return out;
    const double d = spacing;
    // HCP: 層（Y）の間隔 d √6/3、層の中の行（Z）の間隔 d √3/2。行ごとに X を d/2、
    // 層ごとに行の位置を d √3/6・X を d/2 ずらす（ABAB の積み方）。
    const double dy = d * std::sqrt(6.0) / 3.0;
    const double dz = d * std::sqrt(3.0) / 2.0;
    for (const FluidBlock& b : blocks) {
        const Vec3 lo{b.center.x - b.half.x + 0.5 * d, b.center.y - b.half.y + 0.5 * d,
                      b.center.z - b.half.z + 0.5 * d};
        const Vec3 hi{b.center.x + b.half.x - 0.5 * d, b.center.y + b.half.y - 0.5 * d,
                      b.center.z + b.half.z - 0.5 * d};
        if (hi.x < lo.x || hi.y < lo.y || hi.z < lo.z) {
            // 箱が粒子 1 個より小さい: 中心に 1 個だけ置く。
            ++out.requested;
            if (out.positions.size() / 3 >= maxParticles) {
                out.truncated = true;
                continue;
            }
            out.positions.insert(out.positions.end(),
                                 {float(b.center.x), float(b.center.y), float(b.center.z)});
            out.velocities.insert(out.velocities.end(), {float(b.velocity.x),
                                                         float(b.velocity.y),
                                                         float(b.velocity.z)});
            continue;
        }
        const int layers = int(std::floor((hi.y - lo.y) / dy)) + 1;
        for (int k = 0; k < layers; ++k) {
            const double y = lo.y + dy * k;
            const double zOff = (k % 2) ? d * std::sqrt(3.0) / 6.0 : 0.0;
            const int rows = int(std::floor((hi.z - lo.z - zOff) / dz)) + 1;
            for (int j = 0; j < rows; ++j) {
                const double z = lo.z + zOff + dz * j;
                if (z > hi.z + 1e-12) continue;
                const double xOff = ((j + k) % 2) ? 0.5 * d : 0.0;
                const int cols = int(std::floor((hi.x - lo.x - xOff) / d)) + 1;
                for (int i = 0; i < cols; ++i) {
                    const double x = lo.x + xOff + d * i;
                    if (x > hi.x + 1e-12) continue;
                    ++out.requested;
                    if (out.positions.size() / 3 >= maxParticles) {
                        out.truncated = true;
                        continue;
                    }
                    out.positions.insert(out.positions.end(), {float(x), float(y), float(z)});
                    out.velocities.insert(out.velocities.end(), {float(b.velocity.x),
                                                                 float(b.velocity.y),
                                                                 float(b.velocity.z)});
                }
            }
        }
    }
    return out;
}

std::vector<unsigned> particleMeshIndices(std::size_t count) {
    // 頂点の並び: 0 +X, 1 +Y, 2 −X, 3 −Y, 4 +Z, 5 −Z。上（+Z）の 4 面は
    // 赤道を +X → +Y → −X → −Y と回り、下の 4 面は逆向き。
    static const unsigned kTri[kParticleTris][3] = {
        {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4},
        {1, 0, 5}, {2, 1, 5}, {3, 2, 5}, {0, 3, 5},
    };
    std::vector<unsigned> idx;
    idx.reserve(count * kParticleTris * 3);
    for (std::size_t p = 0; p < count; ++p) {
        const unsigned base = unsigned(p * kParticleVerts);
        for (const auto& t : kTri) {
            idx.push_back(base + t[0]);
            idx.push_back(base + t[1]);
            idx.push_back(base + t[2]);
        }
    }
    return idx;
}

void particleMeshVertices(const std::vector<float>& points, double radius,
                          std::vector<float>& pos, std::vector<float>& nrm) {
    static const float kDir[kParticleVerts][3] = {
        {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
    };
    const std::size_t count = points.size() / 3;
    pos.resize(count * kParticleVerts * 3);
    nrm.resize(count * kParticleVerts * 3);
    const float r = float(radius);
    std::size_t o = 0;
    for (std::size_t p = 0; p < count; ++p) {
        const float cx = points[p * 3], cy = points[p * 3 + 1], cz = points[p * 3 + 2];
        for (const auto& dir : kDir) {
            pos[o] = cx + dir[0] * r;
            pos[o + 1] = cy + dir[1] * r;
            pos[o + 2] = cz + dir[2] * r;
            nrm[o] = dir[0];
            nrm[o + 1] = dir[1];
            nrm[o + 2] = dir[2];
            o += 3;
        }
    }
}

}  // namespace fluid
}  // namespace wizengine
