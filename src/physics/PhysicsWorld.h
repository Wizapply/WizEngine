#pragma once

#include <chrono/physics/ChContactMaterialNSC.h>
#include <chrono/physics/ChSystem.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chrono {
class ChLinkBase;
class ChFunction;
}

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
enum class JointType {
    Fixed, Revolute, Spherical, Prismatic, Distance,
    Universal, Cylindrical, Planar, PointLine, PointPlane, Gear, Screw, Spring
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

// Physics engine: owns the Chrono system (gravity, collision, contact material).
// It holds no scene of its own - bodies are added by the Scene via addBox().
class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsBackend backend = PhysicsBackend::Core);

    // "core" or "multicore" - what actually got created.
    const char* backendName() const;

    void step(double dt);

    // Solver iterations: the main quality/cost dial. Fewer = faster but more
    // jitter in resting stacks.
    // Push a body: applies force * dt as a change in linear velocity, and
    // wakes the body so a sleeping box responds. Force is in newtons, dt in
    // seconds; using an impulse rather than Chrono's force accumulators keeps
    // this independent of which accumulator API the installed version has.
    void applyForce(std::size_t id, const chrono::ChVector3d& force, double dt);
    double bodyMass(std::size_t id) const;
    // Linear velocity, for servo-style forces (mouse joint).
    chrono::ChVector3d bodyVelocity(std::size_t id) const;

    // ---- 車両用（点に掛かる力）------------------------------------------
    // ワールド座標の点 point に力 force を dt のあいだ掛ける = 力積 force*dt を
    // 重心の速度と角速度に分けて足す（applyForce と同じく速度で与えるので、
    // Chrono の力アキュムレータの版差に触らない）。角速度の変化は慣性
    // テンソルをワールドへ回して求める。車輪ごとのサス力・タイヤ力が
    // これを毎ステップ呼ぶ（VehicleComponent）。
    void applyForceAtPoint(std::size_t id, const chrono::ChVector3d& force,
                           const chrono::ChVector3d& point, double dt);
    // 角速度（ワールド座標、rad/s）。
    chrono::ChVector3d bodyAngularVelocity(std::size_t id) const;
    // ボディ上のワールド座標の点の速度（v + ω × r）。
    chrono::ChVector3d bodyPointVelocity(std::size_t id,
                                        const chrono::ChVector3d& point) const;

    // Number of worker threads the solver and collision detection may use.
    // Must be called before stepping starts; Chrono sizes its thread pool from
    // this. 0 or less leaves the automatic choice in place.
    void setNumThreads(int threads);

    void setSolverIterations(int iterations);

    // Contact envelope/margin. The envelope makes Chrono create contacts before
    // surfaces actually touch; the default (0.03 m) is large next to 0.5 m
    // boxes and multiplies the contact count. Must be set before bodies are
    // created.
    void setCollisionTolerances(double envelope, double margin);

    // How aggressively existing overlap is pushed out (m/s), and how tightly
    // the solver converges. Raising the recovery speed removes visible
    // interpenetration faster but can make resting stacks pop; tightening the
    // tolerance costs iterations.
    void setContactSettings(double recoverySpeed, double tolerance);
    // Recovery speed alone (used for live tuning from the browser).
    void setContactRecoverySpeed(double recoverySpeed);

    // Timers for the most recent step().
    StepTimers timers() const;

    // Chrono's own per-body sleeping: a body whose linear/angular speed stays
    // below the limits for `seconds` is dropped from the solver until a contact
    // island wakes it. Cheap for piles where only part of the scene moves.
    // Caveat: a sleeping body is only woken through contact, so if its support
    // moves away without touching it, it stays put in mid-air. Keep the limits
    // tight (well under Chrono's 0.1 m/s default) to make that rare.
    void setSleepingEnabled(bool enabled, float seconds = 1.0f,
                            float minLinVel = 0.02f, float minAngVel = 0.02f);
    void wakeAll();
    std::size_t sleepingCount() const;

    // Add a box body. density in kg/m^3; fixed=true makes it static. Returns id.
    // Sphere body - rolls, unlike a box. Radius in metres.
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

    std::size_t addSphere(double radius, double density,
                          const chrono::ChVector3d& pos,
                          const chrono::ChQuaternion<>& rot, bool fixed,
                          const std::vector<ExtraShape>& extras = {},
                          const BodyOptions& options = BodyOptions{});

    // Rolling/spinning friction on the shared contact material. Without any,
    // spheres roll forever on a flat floor.
    void setRollingFriction(float rolling, float spinning);

    // Sliding friction and bounciness of the shared contact material.
    // Friction is what converts sliding into rolling: a low value makes
    // spheres skid, a high one makes them spin up as soon as they touch down.
    void setSurfaceMaterial(float friction, float restitution);

    // Velocity damping, applied every step as v *= exp(-k dt): a stand-in for
    // air resistance that also stops spheres from rolling forever. 0 disables
    // it. Applied here rather than through ChBody's own damping so it behaves
    // the same on both backends and across Chrono versions.
    void setDamping(double linearPerSecond, double angularPerSecond);

    std::size_t addBox(double sx, double sy, double sz, double density,
                       const chrono::ChVector3d& pos,
                       const chrono::ChQuaternion<>& rot, bool fixed,
                       const std::vector<ExtraShape>& extras = {},
                       const BodyOptions& options = BodyOptions{});

    // 自分の形を持たないボディ（geom の無い <body> = プレハブの部品だけ）。
    // 当たり判定は extras（collide 部品）だけ、質量は指定、慣性は extras の
    // 外接箱から見積もる。extras が空なら当たらないフレーム。
    std::size_t addFrame(double mass, const chrono::ChVector3d& pos,
                         const chrono::ChQuaternion<>& rot, bool fixed,
                         const std::vector<ExtraShape>& extras,
                         const BodyOptions& options = BodyOptions{});

    // 三角メッシュそのものを当たり形状にするボディ（glTF の凹形状: 器・
    // トンネル・地形）。Bullet の三角メッシュ形状で、**固定の物に向く**
    // （動く凹形状は GImpact で重く、Multicore は三角形ごとに払う）。
    // 質量と慣性は外接箱の均質な箱として見積もる（凹メッシュの体積は
    // 当てにならない）。この Chrono に三角メッシュ形状の口が無ければ
    // kInvalidId を返し、呼び出し側が凸包へ倒す。
    static constexpr std::size_t kInvalidId = static_cast<std::size_t>(-1);
    std::size_t addTriangleMesh(const std::vector<chrono::ChVector3d>& vertices,
                                const std::vector<std::array<int, 3>>& triangles,
                                double mass, const chrono::ChVector3d& pos,
                                const chrono::ChQuaternion<>& rot, bool fixed,
                                const std::vector<ExtraShape>& extras = {},
                                const BodyOptions& options = BodyOptions{});
    // id のボディを owner の「子」にする: 接触ペア（activeContactPairs）では
    // owner の番号で報告される。固定の持ち主の collide 部品を別ボディにする
    // ときに使う（Scene::createBody。複合形状ではなく普通の箱 / 球）。
    void setAlias(std::size_t id, std::size_t owner);

    // Dynamic body colliding as the convex hull of `points` (metres, already
    // scaled; see MeshCollision::loadCollisionPoints).
    //
    // 質量は `mass` そのもの（箱・球のような密度ではない）。凸包の体積は
    // モデルの大きさ次第で、文書の size とは無関係に決まるので、密度から
    // 出すと桁違いの質量になる（大きな凸包 = 何でも弾き飛ばす重さ、小さな
    // 凸包 = 触れただけで飛んでいく軽さ）。慣性は Chrono が凸包から出した
    // 形のまま、質量の比で伸縮する。
    //
    // Chrono は凸包を**体積重心へ寄せる**（ボディの原点 = 重心）。モデルの
    // 原点が重心から離れていると、そのままでは見た目と当たり判定がずれる
    // ので、寄せた量（ボディ座標の重心、m）を hullCenter で返す - 呼び出し
    // 側（Scene）が見た目を同じだけ逆にずらして重ねる。
    std::size_t addConvexHull(const std::vector<chrono::ChVector3d>& points,
                              double mass, const chrono::ChVector3d& pos,
                              const chrono::ChQuaternion<>& rot,
                              const std::vector<ExtraShape>& extras = {},
                              chrono::ChVector3d* hullCenter = nullptr,
                              const BodyOptions& options = BodyOptions{});

    // ---- ソフトボディ（質点ばね）------------------------------------------
    // 小さな球の剛体（粒子）の集合を、ばねで結んで 1 つの柔らかい物にする。
    // 戻り値は**代表番号**（最初の粒子の physId）で、この番号への既存の
    // 操作はそのまま粒子全体に広がる: bodyTransform は粒子群に当てはめた
    // 剛体姿勢（重心 + 最小二乗の回転）、placeBody / setBodyPose は静止
    // 形状のまま置き直し、setBodyFixed / disableBody は全粒子、applyForce は
    // 全体の質量で速度変化を等しく配る、bodyMass は合計、bodyVelocity は
    // 平均、activeContactPairs は粒子の接触を代表番号で報告する。Scene 側は
    // 剛体と同じ physId 1 個で扱えばよい。
    //
    // ばねは step() の中で DoStepDynamics の前に解く（vehicle と同じく速度に
    // 直接足す）。1 本ずつ implicit Euler で解いた速度変化をガウス・ザイデル
    // で回すので、どんなに硬くしても発散しない（硬さが dt に対して大きい
    // ときは「自然長へ射影する拘束」に近づく）。同じソフトボディの粒子
    // どうしは衝突しない（衝突ファミリで除外。粒子は眠らない）。
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
    std::size_t addSoftBody(const SoftBodySpec& spec, const chrono::ChVector3d& pos,
                            const chrono::ChQuaternion<>& rot);
    // id がソフトボディの代表番号か。
    bool isSoftBody(std::size_t id) const;
    // 粒子のワールド位置を 3 個ずつ out へ詰める（代表番号で。無ければ空）。
    // 描画スレッドへ渡すスナップショットのために float で返す。
    void softParticlePositions(std::size_t id, std::vector<float>& out) const;
    std::size_t softParticleCount(std::size_t id) const;

    std::size_t bodyCount() const;
    BodyTransform bodyTransform(std::size_t id) const;

    // Move a body and clear its velocity/acceleration (for re-dropping).
    void setBodyPose(std::size_t id, const chrono::ChVector3d& pos,
                     const chrono::ChQuaternion<>& rot);

    // ---- エディタ用 ------------------------------------------------------
    // 重力（-Y 方向の大きさ）。エディタのシミュレート設定から呼ぶ。
    void setGravityY(double gravityY);
    // 重力の 3 成分（斜面・無重力・横向きの重力）。
    void setGravity(double gx, double gy, double gz);
    // このボディだけ重力を切る / 戻す（ソフトボディなら全粒子）。
    // Multicore は自前の積分で重力を足すため効かない版がある。
    void setBodyGravity(std::size_t id, bool enabled);
    // 速度と角速度（ワールド座標、rad/s）を直接書く。初速とイベントの
    // setVelocity 用。眠っていれば起こす。固定ボディには何もしない。
    void setBodyVelocity(std::size_t id, const chrono::ChVector3d& linear,
                         const chrono::ChVector3d& angular);
    // 接触の物性を実行中に書き換える。専用の材質を持っているボディは
    // その場で変わり true。共有材質のボディ（= 追加時に指定が無かった）は
    // 材質を差し替えられない（形状に焼き込まれている）ので false を返し、
    // 呼び出し側が作り直す。
    bool setBodySurface(std::size_t id, const BodyOptions& options);

    // 積分器 / ソルバの切替（名前は editor::integratorNameValid /
    // solverNameValid の語彙）。Multicore は自前のステッパと APGD なので
    // どちらも false を返して何もしない。
    bool setIntegrator(const std::string& name);
    bool setSolver(const std::string& name);
    // 2 材質の合成（min = Chrono の既定 / average / max）。
    void setMaterialCombine(CombineMode mode);

    // エディタで置き直すときの姿勢設定。setBodyPose と違い「起こすための
    // 落下速度」を与えない（置いた場所にそのまま静止させたいので）。
    void placeBody(std::size_t id, const chrono::ChVector3d& pos,
                   const chrono::ChQuaternion<>& rot);

    // 土台にする / 動くようにする。
    void setBodyFixed(std::size_t id, bool fixed);
    // いま固定か（設計値ではなく Chrono の実体。イベントの SetFixed で
    // 走行中に固定された物も true）。固定の物に力を掛けても動かないので、
    // 掴みはこれを見て引っぱり線ごと諦める。
    bool bodyFixed(std::size_t id) const;

    // 「削除」。Chrono からボディを取り除くのではなく、当たり判定を切って
    // 固定し、地面のはるか下へ退避させる。Multicore バックエンドはボディの
    // 削除でデータマネージャの配列が崩れることがあるため、ここでは触らない
    // ほうが安全（番号も動かないので、他のオブジェクトの ID がずれない）。
    void disableBody(std::size_t id);
    bool bodyActive(std::size_t id) const;

    // ---- 接触の報告（イベントグラフの衝突トリガー用）----------------------
    // 直前の step() が作った接触のボディペア（physId の組、first < second、
    // 重複なし）。Chrono の接触コンテナを走査するので、必ず物理スレッドから
    // 呼ぶこと。NSC では静止して積まれた物も毎ステップ「接触」なので、
    // 「ぶつかった瞬間」が欲しい側（Scene::runEventGraph）が前ステップとの
    // 差分を取る。Multicore の接触コンテナが ReportAllContacts を実装しない
    // 版では空が返る（衝突トリガーが効かないだけで、他は壊れない）。
    std::vector<std::pair<std::size_t, std::size_t>> activeContactPairs() const;
    // 同じ接触を力・法線・めり込み付きで（接触点ごと。同じペアが複数回
    // 出る）。イベントの「強くぶつかったとき」「上から乗ったとき」の判定用。
    std::vector<ContactInfo> activeContacts() const;

    // ---- ジョイント -------------------------------------------------------
    // anchor / axis はワールド座標。axis は Revolute の回転軸、Prismatic の
    // スライド方向で、それ以外では無視される。distance は Distance 専用で、
    // 0 なら現在の 2 点間の距離をそのまま保つ。
    // 戻り値はジョイント番号。作れなかったときは kInvalidJoint。
    static constexpr std::size_t kInvalidJoint = static_cast<std::size_t>(-1);
    std::size_t addJoint(JointType type, std::size_t bodyA, std::size_t bodyB,
                         const chrono::ChVector3d& anchor,
                         const chrono::ChVector3d& axis, double distance);
    // リミット・モータ・ばね・破断・ギア比まで含めた指定（上の JointSpec）。
    std::size_t addJoint(const JointSpec& spec);
    // シミュレート開始のたびに作り直すので、まとめて捨てる口だけ用意する。
    void removeAllJoints();
    std::size_t jointCount() const;
    // モータの目標値を実行中に書き換える（単位は JointSpec::motorTarget と
    // 同じ）。モータ無し・破断済みなら false。
    bool setJointMotorTarget(std::size_t joint, double target);
    // 直前の step() の拘束反力（力 N と トルク N·m の大きさ。ボディ B 側）。
    // モータの出力トルクもここに出る。無い・破断済みなら false。
    bool jointReaction(std::size_t joint, double& force, double& torque) const;
    // breakForce を超えて外れたか。
    bool jointBroken(std::size_t joint) const;
    // 直前の step() で外れたジョイント番号（取り出すと空になる）。
    std::vector<std::size_t> takeBrokenJoints();

private:
    // Either a ChSystemNSC (serial core) or a ChSystemMulticoreNSC, chosen at
    // build time by WIZ_USE_MULTICORE. Everything above this class is unaware
    // of which one is in use.
    // 追加したボディを逆引きマップにも登録する（activeContactPairs 用）。
    void registerBody(const std::shared_ptr<chrono::ChBody>& body);
    // 衝突系が既に初期化済みなら、いま足したボディの衝突モデルを登録する
    // （Chrono 9 は自動でやらない。詳細は .cpp）。
    void bindCollision(const std::shared_ptr<chrono::ChBody>& body);
    // 追加の当たり形状を本体に足す（AddBody の前に呼ぶ）。
    void attachExtraShapes(chrono::ChBody& body, const std::vector<ExtraShape>& extras,
                           const std::shared_ptr<chrono::ChContactMaterialNSC>& mat);
    // ボディの物性を決める材質（指定が無ければ共有の mat_）。
    std::shared_ptr<chrono::ChContactMaterialNSC> materialFor(const BodyOptions& o);
    // シーン設定と合成した値を材質へ書く（負 = シーン設定）。
    void applySurface(chrono::ChContactMaterialNSC& m, const BodyOptions& o) const;
    // AddBody の**前**に呼ぶ: 衝突レイヤ（ファミリ）と重力オフ。ファミリは
    // 衝突系に登録する前に決めておく - 登録後に変えると Bullet はモデルを
    // 一度 Remove して入れ直し、Multicore の衝突系は Remove が未実装で
    // 例外を投げる（ソフトボディの粒子も同じ順序）。
    void prepareBody(chrono::ChBody& body, const BodyOptions& options);
    // AddBody の後: 逆引き登録・材質とオプションの記録。
    void finishBody(const std::shared_ptr<chrono::ChBody>& body,
                    const std::shared_ptr<chrono::ChContactMaterialNSC>& mat,
                    const BodyOptions& options);
    // 破断の判定（step の末尾）。
    void checkJointBreaks();
    // Multicore 用の手計算のモータ・ばね（step の先頭。下の JointRec::Manual）。
    void applyManualJoints(double dt);
    // ボディへ角力積（ワールド座標）を与える（固定・粒子には何もしない）。
    void applyAngularImpulse(std::size_t id, const chrono::ChVector3d& impulse);

    // ---- ソフトボディの内部 ----------------------------------------------
    struct SoftBody {
        std::size_t root = 0;                   // 代表 = 最初の粒子の physId
        std::vector<std::size_t> particles;     // 粒子の physId（root が先頭）
        std::vector<chrono::ChVector3d> rest;   // 静止位置（ローカル）
        chrono::ChVector3d restCentroid;        // rest の重心
        std::vector<SoftBodySpec::Spring> springs;
        int iterations = 2;
        bool active = true;
    };
    static constexpr std::size_t kNoSoft = static_cast<std::size_t>(-1);
    // ばねを解いて粒子の速度を書き換える（step の先頭）。
    void solveSoftSprings(SoftBody& soft, double dt);
    // 粒子群に当てはめた剛体姿勢（重心 + 最小二乗の回転）。
    BodyTransform softTransform(const SoftBody& soft) const;
    // 静止形状のまま pos / rot へ置き直す（全粒子を止める）。
    void placeSoftBody(SoftBody& soft, const chrono::ChVector3d& pos,
                       const chrono::ChQuaternion<>& rot);
    // その番号が代表するソフトボディ（代表番号でなければ nullptr）。
    const SoftBody* softOfRoot(std::size_t id) const;
    SoftBody* softOfRoot(std::size_t id);
    // 粒子ならその属するソフトボディの代表番号、剛体ならそのまま。
    std::size_t representative(std::size_t id) const;

    std::shared_ptr<chrono::ChSystem> sys_;
    std::shared_ptr<chrono::ChContactMaterialNSC> mat_;
    // シーン設定の値（専用材質を持つボディの「シーン任せ」の欄を解くため）。
    float friction_ = 0.6f;
    float restitution_ = 0.0f;
    float rolling_ = 0.0f;
    float spinning_ = 0.0f;
    std::vector<std::shared_ptr<chrono::ChBody>> bodies_;
    // bodies_ と並ぶ: 専用の材質（共有なら nullptr）とオプション。
    std::vector<std::shared_ptr<chrono::ChContactMaterialNSC>> mats_;
    std::vector<BodyOptions> options_;
    // 接触コールバックが返す ChBody* から physId への逆引き。ボディは
    // 削除されない（disableBody は退場させるだけ）ので、追加時に足すだけ。
    std::unordered_map<const chrono::ChBody*, std::size_t> bodyIndex_;
    // disableBody() で退場させたボディは false。番号は詰めない。
    std::vector<bool> active_;
    // bodies_ と並ぶ: この番号の粒子が属するソフトボディ（softBodies_ の
    // 位置）。剛体は kNoSoft。
    std::vector<std::size_t> softOf_;
    // 子ボディ → 持ち主（setAlias）。kNoSoft = 無し。接触ペアの報告にだけ効く。
    std::vector<std::size_t> alias_;
    std::vector<SoftBody> softBodies_;
    // シミュレート中だけ存在する拘束。エディタへ戻るときに全部外す。
    struct JointRec {
        std::shared_ptr<chrono::ChLinkBase> link;   // 主拘束（ばね種類ならばね。手計算なら null）
        std::shared_ptr<chrono::ChLinkBase> extra;  // 拘束に添えたばね（無ければ null）
        std::shared_ptr<chrono::ChFunction> motorFn;  // ChFunctionConst（モータ無しは null）
        // Multicore では速度モータを「角度 / 位置モータ + 傾きが速度のランプ
        // 関数」で作る（Chrono::Multicore の速度モータは専用の変数一覧を持ち、
        // RemoveLink で外れないため）。目標を変えるときは今の角度から新しい
        // 傾きで引き直す。
        std::shared_ptr<chrono::ChLinkBase> rampMotor;  // 角度 / 位置モータ
        bool rampLinear = false;
        JointType type = JointType::Revolute;
        double breakForce = 0.0;
        bool broken = false;
        // Multicore は ChLinkTSDA / ChLinkRSDA / トルク・力モータの力を積分に
        // 取り込まない（IntLoadResidual_F を使わない）ので、そちらでは毎
        // ステップ手で力積を掛ける。Core では使わない（Chrono のリンクを使う）。
        struct Manual {
            enum class Kind { None, Torque, Force, Spring, Rsda };
            Kind kind = Kind::None;
            std::size_t a = 0, b = 0;
            chrono::ChVector3d axisLocalA{0, 0, 1};    // 軸（A ローカル）
            chrono::ChVector3d p1Local{0, 0, 0};       // ばねの取り付け点（A ローカル）
            chrono::ChVector3d p2Local{0, 0, 0};       // 同 B ローカル
            chrono::ChQuaternion<> relRot0{1, 0, 0, 0};  // RSDA: 開始時の conj(qA) qB
            double k = 0.0, c = 0.0, rest = 0.0, target = 0.0;
            double lastForce = 0.0;   // 反力表示用（N）
            double lastTorque = 0.0;  // 同（N·m）
            // 可動範囲（Multicore は片側拘束を解けず NaN になるので手計算）。
            // 接触と同じ速度レベルの力積: 範囲を超えて更に進む相対速度だけを
            // 打ち消し、めり込みは β/dt で緩く押し戻す。角度は relRot0 基準、
            // 距離は limitRef（開始時の軸方向距離）基準。
            bool limited = false;
            bool limitLinear = false;
            double limitLo = 0.0, limitHi = 0.0;
            double limitRef = 0.0;
        } manual;
    };
    std::vector<JointRec> joints_;
    std::vector<std::size_t> brokenPending_;  // 直前の step で外れた番号
    int diagSteps_ = 0;  // 診断ログ用のステップ数（removeAllJoints で 0 に）
    bool diagNanReported_ = false;
    PhysicsBackend backend_ = PhysicsBackend::Core;
    double linearDamping_ = 0.0;
    double angularDamping_ = 0.0;
    bool sleepingEnabled_ = false;
    float sleepSeconds_ = 1.0f;
    float sleepMinLinVel_ = 0.02f;
    float sleepMinAngVel_ = 0.02f;
};
