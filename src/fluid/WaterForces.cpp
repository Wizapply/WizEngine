#include "fluid/WaterForces.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wizengine {
namespace fluid {

namespace {

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// 種と番号から 0..1 の一様な値（splitmix64。成分の向きと位相を決める）。
double hash01(unsigned seed, int i, int salt) {
    std::uint64_t z = (std::uint64_t(seed) << 32) ^ (std::uint64_t(unsigned(i)) << 8) ^
                      std::uint64_t(unsigned(salt));
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return double(z >> 11) * (1.0 / 9007199254740992.0);
}

// Pierson-Moskowitz のスペクトル S(ω) = α g² ω⁻⁵ exp(−1.25 (ωp/ω)⁴)。
constexpr double kPmAlpha = 0.0081;
double pmSpectrum(double omega, double omegaPeak, double g) {
    const double r = omegaPeak / omega;
    return kPmAlpha * g * g / std::pow(omega, 5.0) * std::exp(-1.25 * r * r * r * r);
}

// 静止位置 x0 の水平の寄り（−Σ q A d sin θ）。
void horizontalShift(const WaterParams& w, double x0, double z0, double t, double& sx,
                     double& sz) {
    sx = 0.0;
    sz = 0.0;
    for (const WaveComponent& c : w.waves) {
        if (c.q <= 0.0) continue;
        const double th = c.k * (c.dx * x0 + c.dz * z0) - c.omega * t + c.phase;
        const double s = c.q * c.amplitude * std::sin(th);
        sx -= s * c.dx;
        sz -= s * c.dz;
    }
}

// 真上の点 (x, z) に来る静止位置 x0 を反復で探す（x0 = x − shift(x0)。寄りの微分は
// choppy の頭打ちで 0.9 以下なので縮小写像 = 収束する）。
void restPosition(const WaterParams& w, double x, double z, double t, double& x0, double& z0) {
    x0 = x;
    z0 = z;
    if (w.maxCompression <= 0.0) return;  // 水平に動かない
    for (int it = 0; it < 4; ++it) {
        double sx, sz;
        horizontalShift(w, x0, z0, t, sx, sz);
        x0 = x - sx;
        z0 = z - sz;
    }
}

}  // namespace

void prepareWaves(WaterParams& w, double gravity) {
    w.waves.clear();
    w.crest = 0.0;
    w.maxCompression = 0.0;
    const double g = std::fabs(gravity) > 1e-6 ? std::fabs(gravity) : 9.81;
    const double hx = std::cos(w.heading), hz = std::sin(w.heading);

    // うねり（手で決める 1 本）。
    if (w.amplitude > 0.0 && w.period > 0.0) {
        WaveComponent c;
        c.dx = hx;
        c.dz = hz;
        c.omega = 2.0 * kPi / w.period;
        c.k = c.omega * c.omega / g;
        c.amplitude = w.amplitude;
        w.waves.push_back(c);
    }

    // 風浪: Pierson-Moskowitz のピーク ωp = 0.877 g / U の 0.6〜3 倍を対数で等分し、
    // 各区間の中に 1 本（区間の中の位置は種でずらす）。振幅は √(2 S Δω) で出し、
    // 最後に全体の分散を理論値 m0 = α g² / (5 ωp⁴) へ合わせる（有義波高 = 0.21 U²/g）。
    const int n = std::max(0, std::min(64, w.waveCount));
    if (w.wind > 0.01 && n > 0) {
        const double wp = 0.877 * g / w.wind;
        const double lo = std::log(0.6), hi = std::log(3.0);
        const std::size_t first = w.waves.size();
        double var = 0.0;
        for (int i = 0; i < n; ++i) {
            const double a0 = lo + (hi - lo) * double(i) / double(n);
            const double a1 = lo + (hi - lo) * double(i + 1) / double(n);
            const double u = 0.15 + 0.7 * hash01(w.seed, i, 1);
            const double omega = wp * std::exp(a0 + (a1 - a0) * u);
            const double dOmega = wp * (std::exp(a1) - std::exp(a0));
            WaveComponent c;
            // 向き: 周波数の低い成分ほど風の向きに揃い、高い成分ほどばらつく。
            const double widen = 0.5 + 0.5 * double(i) / double(std::max(1, n - 1));
            const double ang = w.heading + w.spread * widen * (2.0 * hash01(w.seed, i, 2) - 1.0);
            c.dx = std::cos(ang);
            c.dz = std::sin(ang);
            c.omega = omega;
            c.k = omega * omega / g;
            c.amplitude = std::sqrt(2.0 * pmSpectrum(omega, wp, g) * dOmega);
            c.phase = 2.0 * kPi * hash01(w.seed, i, 3);
            var += 0.5 * c.amplitude * c.amplitude;
            w.waves.push_back(c);
        }
        const double m0 = kPmAlpha * g * g / (5.0 * wp * wp * wp * wp);
        if (var > 0.0) {
            const double scale = std::sqrt(m0 / var);
            for (std::size_t i = first; i < w.waves.size(); ++i) w.waves[i].amplitude *= scale;
        }
    }

    // 尖り方: Σ q k A < 1 なら波面は折り返さない。0.9 で頭打ち。
    double sumKA = 0.0;
    for (const WaveComponent& c : w.waves) {
        sumKA += c.k * c.amplitude;
        w.crest += c.amplitude;
    }
    double q = std::max(0.0, w.choppy);
    if (sumKA > 0.0 && q * sumKA > 0.9) q = 0.9 / sumKA;
    for (WaveComponent& c : w.waves) c.q = q;
    w.maxCompression = q * sumKA;
}

double significantWaveHeight(const WaterParams& w) {
    double var = 0.0;
    for (const WaveComponent& c : w.waves) var += 0.5 * c.amplitude * c.amplitude;
    return 4.0 * std::sqrt(var);
}

SurfacePoint surfacePoint(const WaterParams& w, double x0, double z0, double t) {
    SurfacePoint p;
    double eta = 0.0, sx = 0.0, sz = 0.0;
    // 写像 P(x0) の偏微分（∂P/∂x0 = (1 − Sxx, Hx, −Sxz)、∂P/∂z0 = (−Sxz, Hz, 1 − Szz)）。
    double sxx = 0.0, szz = 0.0, sxz = 0.0, hxd = 0.0, hzd = 0.0;
    for (const WaveComponent& c : w.waves) {
        const double th = c.k * (c.dx * x0 + c.dz * z0) - c.omega * t + c.phase;
        const double cs = std::cos(th), sn = std::sin(th);
        eta += c.amplitude * cs;
        const double qa = c.q * c.amplitude;
        sx -= qa * sn * c.dx;
        sz -= qa * sn * c.dz;
        const double kc = qa * c.k * cs;
        sxx += kc * c.dx * c.dx;
        szz += kc * c.dz * c.dz;
        sxz += kc * c.dx * c.dz;
        const double ks = c.amplitude * c.k * sn;
        hxd -= ks * c.dx;
        hzd -= ks * c.dz;
    }
    p.position = {x0 + sx, w.level + eta, z0 + sz};
    p.height = eta;
    const Vec3 tx{1.0 - sxx, hxd, -sxz};
    const Vec3 tz{-sxz, hzd, 1.0 - szz};
    Vec3 nrm = tz.cross(tx);
    const double len = nrm.length();
    p.normal = len > 1e-12 ? nrm / len : Vec3{0.0, 1.0, 0.0};
    p.compression = 1.0 - ((1.0 - sxx) * (1.0 - szz) - sxz * sxz);
    return p;
}

double surfaceHeight(const WaterParams& w, double x, double z, double t) {
    if (w.waves.empty()) return w.level;
    double x0, z0;
    restPosition(w, x, z, t, x0, z0);
    double eta = 0.0;
    for (const WaveComponent& c : w.waves) {
        eta += c.amplitude * std::cos(c.k * (c.dx * x0 + c.dz * z0) - c.omega * t + c.phase);
    }
    return w.level + eta;
}

Vec3 surfaceNormal(const WaterParams& w, double x, double z, double t) {
    if (w.waves.empty()) return {0.0, 1.0, 0.0};
    double x0, z0;
    restPosition(w, x, z, t, x0, z0);
    return surfacePoint(w, x0, z0, t).normal;
}

Vec3 waterVelocity(const WaterParams& w, const Vec3& pos, double t) {
    Vec3 v = w.flow;
    // 深水波の水粒子の速度（線形理論 = 成分ごとに）: 水平 Aω cos θ・鉛直 Aω sin θ、
    // 深さ ζ（静水面から下が負）で e^{kζ} に弱まる。水面より上は水面の値のまま
    // （そこに水は無いが、浮心が水面の近くにあるときに値が跳ねないように）。
    const double zeta = std::min(0.0, pos.y - w.level);
    for (const WaveComponent& c : w.waves) {
        const double th = c.k * (c.dx * pos.x + c.dz * pos.z) - c.omega * t + c.phase;
        const double a = c.amplitude * c.omega * std::exp(c.k * zeta);
        const double cs = std::cos(th);
        v += Vec3{a * cs * c.dx, a * std::sin(th), a * cs * c.dz};
    }
    return v;
}

bool insideWater(const WaterParams& w, double x, double z) {
    return std::fabs(x - w.centerX) <= w.halfX && std::fabs(z - w.centerZ) <= w.halfZ;
}

double foamAmount(const WaterParams& w, const SurfacePoint& s, double amount) {
    if (amount <= 0.0 || w.waves.empty()) return 0.0;
    // 白波は「波頭で面が詰まっている」所 = 面の縮み 1 − J が大きい所に立つ（絶対値で
    // 見る: 穏やかなうねりの波頭は白くならない）。amount 0.4 で縮み 0.18 から、
    // 1 で 0.03 から白くし始める（海のスペクトルで choppy 1 の縮みは最大 0.2 前後）。
    const double start = 0.03 + 0.25 * (1.0 - clamp01(amount));
    double f = clamp01((s.compression - start) / 0.1);
    // 風浪の白波は風が弱いと立たない（3 m/s から 8 m/s にかけて増やす）。
    if (w.wind > 0.0) f *= clamp01((w.wind - 3.0) / 5.0);
    return f;
}

WaterLoad computeWaterLoad(const WaterParams& w, const BodyState& b, const Vec3& gravity,
                           double t, double dt, int samplesPerAxis) {
    WaterLoad out;

    // ---- 押しのけた体積と浮心 -------------------------------------------------
    Vec3 weighted{0.0, 0.0, 0.0};  // Σ (水面下の体積 × その重心)
    double vsub = 0.0;
    double charLen = 0.0;  // 代表長さ（回転の抵抗）
    if (b.shape == BodyState::Shape::Sphere) {
        const double r = std::max(1e-6, b.radius);
        out.volume = 4.0 / 3.0 * kPi * r * r * r;
        charLen = 2.0 * r;
        if (insideWater(w, b.position.x, b.position.z)) {
            const double eta = surfaceHeight(w, b.position.x, b.position.z, t);
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
                    const double eta = surfaceHeight(w, c.x, c.z, t);
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
    const Vec3 u = vcb - waterVelocity(w, cb, t);
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

void waterGridVertices(const WaterParams& w, int cells, double t, double foam,
                       std::vector<float>& pos, std::vector<float>& nrm,
                       std::vector<float>& foamOut) {
    const int n = std::max(1, cells);
    const std::size_t count = std::size_t(n + 1) * std::size_t(n + 1);
    pos.resize(count * 3);
    nrm.resize(count * 3);
    foamOut.assign(count, 0.0f);
    std::size_t o = 0;
    for (int j = 0; j <= n; ++j) {
        const double z = w.centerZ - w.halfZ + 2.0 * w.halfZ * double(j) / double(n);
        for (int i = 0; i <= n; ++i) {
            const double x = w.centerX - w.halfX + 2.0 * w.halfX * double(i) / double(n);
            SurfacePoint sp;
            if (i == 0 || j == 0 || i == n || j == n) {
                // 縁は水平に動かさない: 真上の点の高さと法線を逆にたどって合わせる。
                double x0, z0;
                restPosition(w, x, z, t, x0, z0);
                sp = surfacePoint(w, x0, z0, t);
                sp.position.x = x;
                sp.position.z = z;
            } else {
                sp = surfacePoint(w, x, z, t);
            }
            pos[o] = float(sp.position.x);
            pos[o + 1] = float(sp.position.y);
            pos[o + 2] = float(sp.position.z);
            nrm[o] = float(sp.normal.x);
            nrm[o + 1] = float(sp.normal.y);
            nrm[o + 2] = float(sp.normal.z);
            foamOut[o / 3] = float(foamAmount(w, sp, foam));
            o += 3;
        }
    }
}

}  // namespace fluid
}  // namespace wizengine
