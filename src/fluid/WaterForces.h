#pragma once

#include <cstddef>
#include <vector>

#include "fluid/FluidMath.h"

// 水面（<worldbody> の <water>）が物に掛ける力の計算。純粋な関数だけで、Chrono も
// Filament も知らない（FluidComponent が物理の値を詰めて呼び、結果を
// PhysicsWorld::applyForceAtPoint で掛ける）。
//
// ---- モデル ------------------------------------------------------------------
// 水は「水平な水面（正弦波 1 本で揺らせる）の下を満たす、静止または一様に流れる
// 水」。物の形は箱か球で近似する（凸包のモデルは geom の半径の球）。
//   浮力   … アルキメデスの原理。押しのけた水の重さ ρ V_sub |g| を重力と逆向きに、
//             押しのけた部分の重心（浮心）に掛ける。箱は n × n × n の小箱に分けて
//             小箱ごとに「水面下にある割合」を出す（傾いた箱の復元モーメント、波の
//             上で傾く動きが自然に出る）。球は球冠の体積と重心の厳密な式。
//   抗力   … 浮心の速度と水の速度（流れ + 波の水粒子の速度）の差 u に対して
//             二乗の抗力 ½ ρ Cd A |u| u（A = u の向きに見た投影面積 × 水に浸かった
//             割合）と、線形の減衰 ρ V_sub c u（揺れが永遠に続かないための粘性の
//             代わり）。浮心は重心からずれているので、抗力は回す力も生む。
//   回転の抵抗 … ρ V_sub L² / 6 × (c_ang ω + Cd |ω| ω)（L は代表長さ）。
//   安全弁 … 抗力（と回転の抵抗）は「この 1 ステップで相対速度を反転させない」
//             大きさで頭打ち（軽い物・速い流れで陽に掛けても発散しない）。
// 付加質量・造波抵抗・水の側の反作用（物が水を押しのける動き）は入れない
// （水は動かない背景。水そのものを動かすのは流体粒子 <fluid> の役目）。
namespace wizengine {
namespace fluid {

// 水面の設定（文書の editor::WaterDesc を物理の語彙に直したもの）。
struct WaterParams {
    double centerX = 0.0, centerZ = 0.0;  // 水の範囲の中心 (m)
    double level = 0.0;                   // 静水面の高さ (m、ワールドの Y)
    double halfX = 20.0, halfZ = 20.0;    // 範囲の半分の広さ (m)。外の物には効かない
    double density = 1000.0;              // kg/m^3
    double drag = 1.0;                    // 二乗の抗力係数 Cd
    double linearDrag = 2.0;              // 線形の減衰 (1/s)
    double angularDrag = 1.0;             // 回転の減衰 (1/s)
    Vec3 flow{0.0, 0.0, 0.0};             // 流れ (m/s)
    double amplitude = 0.0;               // 波の振幅 (m、0 = 波なし)
    double period = 4.0;                  // 波の周期 (s)
    double heading = 0.0;                 // 波の進む向き（Y まわり、rad。0 = +X）
};

// 深水波の分散関係から波数 k = ω² / g（g は重力の大きさ。無重力でも波の形が
// 決まるように 9.81 を下限の代わりに使う）。
double waveNumber(const WaterParams& w, double gravity);
// 位置 (x, z)・時刻 t の水面の高さ（波を含む）。
double surfaceHeight(const WaterParams& w, double x, double z, double t, double gravity);
// 水面の上向きの法線（単位ベクトル）。
Vec3 surfaceNormal(const WaterParams& w, double x, double z, double t, double gravity);
// 位置 p の水の速度（流れ + 波の水粒子の速度。深さで指数的に弱まる）。
Vec3 waterVelocity(const WaterParams& w, const Vec3& p, double t, double gravity);
// (x, z) が水の範囲の中か。
bool insideWater(const WaterParams& w, double x, double z);

// 力を求める物の状態。
struct BodyState {
    enum class Shape { Box, Sphere };
    Shape shape = Shape::Box;
    Vec3 half{0.25, 0.25, 0.25};  // 箱の半寸法 (m)
    double radius = 0.25;         // 球の半径 (m)
    Vec3 position;                // 重心（= 形の中心）
    Quat rotation;
    Vec3 velocity;                // 重心の速度
    Vec3 angularVelocity;         // ワールド
    double mass = 1.0;            // kg（安全弁に使う）
};

// 結果。force は重心に掛ける力、torque は重心まわりのモーメント（浮心のずれ・
// 抗力の腕・回転の抵抗を全部含む）。submerged = 0 なら何も掛からない。
struct WaterLoad {
    Vec3 force;
    Vec3 torque;
    double submergedVolume = 0.0;  // m^3
    double volume = 0.0;           // 物の体積 m^3
    Vec3 buoyancyCenter;           // 浮心（submergedVolume > 0 のときだけ意味がある）
};

// 箱の分割数（1 軸）。4 = 64 個の小箱。
constexpr int kBoxSamplesPerAxis = 4;

// gravity は重力加速度のベクトル (m/s^2)、t はシミュレーション時刻、dt は
// このステップの長さ（安全弁に使う）。
WaterLoad computeWaterLoad(const WaterParams& w, const BodyState& body, const Vec3& gravity,
                           double t, double dt, int samplesPerAxis = kBoxSamplesPerAxis);

// ---- 水面の見た目 ------------------------------------------------------------
// 水の範囲を cells × cells の格子にした三角メッシュ（上から見て反時計回り）。
// 頂点は (cells + 1)^2 個で、並びは i（X）が内側・j（Z）が外側。
std::vector<unsigned> waterGridIndices(int cells);
// 時刻 t の頂点の位置と法線（3 個で 1 頂点）を pos / nrm に書く（大きさは呼び手が
// 合わせる必要はない = ここで resize する）。
void waterGridVertices(const WaterParams& w, int cells, double t, double gravity,
                       std::vector<float>& pos, std::vector<float>& nrm);

}  // namespace fluid
}  // namespace wizengine
