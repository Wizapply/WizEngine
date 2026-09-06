#include "vehicle/SoftTire.h"

#include <algorithm>
#include <cmath>

namespace wizengine {
namespace vehicle {
namespace {

constexpr double kRimRadiusRatio = 0.7;   // リムのフランジ（タイヤと接する輪）の半径（タイヤ半径比）
constexpr double kRimWidthRatio = 0.8;    // フランジの x = ±(w/2) × これ
constexpr double kHubRadiusRatio = 0.35;  // ハブのリング（皿の底）の半径（タイヤ半径比）
constexpr double kHubWidthRatio = 0.55;   // ハブのリングとキャップの x = ±(w/2) × これ（皿状にへこむ）
constexpr double kMeshMass = 8.0;         // メッシュ全体の質量 (kg)。見た目の応答の速さを決める
constexpr double kMaxDeviation = 0.5;     // 静止位置からのずれの上限（半径比。安全弁）
constexpr double kAirDamping = 2.0;       // トレッドの速度減衰 (1/s)。ばね減衰とは別の弱い空気抵抗

double particleRadiusFor(const TireDesc& t) {
    const int rows = std::max(t.rows, 1);
    return std::min(0.5 * t.width / rows, 0.15 * t.radius) * 0.9;
}

// 静止形状（車輪ローカル、X = 車軸）。行 0 = 左ハブ、1 = 左リム（フランジ）、
// 2..rows+1 = トレッド、rows+2 = 右リム、rows+3 = 右ハブ。最後に中心の
// キャップ 2 個（左・右）。ハブ・リム・キャップは運動学的（車輪の姿勢そのもの）
// で、車輪を閉じた面にする - 穴が開いていると向こう側の内面が背面カリングで
// 消えて透けて見える。
void buildRest(const TireDesc& t, std::vector<Vec3>& rest, std::vector<char>& kinematic,
               double& rp) {
    const int n = std::max(t.segments, 3);
    const int rows = std::max(t.rows, 1);
    rp = particleRadiusFor(t);
    const double rTread = t.radius - rp;
    const double rRim = t.radius * kRimRadiusRatio;
    const double rHub = t.radius * kHubRadiusRatio;
    const double xRim = t.width * 0.5 * kRimWidthRatio;
    const double xHub = t.width * 0.5 * kHubWidthRatio;
    const double xEdge = t.width * 0.5 - rp;
    rest.clear();
    kinematic.clear();
    for (int r = 0; r < rows + 4; ++r) {
        double x, radius;
        bool kin = true;
        if (r == 0) {
            x = -xHub;
            radius = rHub;
        } else if (r == 1) {
            x = -xRim;
            radius = rRim;
        } else if (r == rows + 2) {
            x = xRim;
            radius = rRim;
        } else if (r == rows + 3) {
            x = xHub;
            radius = rHub;
        } else {
            const int j = r - 2;
            x = rows == 1 ? 0.0 : -xEdge + 2.0 * xEdge * double(j) / double(rows - 1);
            radius = rTread;
            kin = false;
        }
        for (int i = 0; i < n; ++i) {
            const double th = 2.0 * kPi * double(i) / double(n);
            rest.push_back(Vec3{x, radius * std::cos(th), radius * std::sin(th)});
            kinematic.push_back(kin ? 1 : 0);
        }
    }
    // 中心のキャップ（左・右）。
    rest.push_back(Vec3{-xHub, 0.0, 0.0});
    kinematic.push_back(1);
    rest.push_back(Vec3{xHub, 0.0, 0.0});
    kinematic.push_back(1);
}

}  // namespace

double SoftTire::particleRadius(const TireDesc& tire) {
    return particleRadiusFor(tire);
}

SoftTireTopology SoftTire::topology(const TireDesc& tire) {
    SoftTireTopology t;
    t.segments = std::max(tire.segments, 3);
    t.rows = std::max(tire.rows, 1);
    const int n = t.segments;
    const int stripRows = t.rows + 4;  // ハブ・リム・トレッド…・リム・ハブ
    t.vertexCount = std::size_t(stripRows) * std::size_t(n) + 2;
    // 隣り合う行を四角で結ぶ（閉じた帯）。(r, i) → r * n + i。行は +X 向き、
    // i は車軸まわりの角度向きなので、(p00, p01, p11) の巻きが外向きになる。
    for (int r = 0; r + 1 < stripRows; ++r) {
        for (int i = 0; i < n; ++i) {
            const int i1 = (i + 1) % n;
            const std::uint32_t p00 = std::uint32_t(r * n + i);
            const std::uint32_t p01 = std::uint32_t(r * n + i1);
            const std::uint32_t p10 = std::uint32_t((r + 1) * n + i);
            const std::uint32_t p11 = std::uint32_t((r + 1) * n + i1);
            t.indices.insert(t.indices.end(), {p00, p01, p11, p00, p11, p10});
        }
    }
    // 中心のキャップ: ハブのリングと中心の扇。左は -X 向き、右は +X 向き。
    const std::uint32_t centerL = std::uint32_t(stripRows * n);
    const std::uint32_t centerR = centerL + 1;
    for (int i = 0; i < n; ++i) {
        const int i1 = (i + 1) % n;
        const std::uint32_t l0 = std::uint32_t(i), l1 = std::uint32_t(i1);
        const std::uint32_t r0 = std::uint32_t((stripRows - 1) * n + i);
        const std::uint32_t r1 = std::uint32_t((stripRows - 1) * n + i1);
        t.indices.insert(t.indices.end(), {centerL, l1, l0, centerR, r0, r1});
    }
    return t;
}

void SoftTire::restParticles(const TireDesc& tire, const Vec3& center, const Quat& rot,
                             std::vector<float>& out) {
    std::vector<Vec3> rest;
    std::vector<char> kin;
    double rp = 0.0;
    buildRest(tire, rest, kin, rp);
    out.clear();
    out.reserve(rest.size() * 3);
    for (const Vec3& r : rest) {
        const Vec3 p = center + rot.rotate(r);
        out.push_back(float(p.x));
        out.push_back(float(p.y));
        out.push_back(float(p.z));
    }
}

SoftTire::SoftTire(const TireDesc& tire) : tire_(tire) {
    n_ = std::max(tire.segments, 3);
    rows_ = std::max(tire.rows, 1);
    buildRest(tire_, rest_, kinematic_, rp_);
    mass_ = kMeshMass / double(rest_.size());
    pos_ = rest_;
    vel_.assign(rest_.size(), Vec3{});
    world_ = rest_;

    // ---- ばね ---------------------------------------------------------------
    // 硬さの基準 kBase = stiffness / segments（接地面の粒子が数個まとまって
    // 荷重を受けたとき、輪全体の径方向剛性が stiffness の桁になる）。
    const double kBase = std::max(tire_.stiffness, 1.0) / double(n_);
    auto id = [this](int r, int i) { return r * n_ + ((i % n_) + n_) % n_; };
    auto add = [&](int a, int b, double k) {
        if (a == b || k <= 0.0) return;
        Spring s;
        s.a = a;
        s.b = b;
        s.rest = (rest_[std::size_t(a)] - rest_[std::size_t(b)]).length();
        s.k = k;
        const double invA = kinematic_[std::size_t(a)] ? 0.0 : 1.0 / mass_;
        const double invB = kinematic_[std::size_t(b)] ? 0.0 : 1.0 / mass_;
        const double meff = (invA + invB) > 0.0 ? 1.0 / (invA + invB) : mass_;
        s.c = 2.0 * clampd(tire_.damping, 0.0, 5.0) * std::sqrt(k * meff);
        springs_.push_back(s);
    };
    // 行 0 / rows+3 はハブ、1 / rows+2 はリム（フランジ）、2..rows+1 がトレッド。
    // ばねが要るのはトレッドと、トレッドからリムへの空気圧だけ（ハブと
    // キャップは運動学的な蓋）。
    const int rimL = 1, rimR = rows_ + 2;
    for (int j = 0; j < rows_; ++j) {
        const int r = j + 2;
        for (int i = 0; i < n_; ++i) {
            add(id(r, i), id(r, i + 1), 2.0 * kBase);        // 周方向
            add(id(r, i), id(r, i + 2), 0.5 * kBase);        // 曲げ
            if (r + 1 <= rows_ + 1) {                        // 幅方向 + 対角線
                add(id(r, i), id(r + 1, i), 2.0 * kBase);
                add(id(r, i), id(r + 1, i + 1), kBase);
                add(id(r, i + 1), id(r + 1, i), kBase);
            }
            // 空気圧: 両側のリムへ（同じ角度）。転がりを伝える斜めのばねも。
            add(id(r, i), id(rimL, i), kBase);
            add(id(r, i), id(rimR, i), kBase);
            add(id(r, i), id(rimL, i + 1), 0.5 * kBase);
            add(id(r, i), id(rimL, i - 1), 0.5 * kBase);
            add(id(r, i), id(rimR, i + 1), 0.5 * kBase);
            add(id(r, i), id(rimR, i - 1), 0.5 * kBase);
        }
    }
}

void SoftTire::reset() {
    placed_ = false;
}

// 粒子は**車輪ローカル**（X = 車軸、リムと一緒に回る座標系）で解く。ワールドで
// 解くと、車輪が 1 ステップに何度も回る速さ（40 rad/s × 1/120 s = 19°）では
// トレッドが直線で進むぶんだけ外へ膨らむ（遠心力もどきの離散化誤差）。
// ローカルならリムは動かず、地面の平面だけがタイヤの周りを回るので、
// 接地面の潰れがそのまま「転がりながら戻る変形」になる。
void SoftTire::step(const Vec3& center, const Quat& rotIn, bool grounded,
                    const Vec3& groundPoint, const Vec3& groundNormalIn, double dt) {
    const Quat rot = rotIn.normalized();
    const std::size_t count = rest_.size();
    if (!placed_ || dt <= 0.0) {
        for (std::size_t k = 0; k < count; ++k) {
            pos_[k] = rest_[k];
            vel_[k] = Vec3{};
        }
        placed_ = true;
    }
    if (dt > 0.0) {
        // ---- ばね（陰解法・ガウス・ザイデル。PhysicsWorld::solveSoftSprings と同じ式）
        const double invM = 1.0 / mass_;
        const int iterations = std::max(tire_.iterations, 1);
        for (int it = 0; it < iterations; ++it) {
            for (const Spring& s : springs_) {
                const std::size_t a = std::size_t(s.a), b = std::size_t(s.b);
                const double ia = kinematic_[a] ? 0.0 : invM;
                const double ib = kinematic_[b] ? 0.0 : invM;
                const double w = ia + ib;
                if (w <= 0.0) continue;
                const Vec3 d = pos_[b] - pos_[a];
                const double len = d.length();
                if (len < 1e-9) continue;
                const Vec3 dir = d / len;
                const double meff = 1.0 / w;
                const double vrel = (vel_[b] - vel_[a]).dot(dir);
                const double x = len - s.rest;
                const double denom = 1.0 + dt * (s.c + dt * s.k) / meff;
                const double vNew = (vrel - dt * s.k * x / meff) / denom;
                const double impulse = meff * (vNew - vrel);
                vel_[a] -= dir * (impulse * ia);
                vel_[b] += dir * (impulse * ib);
            }
        }

        // ---- 積分・地面・安全弁（すべて車輪ローカル）------------------------
        const Vec3 nL = rot.rotateBack(groundNormalIn.normalized());
        const Vec3 gpL = rot.rotateBack(groundPoint - center);
        const double damp = std::exp(-kAirDamping * dt);
        const double maxDev = kMaxDeviation * tire_.radius;
        for (std::size_t k = 0; k < count; ++k) {
            if (kinematic_[k]) continue;
            vel_[k] *= damp;
            pos_[k] += vel_[k] * dt;
            if (grounded) {
                // 接地面の平面より粒子の球がめり込まないように押し戻す。接線
                // 方向はそのまま（摩擦なし = トレッドは地面の上を滑る。見た目
                // には潰れだけが要る）。
                const double h = (pos_[k] - gpL).dot(nL) - rp_;
                if (h < 0.0) {
                    pos_[k] -= nL * h;
                    const double vn = vel_[k].dot(nL);
                    if (vn < 0.0) vel_[k] -= nL * vn;
                }
            }
            // 静止位置から離れすぎたら引き戻す（どんな入力でも輪が壊れない）。
            const Vec3 dev = pos_[k] - rest_[k];
            const double dl = dev.length();
            if (dl > maxDev) {
                pos_[k] = rest_[k] + dev * (maxDev / dl);
                vel_[k] = Vec3{};
            }
        }
    }
    // ---- ワールドへ（描画スナップショット用）----------------------------------
    world_.resize(count);
    for (std::size_t k = 0; k < count; ++k) world_[k] = center + rot.rotate(pos_[k]);
}

void SoftTire::particlesAsFloats(std::vector<float>& out) const {
    out.clear();
    out.reserve(world_.size() * 3);
    for (const Vec3& p : world_) {
        out.push_back(float(p.x));
        out.push_back(float(p.y));
        out.push_back(float(p.z));
    }
}

}  // namespace vehicle
}  // namespace wizengine
