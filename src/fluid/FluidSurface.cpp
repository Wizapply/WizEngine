#include "fluid/FluidSurface.h"

#include <algorithm>
#include <cmath>

namespace wizengine {
namespace fluid {

namespace {

// 等値面の値（場の山 1 個の頂上 = 1）。粒子の間隔 d・山の半径 R = 1.4 d の六方最密の
// 塊で、平らな表面が外側の粒子の中心から約 0.6 d 外に来る値（塊の中の場は約 2.5。
// 単体テストで確かめた）。孤立した 1 粒子（しぶき）は半径 R √(1 − iso^⅓) ≈ 0.42 d の
// 水滴として残る（iso を 1 以上にすると、しぶきが消える）。
constexpr double kIso = 0.75;

inline std::size_t nodeIndex(int i, int j, int k, int nx, int ny) {
    return (std::size_t(k) * std::size_t(ny) + std::size_t(j)) * std::size_t(nx) +
           std::size_t(i);
}

}  // namespace

double fluidSurfaceIso() { return kIso; }

void buildFluidSurface(const std::vector<float>& points, const SurfaceParams& params,
                       SurfaceMesh& out, SurfaceScratch& scratch) {
    out.positions.clear();
    out.normals.clear();
    out.indices.clear();
    const std::size_t count = points.size() / 3;
    if (count == 0 || !(params.spacing > 0.0)) return;

    // ---- 格子の範囲（粒子の外接箱 + 山の半径 + 1 セル）---------------------------
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (std::size_t p = 0; p < count; ++p) {
        for (int a = 0; a < 3; ++a) {
            const float v = points[p * 3 + a];
            if (!std::isfinite(v)) continue;
            lo[a] = std::min(lo[a], v);
            hi[a] = std::max(hi[a], v);
        }
    }
    if (lo[0] > hi[0]) return;  // 有限な粒子が無い
    const double R = std::max(1e-6, params.radius * params.spacing);
    double h = std::max(1e-6, params.cell * params.spacing);
    int n[3];
    double origin[3];
    for (int tries = 0; tries < 16; ++tries) {
        std::size_t total = 1;
        for (int a = 0; a < 3; ++a) {
            origin[a] = double(lo[a]) - R - h;
            n[a] = int(std::ceil((double(hi[a]) - double(lo[a]) + 2.0 * (R + h)) / h)) + 1;
            total *= std::size_t(std::max(n[a], 2));
        }
        if (total <= params.maxCells || params.maxCells == 0) break;
        h *= std::cbrt(double(total) / double(params.maxCells)) * 1.02;  // 粗くする
    }
    out.cellSize = h;
    const int nx = n[0], ny = n[1], nz = n[2];
    const std::size_t nodes = std::size_t(nx) * std::size_t(ny) * std::size_t(nz);

    // ---- 場を溜める（粒子ごとに半径 R の中の節点へ山を足す）------------------------
    std::vector<float>& f = scratch.field;
    f.assign(nodes, 0.0f);
    const double invR2 = 1.0 / (R * R);
    const int reach = int(std::ceil(R / h));
    for (std::size_t p = 0; p < count; ++p) {
        const double px = points[p * 3], py = points[p * 3 + 1], pz = points[p * 3 + 2];
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;
        const int ci = int(std::floor((px - origin[0]) / h));
        const int cj = int(std::floor((py - origin[1]) / h));
        const int ck = int(std::floor((pz - origin[2]) / h));
        const int i0 = std::max(0, ci - reach + 1), i1 = std::min(nx - 1, ci + reach);
        const int j0 = std::max(0, cj - reach + 1), j1 = std::min(ny - 1, cj + reach);
        const int k0 = std::max(0, ck - reach + 1), k1 = std::min(nz - 1, ck + reach);
        for (int k = k0; k <= k1; ++k) {
            const double dz = origin[2] + k * h - pz;
            const double dz2 = dz * dz;
            if (dz2 >= R * R) continue;
            for (int j = j0; j <= j1; ++j) {
                const double dy = origin[1] + j * h - py;
                const double dyz2 = dy * dy + dz2;
                if (dyz2 >= R * R) continue;
                float* row = &f[nodeIndex(0, j, k, nx, ny)];
                for (int i = i0; i <= i1; ++i) {
                    const double dx = origin[0] + i * h - px;
                    const double q2 = (dx * dx + dyz2) * invR2;
                    if (q2 >= 1.0) continue;
                    const double t = 1.0 - q2;
                    row[i] += float(t * t * t);
                }
            }
        }
    }

    // ---- 頂点: 等値面をまたぐセルごとに 1 個（辺の交点の平均）---------------------
    const float iso = float(kIso);
    const int cx = nx - 1, cy = ny - 1, cz = nz - 1;
    std::vector<int32_t>& cellVertex = scratch.cellVertex;
    cellVertex.assign(std::size_t(cx) * std::size_t(cy) * std::size_t(cz), -1);
    auto cellIndex = [cx, cy](int i, int j, int k) {
        return (std::size_t(k) * std::size_t(cy) + std::size_t(j)) * std::size_t(cx) +
               std::size_t(i);
    };
    // セルの 8 隅（i, j, k のビット）と 12 辺（隅の組）。
    static const int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                      {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                     {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k < cz; ++k) {
        for (int j = 0; j < cy; ++j) {
            for (int i = 0; i < cx; ++i) {
                float v[8];
                int mask = 0;
                for (int c = 0; c < 8; ++c) {
                    v[c] = f[nodeIndex(i + kCorner[c][0], j + kCorner[c][1], k + kCorner[c][2],
                                       nx, ny)];
                    if (v[c] > iso) mask |= 1 << c;
                }
                if (mask == 0 || mask == 255) continue;
                double sx = 0, sy = 0, sz = 0;
                int crossings = 0;
                for (const auto& e : kEdge) {
                    const bool a = (mask >> e[0]) & 1, b = (mask >> e[1]) & 1;
                    if (a == b) continue;
                    const double t = (iso - v[e[0]]) / (v[e[1]] - v[e[0]]);
                    sx += kCorner[e[0]][0] + t * (kCorner[e[1]][0] - kCorner[e[0]][0]);
                    sy += kCorner[e[0]][1] + t * (kCorner[e[1]][1] - kCorner[e[0]][1]);
                    sz += kCorner[e[0]][2] + t * (kCorner[e[1]][2] - kCorner[e[0]][2]);
                    ++crossings;
                }
                cellVertex[cellIndex(i, j, k)] = int32_t(out.positions.size() / 3);
                out.positions.push_back(float(origin[0] + (i + sx / crossings) * h));
                out.positions.push_back(float(origin[1] + (j + sy / crossings) * h));
                out.positions.push_back(float(origin[2] + (k + sz / crossings) * h));
            }
        }
    }

    // ---- 面: 等値面をまたぐ格子の辺ごとに、周りの 4 セルの頂点で四角形 ------------------
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d, int axis, bool startInside) {
        if (a < 0 || b < 0 || c < 0 || d < 0) return;
        const float* P = out.positions.data();
        // 対角線の外積 = 四角形の法線。外向き = 外側（場が小さい）の節点の側。
        const double ux = P[c * 3] - P[a * 3], uy = P[c * 3 + 1] - P[a * 3 + 1],
                     uz = P[c * 3 + 2] - P[a * 3 + 2];
        const double vx = P[d * 3] - P[b * 3], vy = P[d * 3 + 1] - P[b * 3 + 1],
                     vz = P[d * 3 + 2] - P[b * 3 + 2];
        const double nrm[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
        const double outward = startInside ? 1.0 : -1.0;  // 辺の向き（+axis）に対して
        if (nrm[axis] * outward < 0.0) std::swap(b, d);
        out.indices.insert(out.indices.end(), {uint32_t(a), uint32_t(b), uint32_t(c),
                                               uint32_t(a), uint32_t(c), uint32_t(d)});
    };
    for (int k = 1; k < nz - 1; ++k) {
        for (int j = 1; j < ny - 1; ++j) {
            for (int i = 1; i < nx - 1; ++i) {
                const float v0 = f[nodeIndex(i, j, k, nx, ny)];
                const bool in0 = v0 > iso;
                // +X の辺: 周りのセルは (i, j-1..j, k-1..k)
                if ((f[nodeIndex(i + 1, j, k, nx, ny)] > iso) != in0) {
                    quad(cellVertex[cellIndex(i, j - 1, k - 1)], cellVertex[cellIndex(i, j, k - 1)],
                         cellVertex[cellIndex(i, j, k)], cellVertex[cellIndex(i, j - 1, k)], 0, in0);
                }
                // +Y の辺: 周りのセルは (i-1..i, j, k-1..k)
                if (j < cy && (f[nodeIndex(i, j + 1, k, nx, ny)] > iso) != in0) {
                    quad(cellVertex[cellIndex(i - 1, j, k - 1)], cellVertex[cellIndex(i, j, k - 1)],
                         cellVertex[cellIndex(i, j, k)], cellVertex[cellIndex(i - 1, j, k)], 1, in0);
                }
                // +Z の辺: 周りのセルは (i-1..i, j-1..j, k)
                if (k < cz && (f[nodeIndex(i, j, k + 1, nx, ny)] > iso) != in0) {
                    quad(cellVertex[cellIndex(i - 1, j - 1, k)], cellVertex[cellIndex(i, j - 1, k)],
                         cellVertex[cellIndex(i, j, k)], cellVertex[cellIndex(i - 1, j, k)], 2, in0);
                }
            }
        }
    }

    const std::size_t vcount = out.positions.size() / 3;
    // ---- ならし（つながった頂点の平均へ半分寄せる。格子の段々を消す）----------------
    if (params.smooth > 0 && vcount > 0) {
        std::vector<float> sum(vcount * 3), tmp;
        std::vector<uint16_t> deg(vcount);
        for (int pass = 0; pass < params.smooth; ++pass) {
            std::fill(sum.begin(), sum.end(), 0.0f);
            std::fill(deg.begin(), deg.end(), uint16_t(0));
            for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
                const uint32_t tri[3] = {out.indices[t], out.indices[t + 1], out.indices[t + 2]};
                for (int e = 0; e < 3; ++e) {
                    const uint32_t a = tri[e], b = tri[(e + 1) % 3];
                    for (int c = 0; c < 3; ++c) {
                        sum[a * 3 + c] += out.positions[b * 3 + c];
                        sum[b * 3 + c] += out.positions[a * 3 + c];
                    }
                    if (deg[a] < 60000) ++deg[a];
                    if (deg[b] < 60000) ++deg[b];
                }
            }
            for (std::size_t v = 0; v < vcount; ++v) {
                if (deg[v] == 0) continue;
                for (int c = 0; c < 3; ++c) {
                    const float avg = sum[v * 3 + c] / float(deg[v]);
                    out.positions[v * 3 + c] = 0.5f * (out.positions[v * 3 + c] + avg);
                }
            }
        }
    }

    // ---- 法線: 隣り合う三角形の面積で重み付けた平均（外向き）--------------------------
    out.normals.assign(vcount * 3, 0.0f);
    for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
        const uint32_t a = out.indices[t], b = out.indices[t + 1], c = out.indices[t + 2];
        const float* P = out.positions.data();
        const float ux = P[b * 3] - P[a * 3], uy = P[b * 3 + 1] - P[a * 3 + 1],
                    uz = P[b * 3 + 2] - P[a * 3 + 2];
        const float vx = P[c * 3] - P[a * 3], vy = P[c * 3 + 1] - P[a * 3 + 1],
                    vz = P[c * 3 + 2] - P[a * 3 + 2];
        const float nn[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
        for (const uint32_t v : {a, b, c}) {
            out.normals[v * 3] += nn[0];
            out.normals[v * 3 + 1] += nn[1];
            out.normals[v * 3 + 2] += nn[2];
        }
    }
    for (std::size_t v = 0; v < vcount; ++v) {
        float* nn = &out.normals[v * 3];
        const float len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (len > 1e-20f) {
            nn[0] /= len;
            nn[1] /= len;
            nn[2] /= len;
        } else {
            nn[0] = 0.0f;
            nn[1] = 1.0f;
            nn[2] = 0.0f;
        }
    }
}

}  // namespace fluid
}  // namespace wizengine
