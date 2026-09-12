#include "vehicle/SoftTire.h"

#include <algorithm>
#include <cmath>

#include "vehicle/TireFormula.h"

namespace wizengine {
namespace vehicle {
namespace {

constexpr double kRimRadiusRatio = 0.6;   // リムのフランジ（タイヤと接する輪）の半径（タイヤ半径比）。
                                          // 物理の潰れ上限 25% でもトレッドの粒子（半径 ≤ 13.5%）が
                                          // フランジより外に残る値（0.75 - 0.135 > 0.6）
constexpr double kRimWidthRatio = 0.8;    // フランジの x = ±(w/2) × これ
constexpr double kHubRadiusRatio = 0.35;  // ハブのリング（皿の底）の半径（タイヤ半径比）
constexpr double kHubWidthRatio = 0.55;   // ハブのリングとキャップの x = ±(w/2) × これ（皿状にへこむ）
constexpr double kMeshMass = 8.0;         // メッシュ全体の質量 (kg)。見た目の応答の速さを決める
constexpr double kMaxDeviation = 0.4;     // 静止位置からのずれの上限（半径比。安全弁）
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
    // 空気圧のための閉じた面と静止体積。
    indices_ = topology(tire_).indices;
    restVolume_ = volumeOf(rest_);
    areaVectorsOf(rest_, areaVec_);
    totalArea_ = 0.0;
    for (std::size_t k = 0; k < rest_.size(); ++k) {
        if (!kinematic_[k]) totalArea_ += areaVec_[k].length();
    }

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
    // ばねが要るのはトレッドと、トレッドから中へ向かう空気圧だけ（ハブと
    // キャップは運動学的な蓋）。
    //
    // 空気圧のばねは**ハブ（半径 35%）へ**張る。リムのフランジ（60%）へ張ると、
    // トレッドがフランジより内側へ押し込まれたとき、鏡像の位置でもばねの
    // 長さが同じになって、へこんだまま安定してしまう（双安定）。ハブなら
    // 鏡像の位置がタイヤの中に無いので、離せば必ず元の半径へ戻る。
    // フランジへは転がりを伝える斜めのばねだけを張る。
    const int hubL = 0, rimL = 1, rimR = rows_ + 2, hubR = rows_ + 3;
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
            // 空気圧: 両側のハブへ（同じ角度）。
            add(id(r, i), id(hubL, i), kBase);
            add(id(r, i), id(hubR, i), kBase);
            // 転がり（周方向のずれ）を伝える斜めのばね: フランジへ。
            add(id(r, i), id(rimL, i + 1), 0.5 * kBase);
            add(id(r, i), id(rimL, i - 1), 0.5 * kBase);
            add(id(r, i), id(rimR, i + 1), 0.5 * kBase);
            add(id(r, i), id(rimR, i - 1), 0.5 * kBase);
        }
    }
}

void SoftTire::reset() {
    placed_ = false;
    if (pressureFormula_) pressureFormula_->reset();
}

void SoftTire::setPressureFormula(std::unique_ptr<FormulaInstance> formula) {
    pressureFormula_ = std::move(formula);
    formulaFailures_ = 0;
}

// 閉じた面が囲む体積（発散定理: 三角形ごとの a·(b×c)/6 の和。巻きが
// 外向きなので正）。
double SoftTire::volumeOf(const std::vector<Vec3>& p) const {
    double v = 0.0;
    for (std::size_t t = 0; t + 2 < indices_.size(); t += 3) {
        const Vec3& a = p[indices_[t]];
        const Vec3& b = p[indices_[t + 1]];
        const Vec3& c = p[indices_[t + 2]];
        v += a.dot(b.cross(c));
    }
    return v / 6.0;
}

// 頂点の面積ベクトル = 隣接三角形の（面積 × 外向き法線）/ 3 の和。圧力 p を
// 掛けた p × A がその頂点の受ける力になる。
void SoftTire::areaVectorsOf(const std::vector<Vec3>& p, std::vector<Vec3>& out) const {
    out.assign(p.size(), Vec3{});
    for (std::size_t t = 0; t + 2 < indices_.size(); t += 3) {
        const std::uint32_t i0 = indices_[t], i1 = indices_[t + 1], i2 = indices_[t + 2];
        const Vec3 n = (p[i1] - p[i0]).cross(p[i2] - p[i0]) * (1.0 / 6.0);  // 面積 / 2 / 3
        out[i0] += n;
        out[i1] += n;
        out[i2] += n;
    }
}

// 空気圧: 体積比から圧力の増分 p を出す（式 or 組み込みの等温変化）。
//
// 力そのもの（p × 面積ベクトル）を陽解法で足すと、圧力の「全体が同時に
// 膨らむ / 縮む」モードの結合剛性 K = p_abs A_i A_tot / V が大きく（高圧・
// 粗い刻みで dt² K / m が 10 を超える）、ばねの陰解法と分けて解くと発散するか、
// 安定化のために割った分だけ釣り合いの位置がずれる。そこで圧力は**径方向の
// 陰解法ばね**として表す: 剛性 K_i = p_abs |A_i| A_tot / V（一様モードの
// 線形化と同じ値）、自然長は静止半径 + p |A_i| / K_i = 静止半径 + p V /
// (p_abs A_tot)。釣り合いは「圧力の力 = ばねの力」の真の解と一致し、ばねの
// 反復（solve の中）で 1 本ずつ implicit Euler なので無条件に安定。局所的な
// へこみにも K_i が効く（本物の空気圧は一様にしか押さないので、その分だけ
// へこみに硬い近似）。
void SoftTire::computePressure(double load, double deflection, double dt) {
    const double p0 = tire_.pressure;
    pressureK_ = 0.0;
    pressureOffset_ = 0.0;
    if (p0 <= 0.0 && !pressureFormula_) {
        ratio_ = 1.0;
        pressure_ = 0.0;
        return;
    }
    if (restVolume_ <= 1e-12 || totalArea_ <= 1e-12) return;
    const double volume = volumeOf(pos_);
    const double prevRatio = ratio_;
    ratio_ = clampd(volume / restVolume_, 1e-3, 100.0);
    const double rate = (ratio_ - prevRatio) / dt;

    double p = 0.0;
    bool ok = false;
    if (pressureFormula_) {
        const double in[6] = {ratio_, rate, p0, load, deflection, tire_.radius};
        double out[1] = {0.0};
        ok = pressureFormula_->eval(in, out, dt) && std::isfinite(out[0]);
        if (ok) p = out[0];
        else ++formulaFailures_;
    }
    if (!ok) p = builtinTirePressure(ratio_, p0);
    // 安全のため上限を掛ける（式がどんな数字を出しても輪が壊れない）。
    const double pRef = std::max(p0, 1.0e4);
    p = clampd(p, -10.0 * pRef, 10.0 * pRef);
    pressure_ = p;

    const double pAbs = std::max(pRef + p, 0.1 * pRef);
    areaVectorsOf(pos_, areaVec_);
    // K_i の |A_i| は粒子ごとだが、係数 κ = p_abs A_tot / V は共通。
    pressureK_ = pAbs * totalArea_ / std::max(volume, 1e-9);
    pressureOffset_ = p * std::max(volume, 1e-9) / (pAbs * totalArea_);
}

// 粒子は**車輪ローカル**（X = 車軸、リムと一緒に回る座標系）で解く。ワールドで
// 解くと、車輪が 1 ステップに何度も回る速さ（40 rad/s × 1/120 s = 19°）では
// トレッドが直線で進むぶんだけ外へ膨らむ（遠心力もどきの離散化誤差）。
// ローカルならリムは動かず、地面の平面だけがタイヤの周りを回るので、
// 接地面の潰れがそのまま「転がりながら戻る変形」になる。
void SoftTire::step(const Vec3& center, const Quat& rotIn, bool grounded,
                    const Vec3& groundPoint, const Vec3& groundNormalIn, double load,
                    double deflection, double dt) {
    const Quat rot = rotIn.normalized();
    const std::size_t count = rest_.size();
    if (!placed_ || dt <= 0.0) {
        for (std::size_t k = 0; k < count; ++k) {
            pos_[k] = rest_[k];
            vel_[k] = Vec3{};
        }
        ratio_ = 1.0;
        pressure_ = 0.0;
        placed_ = true;
    }
    if (dt > 0.0) {
        // ---- 空気圧（体積 → 圧力。ばねの反復の中で径方向のばねとして掛ける）
        computePressure(load, deflection, dt);

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
            if (pressureK_ > 0.0) {
                // 空気圧 = 径方向の陰解法ばね（computePressure 参照）。方向は
                // 車輪ローカルの径方向 (0, y, z)、相手は車軸（動かない）。
                const double zeta = clampd(tire_.damping, 0.0, 5.0);
                for (std::size_t k = 0; k < count; ++k) {
                    if (kinematic_[k]) continue;
                    const double rho = std::sqrt(pos_[k].y * pos_[k].y + pos_[k].z * pos_[k].z);
                    if (rho < 1e-9) continue;
                    const double ny = pos_[k].y / rho, nz = pos_[k].z / rho;
                    const double area = areaVec_[k].length();
                    const double kp = pressureK_ * area;
                    if (kp <= 0.0) continue;
                    const double restRho = std::sqrt(rest_[k].y * rest_[k].y + rest_[k].z * rest_[k].z);
                    const double x = rho - (restRho + pressureOffset_);
                    const double vr = vel_[k].y * ny + vel_[k].z * nz;
                    const double c = 2.0 * zeta * std::sqrt(kp * mass_);
                    const double denom = 1.0 + dt * (c + dt * kp) / mass_;
                    const double vNew = (vr - dt * kp * x / mass_) / denom;
                    const double dv = vNew - vr;
                    vel_[k].y += ny * dv;
                    vel_[k].z += nz * dv;
                }
            }
        }

        // ---- 積分・地面・安全弁（すべて車輪ローカル）------------------------
        const Vec3 nL = rot.rotateBack(groundNormalIn.normalized());
        const Vec3 gpL = rot.rotateBack(groundPoint - center);
        const double damp = std::exp(-kAirDamping * dt);
        const double maxDev = kMaxDeviation * tire_.radius;
        // リムは硬い境界: ゴムは金属のリムを通り抜けない。トレッドの粒子の
        // 中心はフランジの半径より内側へ入らない（入ると空気圧ばねが逆向きに
        // 釣り合って戻らなくなる）。地面の押し戻しはこの後に掛ける = 両方が
        // 食い違うときは接地面が勝つ（物理の潰れ上限 25% なら食い違わない）。
        const double rFloor = tire_.radius * kRimRadiusRatio;
        for (std::size_t k = 0; k < count; ++k) {
            if (kinematic_[k]) continue;
            vel_[k] *= damp;
            pos_[k] += vel_[k] * dt;
            // リムの境界（車輪ローカルの径方向 = y, z）。
            {
                const double rho = std::sqrt(pos_[k].y * pos_[k].y + pos_[k].z * pos_[k].z);
                if (rho < rFloor && rho > 1e-9) {
                    const double scale = rFloor / rho;
                    pos_[k].y *= scale;
                    pos_[k].z *= scale;
                    const double vr = (vel_[k].y * pos_[k].y + vel_[k].z * pos_[k].z) / rFloor;
                    if (vr < 0.0) {
                        vel_[k].y -= vr * pos_[k].y / rFloor;
                        vel_[k].z -= vr * pos_[k].z / rFloor;
                    }
                }
            }
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
