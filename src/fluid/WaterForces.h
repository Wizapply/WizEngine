#pragma once

#include <cstddef>
#include <vector>

#include "fluid/FluidMath.h"

// 水面（<worldbody> の <water>）が物に掛ける力の計算。純粋な関数だけで、Chrono も
// Filament も知らない（FluidComponent が物理の値を詰めて呼び、結果を
// PhysicsWorld::applyForceAtPoint で掛ける）。
//
// ---- モデル ------------------------------------------------------------------
// 水は「波で揺れる水面の下を満たす、静止または一様に流れる水」。物の形は箱か球で
// 近似する（凸包のモデルは geom の半径の球）。
//   波     … 何本かの **Gerstner 波**（水面の点が円を描いて回る波。波頭が尖り、谷が
//             平たくなる）の和。成分は 2 種類で、両方あれば足す:
//               うねり … 振幅 amplitude・周期 period・向き heading の 1 本（手で決める）
//               風浪   … 風速 wind から海の波のスペクトル（Pierson-Moskowitz）で決める
//                        waveCount 本（向きは heading ± spread にばらす。seed で並びが
//                        決まる = 同じ文書なら毎回同じ海）。有義波高は 0.21 U² / g。
//             choppy が水平の寄り（尖り方）で、0 = 正弦波の和、1 = 本来の Gerstner 波、
//             それより大きいと誇張（海のスペクトルは傾きが小さいので、絵として尖らせたい
//             ときに）。波面が折り返さないように、全成分の傾き k A の和 × choppy は 0.9 で
//             頭打ち。
//             描画（水面の格子）と浮力は同じ関数を使う = 物は見えている波どおりに揺れる。
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

// 波の成分 1 本（prepareWaves が作る）。位相 θ = k (d · x0) − ω t + phase、
// 静止位置 x0 の水面の点は
//   高さ   η = A cos θ
//   水平   x = x0 − q A d sin θ（q = 尖り方。1 = 本来の Gerstner 波）
// に動く（波は d の向きへ進む）。
struct WaveComponent {
    double dx = 1.0, dz = 0.0;  // 進む向き（単位）
    double k = 0.0;             // 波数 (rad/m)
    double omega = 0.0;         // 角振動数 (rad/s)。深水波 ω² = g k
    double amplitude = 0.0;     // 振幅 (m)
    double phase = 0.0;         // 初期位相 (rad)
    double q = 0.0;             // 水平の寄り（1 = 本来の Gerstner 波）
};

// 水面の設定（文書の editor::WaterDesc を物理の語彙に直したもの）。波の成分
// waves は prepareWaves で作る（作るまでは静水）。
struct WaterParams {
    double centerX = 0.0, centerZ = 0.0;  // 水の範囲の中心 (m)
    double level = 0.0;                   // 静水面の高さ (m、ワールドの Y)
    double halfX = 20.0, halfZ = 20.0;    // 範囲の半分の広さ (m)。外の物には効かない
    double density = 1000.0;              // kg/m^3
    double drag = 1.0;                    // 二乗の抗力係数 Cd
    double linearDrag = 2.0;              // 線形の減衰 (1/s)
    double angularDrag = 1.0;             // 回転の減衰 (1/s)
    Vec3 flow{0.0, 0.0, 0.0};             // 流れ (m/s)
    // うねり（手で決める 1 本）
    double amplitude = 0.0;               // 振幅 (m、0 = 無し)
    double period = 4.0;                  // 周期 (s)
    double heading = 0.0;                 // 波と風の進む向き（Y まわり、rad。0 = +X）
    // 風浪（スペクトルから決める waveCount 本）
    double wind = 0.0;                    // 風速 (m/s、0 = 無し)
    double spread = 0.785;                // 向きのばらつき（heading ± spread、rad）
    int waveCount = 12;                   // 成分の数
    unsigned seed = 1;                    // 成分の並び（向き・位相）を決める種
    double choppy = 1.0;                  // 尖り方（0 = 正弦波、1 = 本来の Gerstner 波、> 1 = 誇張）

    // ---- prepareWaves が埋める ----
    std::vector<WaveComponent> waves;
    double crest = 0.0;          // 静水面からの最大の高さ（全成分の振幅の和）
    double maxCompression = 0.0; // 波頭の縮みの最大（Σ q k A。白波の目安に使う）
};

// 波の成分を作る（gravity は重力の大きさ。無重力でも形が決まるように 0 なら
// 9.81 を使う）。設定や重力を変えたら呼び直す。
void prepareWaves(WaterParams& w, double gravity);
// 有義波高（成分の分散から 4 √m0。テストと説明のため）。
double significantWaveHeight(const WaterParams& w);

// 静止位置 (x0, z0) の水面の点（Gerstner の写像そのもの。描画の格子が使う）。
struct SurfacePoint {
    Vec3 position;            // 動いた先の位置
    Vec3 normal{0.0, 1.0, 0.0};  // 上向きの単位法線
    double compression = 0.0; // 面の縮み 1 − J（正 = 波頭で詰まっている。白波の目安）
    double height = 0.0;      // 静水面からの高さ
};
SurfacePoint surfacePoint(const WaterParams& w, double x0, double z0, double t);
// 位置 (x, z)・時刻 t の水面の高さ（波を含む）。Gerstner の水平の寄りを数回の反復で
// 逆にたどって、その真上の点の高さを返す。
double surfaceHeight(const WaterParams& w, double x, double z, double t);
// 水面の上向きの法線（単位ベクトル）。
Vec3 surfaceNormal(const WaterParams& w, double x, double z, double t);
// 位置 p の水の速度（流れ + 波の水粒子の速度。深さで指数的に弱まる）。
Vec3 waterVelocity(const WaterParams& w, const Vec3& p, double t);
// (x, z) が水の範囲の中か。
bool insideWater(const WaterParams& w, double x, double z);
// 波頭の白さ（0..1）。s は surfacePoint の値、amount は文書の foam（0 = 出さない）。
// 面の縮みの絶対値で決める（穏やかなうねりは白くならない）。風浪があるときは風速
// 3 m/s 以下で 0、8 m/s で最大。
double foamAmount(const WaterParams& w, const SurfacePoint& s, double amount);

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
// 時刻 t の頂点の位置と法線（3 個で 1 頂点）を pos / nrm に、白波の量（1 個で 1 頂点、
// foam が 0 なら全部 0）を foamOut に書く（大きさはここで resize する）。格子の点は
// Gerstner の写像で動かすが、縁の点は水平に動かさない（プールの壁との隙間を作らない。
// 高さは surfaceHeight で合わせる）。
void waterGridVertices(const WaterParams& w, int cells, double t, double foam,
                       std::vector<float>& pos, std::vector<float>& nrm,
                       std::vector<float>& foamOut);

}  // namespace fluid
}  // namespace wizengine
