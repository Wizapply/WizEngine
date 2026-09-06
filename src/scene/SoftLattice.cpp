#include "scene/SoftLattice.h"

#include <algorithm>
#include <cmath>

namespace wizengine {
namespace softlattice {
namespace {

// 立方体の表面 [-1,1]^3（|u|∞ = 1）の点を単位球面へ写す（spherified
// cube）。面の中央も角も極端に潰れない標準的な写像。
Vec3 spherifySurface(const Vec3& u) {
    const double x = u[0], y = u[1], z = u[2];
    const double x2 = x * x, y2 = y * y, z2 = z * z;
    return {x * std::sqrt(1.0 - y2 * 0.5 - z2 * 0.5 + y2 * z2 / 3.0),
            y * std::sqrt(1.0 - z2 * 0.5 - x2 * 0.5 + z2 * x2 / 3.0),
            z * std::sqrt(1.0 - x2 * 0.5 - y2 * 0.5 + x2 * y2 / 3.0)};
}

// 立方体の内部の点を球の内部へ。格子の各殻（|u|∞ = 一定）を、その値を
// 半径とする球殻へ移す: 殻の表面に正規化してから球面へ写し、半径を掛け
// 戻す。上の式をそのまま内側の点に使うと殻が外へ膨らみ、角の粒子が球面を
// はみ出す。この写像なら一番外の殻の粒子は半径 R(1 - 1/n) に並び、当たり
// 半径 R/n を足すとちょうど直径どおりの球になる（箱と同じ関係）。
Vec3 spherify(const Vec3& u) {
    const double shell = std::max(std::abs(u[0]), std::max(std::abs(u[1]), std::abs(u[2])));
    if (shell < 1e-12) return u;  // 中心の粒子（n が奇数のとき）
    const Vec3 s = spherifySurface({u[0] / shell, u[1] / shell, u[2] / shell});
    return {s[0] * shell, s[1] * shell, s[2] * shell};
}

double dist(const Vec3& a, const Vec3& b) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

Lattice build(const editor::BodyDesc& descIn) {
    const editor::BodyDesc desc = editor::clampBody(descIn);
    const editor::SoftDesc soft = editor::clampSoft(desc.soft);
    Lattice l;
    const int n = soft.resolution;
    l.n = n;

    // ---- 粒子の静止位置 -----------------------------------------------------
    // 箱: 各辺を n 等分したセルの中心。球: 同じ格子を球へ写す（直径は size.x）。
    const bool sphere = desc.shape == editor::ShapeKind::Sphere;
    const double sx = desc.size.x;
    const double sy = sphere ? desc.size.x : desc.size.y;
    const double sz = sphere ? desc.size.x : desc.size.z;
    const double hx = sx / n, hy = sy / n, hz = sz / n;
    auto id = [n](int i, int j, int k) { return (i * n + j) * n + k; };

    l.rest.resize(std::size_t(n) * std::size_t(n) * std::size_t(n));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            for (int k = 0; k < n; ++k) {
                Vec3 p{-sx * 0.5 + hx * (i + 0.5), -sy * 0.5 + hy * (j + 0.5),
                       -sz * 0.5 + hz * (k + 0.5)};
                if (sphere) {
                    const double r = sx * 0.5;
                    const Vec3 s = spherify({p[0] / r, p[1] / r, p[2] / r});
                    p = {s[0] * r, s[1] * r, s[2] * r};
                }
                l.rest[std::size_t(id(i, j, k))] = p;
            }
        }
    }

    // 当たり半径はセルの半分より少し小さく（隣と接触させない）。球は写像で
    // 角の付近が詰まるので粒子どうしが重なるが、同じソフトボディの粒子は
    // 衝突しない設定にするので害は無い（PhysicsWorld::addSoftBody）。
    const double h = std::min(hx, std::min(hy, hz));
    l.radius = 0.5 * h * 0.92;
    l.particleMass = desc.mass / double(l.rest.size());

    // ---- ばね ---------------------------------------------------------------
    // 硬さは弾性率相当 E (Pa) で持ち、ばね定数は k = E × 格子間隔にする
    // （角柱 1 本の k = E A / L で A = h^2, L = h）。解像度を変えても
    // 「材料の硬さ」が変わらないようにするため。減衰は減衰比 ζ から
    // c = 2 ζ sqrt(k m_eff)（m_eff = 粒子 2 個の換算質量 = m/2）。
    const double hRef = (hx + hy + hz) / 3.0;
    const double kStruct = soft.stiffness * hRef;
    const double mEff = l.particleMass * 0.5;
    auto addSpring = [&](int a, int b, SpringKind kind, double factor) {
        Spring s;
        s.a = a;
        s.b = b;
        s.kind = kind;
        s.rest = dist(l.rest[std::size_t(a)], l.rest[std::size_t(b)]);
        s.k = kStruct * factor;
        s.c = 2.0 * soft.damping * std::sqrt(std::max(s.k, 0.0) * mEff);
        l.springs.push_back(s);
    };
    // オフセットは「正方向が先」に揃えてあるので、同じ組が 2 回出ない。
    struct Offset {
        int di, dj, dk;
        SpringKind kind;
    };
    const Offset offsets[] = {
        {1, 0, 0, SpringKind::Structural},  {0, 1, 0, SpringKind::Structural},
        {0, 0, 1, SpringKind::Structural},
        {1, 1, 0, SpringKind::Shear},       {1, -1, 0, SpringKind::Shear},
        {1, 0, 1, SpringKind::Shear},       {1, 0, -1, SpringKind::Shear},
        {0, 1, 1, SpringKind::Shear},       {0, 1, -1, SpringKind::Shear},
        {1, 1, 1, SpringKind::Shear},       {1, 1, -1, SpringKind::Shear},
        {1, -1, 1, SpringKind::Shear},      {1, -1, -1, SpringKind::Shear},
        {2, 0, 0, SpringKind::Bend},        {0, 2, 0, SpringKind::Bend},
        {0, 0, 2, SpringKind::Bend},
    };
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            for (int k = 0; k < n; ++k) {
                for (const Offset& o : offsets) {
                    const int ii = i + o.di, jj = j + o.dj, kk = k + o.dk;
                    if (ii < 0 || jj < 0 || kk < 0 || ii >= n || jj >= n ||
                        kk >= n) {
                        continue;
                    }
                    const double factor = o.kind == SpringKind::Structural
                                              ? 1.0
                                              : (o.kind == SpringKind::Shear
                                                     ? soft.shear
                                                     : soft.bend);
                    if (factor <= 0.0) continue;
                    addSpring(id(i, j, k), id(ii, jj, kk), o.kind, factor);
                }
            }
        }
    }

    // ---- 表面 ---------------------------------------------------------------
    // 表面粒子 = どれかの添字が 0 か n-1。頂点は粒子ごとに 1 個（面をまたいで
    // 共有）で、法線は隣接三角形の平均 = 角が少し丸く見える。柔らかい物の
    // 見た目としては都合がよい。
    std::vector<int> vertexOf(l.rest.size(), -1);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            for (int k = 0; k < n; ++k) {
                const bool onSurface = i == 0 || j == 0 || k == 0 || i == n - 1 ||
                                       j == n - 1 || k == n - 1;
                if (!onSurface) continue;
                vertexOf[std::size_t(id(i, j, k))] = int(l.surface.size());
                l.surface.push_back(id(i, j, k));
            }
        }
    }
    // 6 面それぞれを (n-1)^2 枚の四角 = 2 三角形で張る。固定する軸 a と
    // 残りの 2 軸 (b, c) を巡回順（x→yz, y→zx, z→xy）に取ると、(b, c) の
    // 順で組んだ三角形の法線が +a 側で外向きになる。-a 側は巻きを逆にする。
    auto idx = [&](int a, int side, int u, int v) {
        int c[3];
        c[a] = side;
        c[(a + 1) % 3] = u;
        c[(a + 2) % 3] = v;
        return vertexOf[std::size_t(id(c[0], c[1], c[2]))];
    };
    for (int a = 0; a < 3; ++a) {
        for (int s = 0; s < 2; ++s) {
            const int side = s == 0 ? 0 : n - 1;
            for (int u = 0; u + 1 < n; ++u) {
                for (int v = 0; v + 1 < n; ++v) {
                    const std::uint32_t p00 = std::uint32_t(idx(a, side, u, v));
                    const std::uint32_t p10 = std::uint32_t(idx(a, side, u + 1, v));
                    const std::uint32_t p11 =
                        std::uint32_t(idx(a, side, u + 1, v + 1));
                    const std::uint32_t p01 = std::uint32_t(idx(a, side, u, v + 1));
                    if (s == 1) {  // +a 側: (u, v) の順が外向き
                        l.indices.insert(l.indices.end(),
                                         {p00, p10, p11, p00, p11, p01});
                    } else {       // -a 側: 逆巻き
                        l.indices.insert(l.indices.end(),
                                         {p00, p11, p10, p00, p01, p11});
                    }
                }
            }
        }
    }
    return l;
}

void buildSurface(const Lattice& l, const float* particles,
                  std::vector<float>& positions, std::vector<float>& normals) {
    buildSurfaceMesh(l.surface.data(), l.surface.size(), l.indices.data(),
                     l.indices.size(), l.radius, particles, positions, normals);
}

void buildSurfaceMesh(const int* surface, std::size_t nv, const std::uint32_t* indices,
                      std::size_t indexCount, double radius, const float* particles,
                      std::vector<float>& positions, std::vector<float>& normals) {
    positions.assign(nv * 3, 0.0f);
    normals.assign(nv * 3, 0.0f);
    if (nv == 0) return;

    // 頂点 = 表面粒子の中心（まず法線を貯める）。
    for (std::size_t v = 0; v < nv; ++v) {
        const std::size_t pi = surface ? std::size_t(surface[v]) : v;
        const float* p = particles + pi * 3;
        positions[v * 3 + 0] = p[0];
        positions[v * 3 + 1] = p[1];
        positions[v * 3 + 2] = p[2];
    }
    // 三角形の法線（面積重み付き = 外積そのまま）を頂点へ足し込む。
    for (std::size_t t = 0; t + 2 < indexCount; t += 3) {
        const std::uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
        if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
        const float* a = &positions[i0 * 3];
        const float* b = &positions[i1 * 3];
        const float* c = &positions[i2 * 3];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float nx = e1[1] * e2[2] - e1[2] * e2[1];
        const float ny = e1[2] * e2[0] - e1[0] * e2[2];
        const float nz = e1[0] * e2[1] - e1[1] * e2[0];
        for (const std::uint32_t i : {i0, i1, i2}) {
            normals[i * 3 + 0] += nx;
            normals[i * 3 + 1] += ny;
            normals[i * 3 + 2] += nz;
        }
    }
    // 正規化して、粒子の球の外側まで表面を押し出す。
    const float r = float(radius);
    for (std::size_t v = 0; v < nv; ++v) {
        float* nrm = &normals[v * 3];
        const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
        if (len > 1e-12f) {
            nrm[0] /= len;
            nrm[1] /= len;
            nrm[2] /= len;
        } else {
            nrm[0] = 0.0f;
            nrm[1] = 1.0f;
            nrm[2] = 0.0f;
        }
        positions[v * 3 + 0] += nrm[0] * r;
        positions[v * 3 + 1] += nrm[1] * r;
        positions[v * 3 + 2] += nrm[2] * r;
    }
}

}  // namespace softlattice
}  // namespace wizengine
