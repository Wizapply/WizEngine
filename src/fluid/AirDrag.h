#pragma once
// 空気抵抗（シーン全体の設定 <option density wind airdrag>）の純粋な計算。
// Chrono も Filament も知らない（WaterForces と同じ方針。FluidComponent が掛ける）。
//
// モデル（MuJoCo の <option density wind> と同じ置き場・同じ意味の値）:
//   並進: F = −½ ρ Cd A |u| u、u = 物の速度 − 風（相対速度）。
//         A は u の向きへの投影面積（箱 = |ux| sy sz + |uy| sx sz + |uz| sx sy を
//         ボディのローカルで、球 = π r²）。Cd は形ごとの代表値（箱 1.05・球 0.47）×
//         シーンの倍率 airdrag。力は重心に掛ける = 風でモーメントは生まない。
//   揚力（板）: 箱は 3 組の面をそれぞれ「平らな板」として、迎え角 α の板が受ける
//         流れに垂直な力 C_L = sin 2α（平板の実験値の形。小さい α で 2α、45° で 1）を
//         ½ ρ A_面 |u|² に掛けて足す: F = −ρ |u|² K A_i (û·n_i)(n_i − (û·n_i) û)、K = lift。
//         立方体は 3 組が打ち消して 0、薄い板・長い棒ほど効く（翼・紙・板が滑空する）。
//         力は重心に掛ける（圧力中心のずれ = 板がひらひら回る動きは入れない）。
//   マグヌス: 球は回転 ω で流れに垂直な力 F = ½ ρ π r² C_L |u|² (ω × û)/|ω × û|、
//         C_L = S / (2.022 S + 0.981)（Sawicki ほか、野球のボールの実測の当てはめ）、
//         S = r |ω⊥| / |u|（ω⊥ = ω の流れに垂直な成分）。逆回転で浮く・横回転で曲がる。
//   回転: 箱は面ごとに「軸まわりに回る板」の二乗抵抗 τ = ρ Cd b L⁴ ω|ω| / 64 を
//         ローカルの 3 軸それぞれで足す（L = 回転で掃く長さ、b = 軸方向の幅）。
//         球は形の抵抗が無いので回転の抵抗は掛けない。
// 安全弁: 陽に掛けるので、そのステップで相対速度（角速度）を反転させない大きさで
// 頭打ち（軽い物・強い風でも発散しない。水の抗力と同じ）。揚力とマグヌスも 1 ステップで
// 相対速度の大きさの半分を超える速度変化を与えない。
// 入れていないもの: 圧力中心のずれ（風見の効果・ひらひら）・物どうしの風よけ（後ろの物も
// 同じ風を受ける）・乱流の変動・地面近くの風の弱まり・失速の作り込み。

#include <algorithm>
#include <cmath>

#include "fluid/FluidMath.h"

namespace wizengine::fluid {

// 形ごとの抗力係数の代表値（倍率 1 のとき）。
constexpr double kAirCdBox = 1.05;
constexpr double kAirCdSphere = 0.47;

struct AirParams {
    double density = 0.0;  // kg/m³（0 = 空気抵抗なし）
    Vec3 wind;             // m/s（ワールド）
    double dragScale = 1.0;  // Cd の倍率
    double liftScale = 1.0;    // 板の揚力の倍率（0 = 無し）
    double magnusScale = 1.0;  // マグヌス効果の倍率（0 = 無し）
};

struct AirBody {
    enum class Shape { Box, Sphere };
    Shape shape = Shape::Box;
    Vec3 half;            // 箱の半寸法 (m)
    double radius = 0.0;  // 球の半径 (m)
    Quat rotation;
    Vec3 velocity;         // 重心の速度 (m/s)
    Vec3 angularVelocity;  // ワールド (rad/s)
    double mass = 1.0;
    // 球のマグヌス効果を掛けるか（本物の球だけ。形の分からないメッシュを球で
    // 近似したときは掛けない）。
    bool magnus = true;
};

struct AirLoad {
    Vec3 force;   // ワールド (N)、重心に掛ける
    Vec3 torque;  // ワールド (N·m)
};

inline AirLoad computeAirLoad(const AirParams& p, const AirBody& b, double dt) {
    AirLoad out;
    if (p.density <= 0.0 || p.dragScale <= 0.0 || dt <= 0.0 || b.mass <= 0.0) return out;
    const double rho = p.density;

    // ---- 並進 ----
    const Vec3 u = b.velocity - p.wind;
    const double speed = u.length();
    if (speed > 1e-6) {
        double area = 0.0;
        double cd = 0.0;
        if (b.shape == AirBody::Shape::Sphere) {
            area = kPi * b.radius * b.radius;
            cd = kAirCdSphere;
        } else {
            const Vec3 n = b.rotation.rotateBack(u / speed);  // ローカルの向き
            const double sx = 2.0 * b.half.x, sy = 2.0 * b.half.y, sz = 2.0 * b.half.z;
            area = std::fabs(n.x) * sy * sz + std::fabs(n.y) * sx * sz +
                   std::fabs(n.z) * sx * sy;
            cd = kAirCdBox;
        }
        double f = 0.5 * rho * cd * p.dragScale * area * speed * speed;
        // 安全弁: このステップで相対速度を 0 より先へ押さない。
        f = std::min(f, b.mass * speed / dt);
        out.force = u * (-f / speed);

        // 揚力とマグヌスは流れに垂直。1 ステップの速度変化を |u| の半分までに抑える
        // （向きを 1 ステップで大きく振らない = 軽い板・強い風でも振動しない）。
        const Vec3 uh = u / speed;
        const double q = 0.5 * rho * speed * speed;  // 動圧
        const double cap = 0.5 * b.mass * speed / dt;
        auto capped = [&](const Vec3& f) {
            const double m = f.length();
            return m > cap ? f * (cap / m) : f;
        };
        if (b.shape == AirBody::Shape::Box && p.liftScale > 0.0) {
            const double s[3] = {2.0 * b.half.x, 2.0 * b.half.y, 2.0 * b.half.z};
            const Vec3 axes[3] = {b.rotation.rotate(Vec3{1.0, 0.0, 0.0}),
                                  b.rotation.rotate(Vec3{0.0, 1.0, 0.0}),
                                  b.rotation.rotate(Vec3{0.0, 0.0, 1.0})};
            Vec3 lift;
            for (int i = 0; i < 3; ++i) {
                const double area = s[(i + 1) % 3] * s[(i + 2) % 3];  // 法線が軸 i の面
                const double c = uh.dot(axes[i]);
                // 2 q A c (n − c û): 大きさ q A sin 2α、向きは流れに垂直。
                lift = lift + (axes[i] - uh * c) * (-2.0 * q * p.liftScale * area * c);
            }
            out.force = out.force + capped(lift);
        }
        if (b.shape == AirBody::Shape::Sphere && b.magnus && p.magnusScale > 0.0) {
            const Vec3 side = b.angularVelocity.cross(uh);  // |ω⊥| と向き
            const double wPerp = side.length();
            if (wPerp > 1e-9) {
                const double spin = b.radius * wPerp / speed;
                const double cl = spin / (2.022 * spin + 0.981);
                const double area = kPi * b.radius * b.radius;
                out.force = out.force +
                            capped(side * (q * area * cl * p.magnusScale / wPerp));
            }
        }
    }

    // ---- 回転（箱だけ）----
    if (b.shape == AirBody::Shape::Box) {
        const Vec3 w = b.rotation.rotateBack(b.angularVelocity);
        const double s[3] = {2.0 * b.half.x, 2.0 * b.half.y, 2.0 * b.half.z};
        const double wl[3] = {w.x, w.y, w.z};
        double tl[3] = {0.0, 0.0, 0.0};
        const double k = rho * kAirCdBox * p.dragScale / 64.0;
        for (int a = 0; a < 3; ++a) {
            const int i = (a + 1) % 3, j = (a + 2) % 3;
            // 軸 a まわり: 面 ⟂ i（掃く長さ s[j]、幅 s[a]）と面 ⟂ j（長さ s[i]）。
            const double c = k * s[a] * (std::pow(s[i], 4) + std::pow(s[j], 4));
            double t = c * std::fabs(wl[a]) * wl[a];
            // 安全弁: 箱の慣性 m (si² + sj²) / 12 で角速度を反転させない。
            const double inertia = b.mass * (s[i] * s[i] + s[j] * s[j]) / 12.0;
            const double cap = inertia * std::fabs(wl[a]) / dt;
            t = std::clamp(t, -cap, cap);
            tl[a] = -t;
        }
        out.torque = b.rotation.rotate(Vec3{tl[0], tl[1], tl[2]});
    }
    return out;
}

}  // namespace wizengine::fluid
