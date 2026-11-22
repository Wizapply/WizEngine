#include "fluid/WaterForces.h"

#include <algorithm>
#include <cmath>

namespace wizengine {
namespace fluid {

namespace {

// 波の位相 θ = k (d · p) − ω t と、ω・k。
struct Phase {
    double theta = 0.0;
    double omega = 0.0;
    double k = 0.0;
    double dx = 1.0, dz = 0.0;  // 進む向き（単位）
};

Phase phaseAt(const WaterParams& w, double x, double z, double t, double gravity) {
    Phase p;
    p.dx = std::cos(w.heading);
    p.dz = std::sin(w.heading);
    if (w.amplitude <= 0.0 || w.period <= 0.0) return p;
    p.omega = 2.0 * kPi / w.period;
    p.k = waveNumber(w, gravity);
    p.theta = p.k * (p.dx * x + p.dz * z) - p.omega * t;
    return p;
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

}  // namespace

double waveNumber(const WaterParams& w, double gravity) {
    if (w.period <= 0.0) return 0.0;
    const double g = std::fabs(gravity) > 1e-6 ? std::fabs(gravity) : 9.81;
    const double omega = 2.0 * kPi / w.period;
    return omega * omega / g;
}

double surfaceHeight(const WaterParams& w, double x, double z, double t, double gravity) {
    if (w.amplitude <= 0.0) return w.level;
    const Phase p = phaseAt(w, x, z, t, gravity);
    return w.level + w.amplitude * std::cos(p.theta);
}

Vec3 surfaceNormal(const WaterParams& w, double x, double z, double t, double gravity) {
    if (w.amplitude <= 0.0) return {0.0, 1.0, 0.0};
    const Phase p = phaseAt(w, x, z, t, gravity);
    // η = level + A cos θ → ∂η/∂x = −A k sin θ dx（z も同じ）。
    const double s = -w.amplitude * p.k * std::sin(p.theta);
    const Vec3 n{-s * p.dx, 1.0, -s * p.dz};
    return n / n.length();
}

Vec3 waterVelocity(const WaterParams& w, const Vec3& pos, double t, double gravity) {
    Vec3 v = w.flow;
    if (w.amplitude <= 0.0 || w.period <= 0.0) return v;
    const Phase p = phaseAt(w, pos.x, pos.z, t, gravity);
    // 深水波の水粒子の速度: 水平 Aω cos θ・鉛直 Aω sin θ、深さ ζ（静水面から下が負）
    // で e^{kζ} に弱まる。水面より上は水面の値のまま（そこに水は無いが、浮心が
    // 水面の近くにあるときに値が跳ねないように）。
    const double zeta = std::min(0.0, pos.y - w.level);
    const double a = w.amplitude * p.omega * std::exp(p.k * zeta);
    v += Vec3{a * std::cos(p.theta) * p.dx, a * std::sin(p.theta), a * std::cos(p.theta) * p.dz};
    return v;
}

bool insideWater(const WaterParams& w, double x, double z) {
    return std::fabs(x - w.centerX) <= w.halfX && std::fabs(z - w.centerZ) <= w.halfZ;
}

WaterLoad computeWaterLoad(const WaterParams& w, const BodyState& b, const Vec3& gravity,
                           double t, double dt, int samplesPerAxis) {
    WaterLoad out;
    const double gmag = gravity.length();

    // ---- 押しのけた体積と浮心 -------------------------------------------------
    Vec3 weighted{0.0, 0.0, 0.0};  // Σ (水面下の体積 × その重心)
    double vsub = 0.0;
    double charLen = 0.0;  // 代表長さ（回転の抵抗）
    if (b.shape == BodyState::Shape::Sphere) {
        const double r = std::max(1e-6, b.radius);
        out.volume = 4.0 / 3.0 * kPi * r * r * r;
        charLen = 2.0 * r;
        if (insideWater(w, b.position.x, b.position.z)) {
            const double eta = surfaceHeight(w, b.position.x, b.position.z, t, gmag);
            // 水面下にある球冠の高さ h（0 = 水面より上、2r = 全部沈んでいる）。
            const double h = std::min(2.0 * r, std::max(0.0, eta - (b.position.y - r)));
            if (h > 0.0) {
                vsub = kPi * h * h * (3.0 * r - h) / 3.0;
                // 下側の球冠の重心は中心から 3 (2r − h)² / (4 (3r − h)) だけ下。
                const double d = 3.0 * (2.0 * r - h) * (2.0 * r - h) / (4.0 * (3.0 * r - h));
                weighted = (b.position - Vec3{0.0, d, 0.0}) * vsub;
            }
        }
    } else {
        const int n = std::max(1, std::min(16, samplesPerAxis));
        const Vec3 full = b.half * 2.0;
        out.volume = full.x * full.y * full.z;
        charLen = std::cbrt(std::max(1e-12, out.volume));
        const Vec3 cell = full / double(n);
        const double dV = cell.x * cell.y * cell.z;
        // 小箱の鉛直方向の半分の広がり（回転していると軸ごとの寄与を足す）。
        const Vec3 ex = b.rotation.rotate({cell.x, 0.0, 0.0});
        const Vec3 ey = b.rotation.rotate({0.0, cell.y, 0.0});
        const Vec3 ez = b.rotation.rotate({0.0, 0.0, cell.z});
        const double hv = 0.5 * (std::fabs(ex.y) + std::fabs(ey.y) + std::fabs(ez.y));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                for (int k = 0; k < n; ++k) {
                    const Vec3 local{-b.half.x + cell.x * (i + 0.5),
                                     -b.half.y + cell.y * (j + 0.5),
                                     -b.half.z + cell.z * (k + 0.5)};
                    const Vec3 c = b.position + b.rotation.rotate(local);
                    if (!insideWater(w, c.x, c.z)) continue;
                    const double eta = surfaceHeight(w, c.x, c.z, t, gmag);
                    // 小箱の下端から水面までの割合（直線の近似 = 揺れても力が跳ねない）。
                    const double f = hv > 1e-9 ? clamp01((eta - (c.y - hv)) / (2.0 * hv))
                                               : (c.y <= eta ? 1.0 : 0.0);
                    if (f <= 0.0) continue;
                    // 水面下の部分の重心: 小箱の中心から下へ (1 − f) hv。
                    const Vec3 p = c - Vec3{0.0, hv * (1.0 - f), 0.0};
                    vsub += f * dV;
                    weighted += p * (f * dV);
                }
            }
        }
    }
    if (vsub <= 0.0) return out;
    out.submergedVolume = vsub;
    const Vec3 cb = weighted / vsub;
    out.buoyancyCenter = cb;
    const Vec3 arm = cb - b.position;

    // ---- 浮力（押しのけた水の重さを重力と逆向きに）--------------------------
    const Vec3 fb = gravity * (-w.density * vsub);
    out.force += fb;
    out.torque += arm.cross(fb);

    // ---- 抗力（浮心の水に対する速度で）---------------------------------------
    const Vec3 vcb = b.velocity + b.angularVelocity.cross(arm);
    const Vec3 u = vcb - waterVelocity(w, cb, t, gmag);
    const double speed = u.length();
    if (speed > 1e-9) {
        const double phi = std::min(1.0, vsub / std::max(1e-12, out.volume));
        double area;
        if (b.shape == BodyState::Shape::Sphere) {
            area = kPi * b.radius * b.radius;
        } else {
            const Vec3 ul = b.rotation.rotateBack(u / speed);
            const Vec3 f = b.half * 2.0;
            area = f.y * f.z * std::fabs(ul.x) + f.x * f.z * std::fabs(ul.y) +
                   f.x * f.y * std::fabs(ul.z);
        }
        area *= phi;
        Vec3 fd = u * (-(0.5 * w.density * w.drag * area * speed + w.density * vsub * w.linearDrag));
        // 安全弁: このステップで相対速度を反転させない（陽に掛けるため）。
        if (dt > 0.0 && b.mass > 0.0) {
            const double limit = b.mass * speed / dt;
            const double mag = fd.length();
            if (mag > limit) fd = fd * (limit / mag);
        }
        out.force += fd;
        out.torque += arm.cross(fd);
    }

    // ---- 回転の抵抗 -------------------------------------------------------------
    const double wmag = b.angularVelocity.length();
    if (wmag > 1e-9) {
        const double inertiaWater = w.density * vsub * charLen * charLen / 6.0;
        Vec3 tr = b.angularVelocity * (-inertiaWater * (w.angularDrag + w.drag * wmag));
        if (dt > 0.0 && b.mass > 0.0) {
            const double limit = b.mass * charLen * charLen / 6.0 * wmag / dt;
            const double mag = tr.length();
            if (mag > limit) tr = tr * (limit / mag);
        }
        out.torque += tr;
    }
    return out;
}

std::vector<unsigned> waterGridIndices(int cells) {
    const int n = std::max(1, cells);
    std::vector<unsigned> idx;
    idx.reserve(std::size_t(n) * std::size_t(n) * 6);
    auto v = [n](int i, int j) { return unsigned(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            // 上（+Y）から見て反時計回り。
            idx.push_back(v(i, j));
            idx.push_back(v(i, j + 1));
            idx.push_back(v(i + 1, j));
            idx.push_back(v(i + 1, j));
            idx.push_back(v(i, j + 1));
            idx.push_back(v(i + 1, j + 1));
        }
    }
    return idx;
}

void waterGridVertices(const WaterParams& w, int cells, double t, double gravity,
                       std::vector<float>& pos, std::vector<float>& nrm) {
    const int n = std::max(1, cells);
    const std::size_t count = std::size_t(n + 1) * std::size_t(n + 1);
    pos.resize(count * 3);
    nrm.resize(count * 3);
    std::size_t o = 0;
    for (int j = 0; j <= n; ++j) {
        const double z = w.centerZ - w.halfZ + 2.0 * w.halfZ * double(j) / double(n);
        for (int i = 0; i <= n; ++i) {
            const double x = w.centerX - w.halfX + 2.0 * w.halfX * double(i) / double(n);
            const double y = surfaceHeight(w, x, z, t, gravity);
            const Vec3 nn = surfaceNormal(w, x, z, t, gravity);
            pos[o] = float(x);
            pos[o + 1] = float(y);
            pos[o + 2] = float(z);
            nrm[o] = float(nn.x);
            nrm[o + 1] = float(nn.y);
            nrm[o + 2] = float(nn.z);
            o += 3;
        }
    }
}

}  // namespace fluid
}  // namespace wizengine
