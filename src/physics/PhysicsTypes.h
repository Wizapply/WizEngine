#pragma once

// PhysicsWorld の窓口でやり取りする値の型（Chrono の系を知らなくても使える）を集めたヘッダ。

#include <cstddef>
#include <vector>

#include <chrono/core/ChQuaternion.h>
#include <chrono/core/ChVector3.h>

// Where the time went inside the last physics step (seconds), straight from
// Chrono's own timers. Useful for deciding what to optimise: a solver-dominated
// step wants fewer iterations, a collision-dominated one wants a smaller
// contact envelope or a different collision system.
struct StepTimers {
    double step = 0.0;
    double solver = 0.0;
    double collision = 0.0;
    double setup = 0.0;
    double update = 0.0;
};

// Which Chrono system to build on. Multicore needs a Chrono built with the
// MULTICORE module (CMake: -DWIZ_USE_MULTICORE=ON); if that is not available
// the world silently falls back to Core and says so on stdout.
enum class PhysicsBackend {
    Core,       // ChSystemNSC - serial solver, supports sleeping
    Multicore,  // ChSystemMulticoreNSC - OpenMP solver + collision, no sleeping
};

// True when this build can actually use PhysicsBackend::Multicore.
bool multicoreAvailable();

// 接触の解き方（B の 22）。
//   NSC … 相補性（硬い接触。既定。1/60 s でも床を抜けない）
//   SMC … ペナルティ法（ヤング率で決まる柔らかい接触。粒状体・FEA 向け。
//         剛い材質ほど小さな dt が要る）
// Core は ChSystemNSC / ChSystemSMC、Multicore は ChSystemMulticoreNSC /
// ChSystemMulticoreSMC。切替は系の作り直し（recreate）。
enum class ContactMethod { NSC, SMC };

// このビルドが持っている B の機能（CMake / ヘッダの有無）。Scene が
// 「Core へ切り替えれば使えるか」を判断するのに使う。
struct PhysicsFeatures {
    bool fea = false;         // FEA のケーブル（chrono/fea）
    bool loads = false;       // ChLoad（ブッシュ・定常荷重）
    bool directSolvers = true;  // SparseLU / SparseQR（Eigen、常に有る）
    bool pardiso = false;     // Chrono::PardisoMKL
    bool mumps = false;       // Chrono::MUMPS
    bool modal = false;       // Chrono::Modal
};

// Rigid-body transform snapshot passed to the render side.
// Position in metres, rotation as a unit quaternion (w, x, y, z).
struct BodyTransform {
    double px, py, pz;
    double qw, qx, qy, qz;
};

// 拘束の種類。エディタの editor::JointKind と一対一だが、PhysicsWorld を
// エディタの型から独立させるため別の enum にしてある（変換は Scene 側）。
//   Fixed      … 完全固定（溶接）
//   Revolute   … ちょうつがい。軸まわりの回転だけ許す
//   Spherical  … ボールジョイント。回転は自由、位置だけ拘束
//   Prismatic  … 直動。軸方向のスライドだけ許す
//   Distance   … 2点間の距離を一定に保つ（棒・ロープの芯）
//   Universal  … 自在継手（ChLinkUniversal。axis = シャフトの向き、十字軸は
//                その直交 2 軸）
//   Cylindrical… 軸まわりの回転 + 軸方向のスライド（ChLinkLockCylindrical）
//   Planar     … 面内の移動と面内の回転（ChLinkLockPlanar。axis = 面の法線）
//   PointLine  … 点を線に載せる（ChLinkLockPointLine。axis = 線の向き）
//   PointPlane … 点を面に載せる（ChLinkLockPointPlane。axis = 面の法線）
//   Gear       … 歯車（ChLinkLockGear。2 本のシャフトの角速度比 ratio）
//   Screw      … ねじ（ChLinkLockScrew。1 回転で pitch [m] 進む）
//   Spring     … 2 点間のばね・ダンパ（ChLinkTSDA。拘束ではなく力）
//   Bushing    … ゴムブッシュ（ChLoadBodyBodyBushingMate。並進・回転の
//                剛性と減衰を持つ荷重。Core 専用 - Multicore は荷重
//                コンテナを積分に取り込まない）
enum class JointType {
    Fixed, Revolute, Spherical, Prismatic, Distance,
    Universal, Cylindrical, Planar, PointLine, PointPlane, Gear, Screw, Spring,
    Bushing
};

// ジョイントの駆動。Revolute → ChLinkMotorRotationSpeed / Angle / Torque、
// Prismatic → ChLinkMotorLinearSpeed / Position / Force に置き換わる。
// Chrono のモータは拘束型（速度・位置モータのトルクは無制限）なので、
// リミットとは併用できない。
enum class MotorType { None, Speed, Position, Force };

// addJoint への指定をひとまとめにしたもの。角度は**ラジアン**、長さは m、
// 力は N（文書の度からの変換は Scene 側）。
struct JointSpec {
    JointType type = JointType::Revolute;
    std::size_t bodyA = 0;
    std::size_t bodyB = 0;
    chrono::ChVector3d anchor{0.0, 0.0, 0.0};  // ワールド座標
    chrono::ChVector3d axis{0.0, 1.0, 0.0};    // ワールド座標（正規化不要）
    // Distance: 保つ距離。Spring: 自然長。0 = 現在の間隔。
    double distance = 0.0;
    // 可動範囲（Revolute / Cylindrical / Universal は rad、Prismatic は m）。
    bool limited = false;
    double limitLo = 0.0;
    double limitHi = 0.0;
    // 駆動（Revolute / Prismatic のみ）。target は rad/s・rad・N·m または
    // m/s・m・N。
    MotorType motor = MotorType::None;
    double motorTarget = 0.0;
    // ばね・ダンパ。Spring は 2 点間（N/m, N·s/m）。Revolute は回転ばね
    // （N·m/rad, N·m·s/rad）、Prismatic は両体の中心間の直線ばね。0 = 無し。
    double stiffness = 0.0;
    double damping = 0.0;
    // 反力の大きさ (N) がこれを超えたステップで外す。0 = 切れない。
    double breakForce = 0.0;
    // Gear: 変速比。Screw: 1 回転あたりの前進 (m)。
    double ratio = 1.0;
    double pitch = 0.01;
    // Gear の 2 本目のシャフト（ワールド座標）。hasAnchor2 = false なら B の
    // 中心、hasAxis2 = false なら axis と同じ向き。
    bool hasAnchor2 = false;
    bool hasAxis2 = false;
    chrono::ChVector3d anchor2{0.0, 0.0, 0.0};
    chrono::ChVector3d axis2{0.0, 0.0, 1.0};
    // Bushing: 回転の剛性 (N·m/rad) と減衰 (N·m·s/rad)。並進は stiffness /
    // damping を 3 軸に同じ値で使う。
    double rotStiffness = 0.0;
    double rotDamping = 0.0;
};

// ボディの追加時に渡す、接触の物性・衝突レイヤ・重力。負の値は「シーンの
// 設定（setSurfaceMaterial / setRollingFriction）を使う」。どれかを指定した
// ボディだけ専用の ChContactMaterialNSC を持つ（他は共有の 1 個）。
struct BodyOptions {
    float friction = -1.0f;
    float restitution = -1.0f;
    float rolling = -1.0f;   // 転がり摩擦（スピン摩擦も同じ値）
    float cohesion = 0.0f;   // 粘着力 (N)
    // 衝突レイヤ（Chrono の衝突ファミリ 0〜7）と、当たらないレイヤのビット
    // マスク（bit i = レイヤ i）。8〜15 はソフトボディの粒子が使う。
    int layer = 0;
    unsigned nocollide = 0;
    bool gravity = true;     // false = このボディだけ無重力
    // SMC（ペナルティ法）の材質。負 = シーン設定（setSmcDefaults）。NSC では無視。
    float young = -1.0f;
    float poisson = -1.0f;
    // 定常荷重（ワールド座標。ChLoadBodyForce / ChLoadBodyTorque = 荷重
    // コンテナ）。Core 専用。零 = 無し。
    chrono::ChVector3d force{0.0, 0.0, 0.0};
    chrono::ChVector3d torque{0.0, 0.0, 0.0};
};

// ---- ケーブル（FEA。B の 15）-----------------------------------------------
// ANCF ケーブル要素（ChBuilderCableANCF）を a → b の直線に segments 本
// 並べる。端は「固定点」「ボディに留める」「自由」のどれか。Core 専用
// （Multicore のデータマネージャは FEA 要素を持たない）。
struct CableSpec {
    enum class End { Free, Fixed, Body };
    chrono::ChVector3d a{0.0, 2.0, 0.0};
    chrono::ChVector3d b{1.0, 2.0, 0.0};
    End endA = End::Fixed;
    End endB = End::Free;
    std::size_t bodyA = 0;  // endA == Body のとき
    std::size_t bodyB = 0;
    int segments = 16;
    double diameter = 0.02;   // m
    double density = 1000.0;  // kg/m^3
    double young = 1.0e7;     // Pa
    double damping = 0.01;    // Rayleigh 減衰
    bool collide = true;      // 節点の球で接触する
    BodyOptions options;      // 接触の物性（collide のとき）
    // 留め先のボディの中心からこの距離 (m) 以内にある節点は接触面に入れない。
    // 端の節点はボディの中心に留まるので、そのまま接触させるとケーブルが
    // 自分の留め先と深くめり込んだ接触を作り、留め先が弾き飛ばされる
    // （実機で確認: おもりが暴れた）。呼び出し側がボディの大きさから決める。
    double clearA = 0.0;
    double clearB = 0.0;
};

// 直前の step() が作った接触 1 点ぶん（activeContacts）。
struct ContactInfo {
    std::size_t a = 0, b = 0;      // physId（a < b）
    double force = 0.0;            // 接触力の大きさ (N)
    double normalForce = 0.0;      // 法線方向の成分 (N)
    double nx = 0.0, ny = 0.0, nz = 0.0;  // 接触の法線（a から b へ向く）
    double depth = 0.0;            // めり込み (m、正 = 重なっている)
};

// 2 つの材質から接触の摩擦・反発・粘着をどう合成するか。
enum class CombineMode { Min, Average, Max };

// 衝突レイヤの数（ファミリ 0〜7。8〜15 はソフトボディの粒子用）。
constexpr int kCollisionLayerCount = 8;

// PhysicsWorld の入れ子として使う型と定数（PhysicsWorld に同じ名前の別名がある）。
namespace wizengine {
namespace physics {

// 本体の形に足す追加の当たり形状（プレハブの <part collide="true">）。
// ボディのローカル座標で、Chrono の複合形状として 1 ボディに入る。
// 質量・慣性には入らない（本体の geom のまま）。階段のように「固定の箱を
// 並べた物」を 1 オブジェクトにする用途。
struct ExtraShape {
    bool sphere = false;                          // false = 箱（cylinder も false）
    bool cylinder = false;                        // 円柱: x = 長さ（軸 X）、y = 直径
    chrono::ChVector3d size{0.5, 0.5, 0.5};       // 箱: 各辺の長さ、球: x = 直径
    chrono::ChVector3d pos{0.0, 0.0, 0.0};
    chrono::ChQuaternion<> rot{1.0, 0.0, 0.0, 0.0};
};

// physId やケーブル番号が無いことを表す値（add* / addCable / setTerrain が
// 作れなかったとき、テレインが無いときの terrainBodyId など）。
constexpr std::size_t kInvalidId = static_cast<std::size_t>(-1);

// PhysicsWorld::addSoftBody への指定。
struct SoftBodySpec {
    std::vector<chrono::ChVector3d> rest;  // 粒子の静止位置（ローカル）
    double radius = 0.05;                  // 粒子の当たり半径
    double particleMass = 0.01;            // 粒子 1 個の質量
    struct Spring {
        std::size_t a = 0, b = 0;
        double rest = 0.0;  // 自然長
        double k = 0.0;     // ばね定数 (N/m)
        double c = 0.0;     // 減衰係数 (N·s/m)
    };
    std::vector<Spring> springs;
    int iterations = 2;
    bool fixed = false;
    BodyOptions options;  // 物性・レイヤ・重力（全粒子に同じ）
};

// addJoint が作れなかったときに返す番号。
constexpr std::size_t kInvalidJoint = static_cast<std::size_t>(-1);

// PhysicsWorld::terrainContactShape の戻り値（意味はその宣言のコメント）。
enum class TerrainContactShape { None, Exact, BoundingBox };

}
}
