#pragma once

#include <chrono/physics/ChContactMaterial.h>
#include <chrono/physics/ChSystem.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "physics/PhysicsTypes.h"

namespace chrono {
class ChLinkBase;
class ChFunction;
class ChLoadBase;
class ChLoadContainer;
namespace fea {
class ChMesh;
class ChNodeFEAxyzD;
}
}

namespace wizengine {
namespace terrain {
class HeightField;
struct BodyProxy;
}
}

PhysicsFeatures physicsFeatures();

// Physics engine: owns the Chrono system (gravity, collision, contact material).
// It holds no scene of its own - bodies are added by the Scene via addBox().
class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsBackend backend = PhysicsBackend::Core,
                          ContactMethod contact = ContactMethod::NSC);
    ~PhysicsWorld();

    // "core" or "multicore" - what actually got created.
    const char* backendName() const;
    PhysicsBackend backend() const;
    // "nsc" or "smc".
    const char* contactName() const;
    ContactMethod contactMethod() const;

    // 系をまるごと作り直す（B の自動切替）。ボディ・ジョイント・ケーブルは
    // 全部消える（番号も無効になる）ので、呼び出し側（Scene）が作り直す。
    // 設定（重力・反復回数・許容差・スレッド数・スリープ・減衰・材質の
    // 既定）は覚えていて、新しい系にそのまま当て直す。Multicore を頼まれて
    // ビルドに無ければ Core になる（コンストラクタと同じ）。
    void recreate(PhysicsBackend backend, ContactMethod contact);
    // SMC の材質の既定（<option young poisson>）。専用材質を持たないボディに
    // 使う。NSC では覚えるだけ。
    void setSmcDefaults(double young, double poisson);

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

    using ExtraShape = wizengine::physics::ExtraShape;

    // Sphere body - rolls, unlike a box. Radius in metres.
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

    // Add a box body. density in kg/m^3; fixed=true makes it static. Returns id.
    std::size_t addBox(double sx, double sy, double sz, double density,
                       const chrono::ChVector3d& pos,
                       const chrono::ChQuaternion<>& rot, bool fixed,
                       const std::vector<ExtraShape>& extras = {},
                       const BodyOptions& options = BodyOptions{});

    // 自分の形を持たないボディ（geom の無い <body> = プレハブの部品だけ）。
    // 当たり判定は extras（collide 部品）だけ、質量は指定、慣性は extras の
    // 外接箱から見積もる。extras が空なら当たらないフレーム。
    // terrainContact = false はテレインの接触の形の記録（proxy）を作らない。固定の
    // まま使う物用（固定を解くと安全網の AABB で当たる）。
    std::size_t addFrame(double mass, const chrono::ChVector3d& pos,
                         const chrono::ChQuaternion<>& rot, bool fixed,
                         const std::vector<ExtraShape>& extras,
                         const BodyOptions& options = BodyOptions{},
                         bool terrainContact = true);

    // 三角メッシュそのものを当たり形状にするボディ（glTF の凹形状: 器・
    // トンネル・地形）。Bullet の三角メッシュ形状で、**固定の物に向く**
    // （動く凹形状は GImpact で重く、Multicore は三角形ごとに払う）。
    // 質量と慣性は外接箱の均質な箱として見積もる（凹メッシュの体積は
    // 当てにならない）。この Chrono に三角メッシュ形状の口が無ければ
    // kInvalidId を返し、呼び出し側が凸包へ倒す。
    static constexpr std::size_t kInvalidId = wizengine::physics::kInvalidId;
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
    using SoftBodySpec = wizengine::physics::SoftBodySpec;
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
    // どちらも false を返して何もしない。Core では直接法（sparselu /
    // sparseqr / pardiso / mumps）と hht / newmark も選べる（B の 16）。
    // 直接法は片側拘束を解けないので、NSC のまま頼まれたら bb に戻して
    // 警告する（true を返す = 「処理した」）。ビルドに無いモジュール
    // （pardiso / mumps）も同じく bb に戻す。
    bool setIntegrator(const std::string& name);
    bool setSolver(const std::string& name);
    // いま実際に使っているソルバ名（bb に戻したときはそれが分かる）。
    const std::string& solverName() const;
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
    static constexpr std::size_t kInvalidJoint = wizengine::physics::kInvalidJoint;
    std::size_t addJoint(JointType type, std::size_t bodyA, std::size_t bodyB,
                         const chrono::ChVector3d& anchor,
                         const chrono::ChVector3d& axis, double distance);
    // リミット・モータ・ばね・破断・ギア比まで含めた指定（PhysicsTypes.h の JointSpec）。
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

    // ---- ケーブル（FEA）----------------------------------------------------
    // 戻り値はケーブル番号（作れなければ kInvalidId: FEA の無いビルド・
    // Multicore・端のボディが無い）。ジョイントと同じくシミュレート開始で
    // 作り、停止で removeAllCables。節点の位置は cableNodePositions で
    // 3 個ずつ float に詰める（描画スレッドへのスナップショット用）。
    std::size_t addCable(const CableSpec& spec);
    void removeAllCables();
    std::size_t cableCount() const;
    void cableNodePositions(std::size_t cable, std::vector<float>& out) const;
    // ケーブルの張力の目安（端の拘束の反力 N。無ければ 0）。
    double cableTension(std::size_t cable) const;

    // ---- モーダル解析（Chrono::Modal。B の 20）-----------------------------
    // いまの系（剛体 + FEA + 拘束）の非減衰固有振動数を count 本求める
    // （Hz、昇順）。接触は入らない。Core 専用で、モジュールの無いビルド・
    // Multicore・失敗は false と理由（英語）。数十 ms〜数秒かかるので
    // シミュレート開始時に 1 回だけ呼ぶ。
    bool modalFrequencies(int count, std::vector<double>& hz, std::string& why);

    // ---- テレイン（高さ格子との自前の接触。段階 1.5）------------------------
    // 三角メッシュではなく高さ格子（terrain::HeightField）を正として、物との
    // 接触を自前で見つけ（terrain::collectContacts）、Chrono の接触コンテナへ
    // 足す（ChSystem::CustomCollisionCallback。Bullet の接触の後、
    // DoStepDynamics ごとに 1 回）。力・摩擦・スリープ・積分は Chrono のまま。
    // 接触の相手は「ゴースト」の固定ボディ（空の衝突モデル、Bullet には
    // 登録しない）で、その physId が接触の報告（activeContacts）に出る。
    // 対象は動く（非固定の）ボディで、形の記録（proxy）を add* が書く:
    //   addBox / addSphere … 本体の箱 / 球 + 追加の当たり形状（箱・球・円柱）
    //   addConvexHull      … Chrono が当たり形状にした凸包の頂点（体積重心へ
    //                        寄せた後 = ボディローカル。512 点まで間引く）+ 追加形状
    //                        （凸包が作れなかったときは追加形状だけ）
    //   addFrame           … 追加の当たり形状だけ（空なら当たり判定が無い = 対象外）
    //   addSoftBody        … 粒子ごとに半径 radius の球
    //   addTriangleMesh    … 動く物だけ、頂点（512 点まで間引く）+ 追加形状
    // 記録の無い動く物（上のどれでもない経路・走行中に固定を外した三角
    // メッシュなど）は**安全網**として衝突モデルの AABB の 8 角で当てる（粗い。
    // 初めての 1 体だけ LOGW）。Core 専用（Multicore では何もしない）。
    //
    // field を張る / 差し替える。ゴーストが無ければ作り、あれば使い回す。
    // terrainOptions はテレイン側の物性（負 = シーン設定）。テレインの材質は
    // 2 つ: 丸い形（球）との接触 = terrainOptions のまま、それ以外（箱など
    // 多点で触れる形）= 転がり・スピン摩擦 0（BB ソルバで斜面の箱が沈むため。
    // ベンチ B1 / B2）。**格子の高さを書き換えたら呼び直す**（早期棄却の値を
    // 作り直すため）。戻り値 = ゴーストの physId（Multicore・null の field では
    // kInvalidId）。眠っている物は起こす（地面が変わったので）。
    // recreate で系ごと消えるので、作り直したら呼び直すこと（Scene の役目）。
    std::size_t setTerrain(std::shared_ptr<const wizengine::terrain::HeightField> field,
                           const BodyOptions& terrainOptions);
    // テレインのフレーム（格子のローカル → ワールド: ワールド = origin +
    // rot * ローカル。格子は原点基準 = terrain/TerrainFrame.h）。接触の収集は
    // ボディをこのローカルへ直してから格子に問い合わせ、結果をワールドへ戻す。
    // setTerrain とは別に覚えていて（張り直し・clearTerrain でも残る）、
    // 変わったら眠っている物を起こす。recreate のあとも Scene が当て直す。
    void setTerrainFrame(const chrono::ChVector3d& origin, const chrono::ChQuaternion<>& rot);
    // テレインの接触をやめる（ゴーストは次の setTerrain のために残す。
    // 番号は terrainBodyId から消える）。眠っている物は起こす。
    void clearTerrain();
    bool hasTerrain() const;
    // ゴーストの physId（テレインが無ければ kInvalidId）。
    std::size_t terrainBodyId() const;
    // id が形の記録（安全網でない proxy）を持つか。固定かどうかは見ない。
    bool hasTerrainProxy(std::size_t id) const;
    // id がテレインにどう当たるか（テレインの有無は見ない = 張る前に聞ける）:
    //   None        … 当たらない（退場・固定・当たり判定なし・Chrono 側に形が
    //                   1 つも無い・ゴースト）
    //   Exact       … 形の記録で当たる
    //   BoundingBox … 記録が無いので安全網（衝突モデルの AABB の 8 角）で当たる
    // 固定は**今の**状態（IsFixed）を見る。素通り / 粗い当たりの判定はこれ 1 か所。
    using TerrainContactShape = wizengine::physics::TerrainContactShape;
    TerrainContactShape terrainContactShape(std::size_t id) const;

private:
    // Chrono の系を（作り直しも含めて）組む。コンストラクタと recreate が呼ぶ。
    void createSystem();
    // 本当に系から外す（Core。Multicore は退避だけ）。disableBody から。
    void detachBody(std::size_t id);
    // 破断の判定（step の末尾）。
    void checkJointBreaks();
    // ジョイントごとの反力をログへ（診断が NaN を見つけたステップに step から）。
    void logJointReactions() const;
    // Multicore 用の手計算のモータ・ばね（step の先頭。下の JointRec::Manual）。
    void applyManualJoints(double dt);

    // ---- ソフトボディの内部 ----------------------------------------------
    struct SoftBody {
        std::size_t root = 0;                   // 代表 = 最初の粒子の physId（粒子の列は名簿のグループ）
        std::vector<chrono::ChVector3d> rest;   // 静止位置（ローカル）
        chrono::ChVector3d restCentroid;        // rest の重心
        std::vector<SoftBodySpec::Spring> springs;
        int iterations = 2;
        bool active = true;
    };
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

    // ---- テレインの内部（中身は PhysicsWorldInternal.h。Eigen と terrain の型を見せない）--
    struct TerrainProxyRec;   // ボディ 1 個の形の記録 + 点ごとの反力キャッシュ
    struct TerrainState;      // 格子・ゴースト・材質・判定の設定
    struct TerrainCallback;   // ChSystem::CustomCollisionCallback
    // id のボディの形を記録する（追加の通知から。Parts::TerrainProxyBridge）。
    void setTerrainProxy(std::size_t id, const wizengine::terrain::BodyProxy& proxy);
    // id の「前のステップに接触した」印と反力キャッシュを 0 に戻す（置き直し・
    // wakeAll。前回の走行の反力で次の最初のステップを始めないため）。
    void forgetTerrainContacts(std::size_t id);
    // テレインと接触しうるか（terrainContactShape と collectTerrainContacts の
    // 共通の条件。眠りは見ない）。
    bool terrainEligible(std::size_t id) const;
    // 安全網の記録（無ければ作る。LOGW は初めての 1 体だけ）。
    TerrainProxyRec* terrainFallbackRec(std::size_t id);
    // テレインの 2 つの材質へシーン設定と terrainOptions を書き直す。
    void refreshTerrainMaterials();
    // コールバックの本体（DoStepDynamics の衝突判定の末尾）。
    void collectTerrainContacts(chrono::ChSystem& sys);

    // 担当の入れ物（Chrono の系と名簿。中身は PhysicsParts.h）。
    struct Parts;
    // 他のメンバより先に宣言する = 系は他のメンバ（ジョイント・テレイン・荷重など）が
    // 全部壊れた後に壊れる。
    std::unique_ptr<Parts> p_;
    // 荷重コンテナ（ChLoad。Core のみ）。ジョイント（ブッシュ）とボディの
    // 定常荷重（BodyLoads）で分けてある - ジョイントは停止で全部捨てるため。
    std::shared_ptr<chrono::ChLoadContainer> jointLoads_;
    std::vector<SoftBody> softBodies_;  // root の昇順（softOfRoot が二分探索で引く）
    // 名簿の番号と並ぶ: テレインに当たる形の記録（無ければ null = テレインの
    // 接触の対象外）。shared_ptr なのは型の中身を PhysicsWorldInternal.h に閉じる
    // ため（ここは前方宣言だけ）。反力キャッシュは記録のときに 1 回だけ確保し、
    // 以後は再確保しない（アドレスを Chrono の接触へ渡すため）。
    std::vector<std::shared_ptr<TerrainProxyRec>> terrainProxies_;
    // テレインのフレーム（setTerrainFrame）。単位なら変換を省く。
    chrono::ChVector3d terrainFrameOrigin_{0, 0, 0};
    chrono::ChQuaternion<> terrainFrameRot_{1, 0, 0, 0};
    bool terrainFrameIdentity_ = true;
    // テレイン（setTerrain が作る。recreate で捨てる）とコールバック
    // （createSystem が Core のときだけ作って系に登録する）。
    std::shared_ptr<TerrainState> terrain_;
    std::shared_ptr<TerrainCallback> terrainCallback_;
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
        // Bushing（荷重。jointLoads_ に入っている）。
        std::shared_ptr<chrono::ChLoadBase> load;
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
    // ケーブル（FEA メッシュ + 端の拘束）。removeAllCables でまとめて外す。
    struct CableRec {
        std::shared_ptr<chrono::fea::ChMesh> mesh;
        std::vector<std::shared_ptr<chrono::fea::ChNodeFEAxyzD>> nodes;
        std::vector<std::shared_ptr<chrono::ChLinkBase>> links;  // 端の拘束
        bool valid = false;
    };
    std::vector<CableRec> cables_;
};
