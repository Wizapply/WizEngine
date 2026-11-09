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

namespace wizengine {
namespace terrain {
class HeightField;
}
}

PhysicsFeatures physicsFeatures();

// 物理エンジン: Chrono の系（重力・衝突・接触材質）を持つ。
// 自分ではシーンを持たない - ボディは Scene が add*（addBox ほか）で足す。
//
// ---- 部品の足し方（風など、新しい担当を足すとき）----------------------------
//   1. 担当ごとに新しいファイル（physics/<名前>.{h,cpp}）を作る。
//   2. PhysicsParts.h に include を 1 行、Parts にメンバを 1 行足す。
//   3. 名簿や系の作り直しの出来事を受けるなら PhysicsPart を継承し、Parts の
//      コンストラクタで bodies.events().add(&...) を 1 行。
//   4. 毎ステップ動くなら step() に 1 行（呼ぶ順番は step() の並びが正）。
//   5. Scene から呼ぶ口が要るなら、この PhysicsWorld.h に公開メソッドを 1 つ、
//      PhysicsWorld*.cpp に部品への中継を 1 つ。
//   6. CMakeLists.txt の物理のソース一覧に 1 行。
//   番号ごとの配列は onBodiesCleared で捨て、系に結び付く物は onSystemRecreated で
//   作り直す。recreate は触らない。
//   依存は窓口 → 部品の一方向。部品から窓口（PhysicsWorld）を呼ばない。
class PhysicsWorld {
public:
    // ---- 定数 ------------------------------------------------------------
    // physId・ケーブル番号が無いことを表す値（PhysicsTypes.h の kInvalidId）。
    static constexpr std::size_t kInvalidId = wizengine::physics::kInvalidId;
    // addJoint が作れなかったときに返す番号。
    static constexpr std::size_t kInvalidJoint = wizengine::physics::kInvalidJoint;

    explicit PhysicsWorld(PhysicsBackend backend = PhysicsBackend::Core,
                          ContactMethod contact = ContactMethod::NSC);
    ~PhysicsWorld();

    // "core" または "multicore" - 実際に作られた方。
    const char* backendName() const;
    PhysicsBackend backend() const;
    // "nsc" または "smc"。
    const char* contactName() const;
    ContactMethod contactMethod() const;

    // 系をまるごと作り直す（バックエンドの自動切替）。ボディ・ジョイント・ケーブルは
    // 全部消える（番号も無効になる）ので、呼び出し側（Scene）が作り直す。
    // 設定（重力・反復回数・許容差・スレッド数・スリープ・減衰・材質の
    // 既定）は覚えていて、新しい系にそのまま当て直す。Multicore を頼まれて
    // ビルドに無ければ Core になる（コンストラクタと同じ）。頼まれた系が作れなければ
    // Core + 同じ接触法へ倒して続ける（LOGE）。それも失敗したら投げる。倒した後も、
    // 次の作り直しでは頼まれた系をもう一度試す。
    void recreate(PhysicsBackend backend, ContactMethod contact);
    // SMC の材質の既定（<option young poisson>）。専用材質を持たないボディに
    // 使う。NSC では覚えるだけ。
    void setSmcDefaults(double young, double poisson);

    void step(double dt);

    // ボディを押す: 速度の変化 dv = force * (dt / 質量) を足し、眠っている箱も
    // 応じるようにボディを起こす（ソフトボディは合計質量で出した dv を全粒子に
    // 等しく足す。固定の物には掛けない）。力は N、dt は秒。Chrono の力アキュムレータ
    // ではなく力積を使うので、入っている版のアキュムレータの API に左右されない。
    void applyForce(std::size_t id, const chrono::ChVector3d& force, double dt);
    double bodyMass(std::size_t id) const;
    // 直線速度。サーボ式の力（マウスの掴み）用。
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

    // ワーカースレッドの数。Chrono 本体（FEA の内力とヤコビアン）・衝突判定・
    // Eigen（直接法ソルバ）の 3 つに同じ値で渡る。ステップを始める前に
    // 呼ぶこと（Chrono はこの値でスレッドプールの大きさを決める）。0 以下なら
    // 自動の選択のまま。
    void setNumThreads(int threads);

    // ソルバの反復回数: 品質とコストを決めるいちばんのつまみ。少ない = 速いが、
    // 静止した積み重ねの揺れが増える。
    void setSolverIterations(int iterations);

    // 接触のエンベロープ / マージン。エンベロープがあると、Chrono は面が実際に
    // 触れる前に接触を作る。既定（0.03 m）は 0.5 m の箱に比べて大きく、接触の
    // 数を何倍にもする。ボディを作る前に設定すること。
    void setCollisionTolerances(double envelope, double margin);

    // 既にある重なりをどれだけ強く押し出すか（m/s）と、ソルバをどれだけきつく
    // 収束させるか。押し戻しの速さを上げると目に見えるめり込みが早く消えるが、
    // 静止した積み重ねが弾けることがある。許容差をきつくすると反復が要る。
    void setContactSettings(double recoverySpeed, double tolerance);
    // 押し戻しの速さだけ（ブラウザからの実行中の調整に使う）。
    void setContactRecoverySpeed(double recoverySpeed);

    // 直前の step() のタイマー。
    StepTimers timers() const;

    // Chrono 自身のボディごとのスリープ: 直線 / 角速度が `seconds` のあいだ
    // しきい値を下回ったボディは、接触のアイランドが起こすまでソルバから外れる。
    // シーンの一部だけが動く山積みには安い。
    // 注意: 眠ったボディは接触でしか起きないので、支えが触れずに離れていくと
    // 宙に浮いたまま止まる。しきい値をきつく（Chrono の既定 0.1 m/s よりずっと
    // 下に）して、それを稀にする。
    void setSleepingEnabled(bool enabled, float seconds = 1.0f,
                            float minLinVel = 0.02f, float minAngVel = 0.02f);
    void wakeAll();
    std::size_t sleepingCount() const;

    using ExtraShape = wizengine::physics::ExtraShape;

    // 球のボディ - 箱と違って転がる。半径は m。
    std::size_t addSphere(double radius, double density,
                          const chrono::ChVector3d& pos,
                          const chrono::ChQuaternion<>& rot, bool fixed,
                          const std::vector<ExtraShape>& extras = {},
                          const BodyOptions& options = BodyOptions{});

    // シーン設定の転がり / スピン摩擦（共有の接触材質と、専用材質のボディの
    // 「シーン任せ」の欄が追従する。SMC の材質には無い）。どちらも無いと、
    // 平らな床で球が永遠に転がる。
    void setRollingFriction(float rolling, float spinning);

    // シーン設定の滑り摩擦と反発（共有の接触材質と、専用材質のボディの
    // 「シーン任せ」の欄が追従する）。
    // 滑りを転がりに変えるのは摩擦: 低い値では球が滑り、高い値では着地した
    // 途端に回り始める。
    void setSurfaceMaterial(float friction, float restitution);

    // 速度の減衰。毎ステップ v *= exp(-k dt) で掛ける: 空気抵抗の代わりで、
    // 球が永遠に転がるのも止める。0 で無効。ChBody 自身の減衰ではなくここで
    // 掛けるので、両バックエンドと Chrono の版をまたいで同じに振る舞う。
    void setDamping(double linearPerSecond, double angularPerSecond);

    // 箱のボディを足す。density は kg/m^3、fixed=true で動かない物にする。戻り値は id。
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

    // `points` の凸包で当たる動くボディ（m、拡縮済み。
    // MeshCollision::loadCollisionPoints を参照）。
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
    // 剛体姿勢（重心 + 最小二乗の回転）、placeBody は静止
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

    // ---- エディタ用 ------------------------------------------------------
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
    // sparseqr / pardiso / mumps）と陰的な積分器 hht / newmark も選べる。
    // 直接法は片側拘束を解けないので、NSC のまま頼まれたら bb に戻して
    // 警告する（true を返す = 「処理した」）。ビルドに無いモジュール
    // （pardiso / mumps）も同じく bb に戻す。
    bool setIntegrator(const std::string& name);
    bool setSolver(const std::string& name);
    // いま実際に使っているソルバ名（bb に戻したときはそれが分かる）。
    const std::string& solverName() const;
    // 2 材質の合成（min = Chrono の既定 / average / max）。
    void setMaterialCombine(CombineMode mode);

    // エディタで置き直すときの姿勢設定。「起こすための落下速度」は
    // 与えない（置いた場所にそのまま静止させたいので）。
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

    // ---- モーダル解析（Chrono::Modal。固有振動数）-------------------------
    // いまの系（剛体 + FEA + 拘束）の非減衰固有振動数を count 本求める
    // （Hz、昇順）。接触は入らない。Core 専用で、モジュールの無いビルド・
    // Multicore・失敗は false と理由（英語）。数十 ms〜数秒かかるので
    // シミュレート開始時に 1 回だけ呼ぶ。
    bool modalFrequencies(int count, std::vector<double>& hz, std::string& why);

    // ---- テレイン（高さ格子との自前の接触）--------------------------------
    // 三角メッシュではなく高さ格子（terrain::HeightField）を正として、物との
    // 接触を自前で見つけ（terrain::collectContacts）、Chrono の接触コンテナへ
    // 足す（ChSystem::CustomCollisionCallback。Bullet の接触の後、
    // DoStepDynamics ごとに 1 回）。力・摩擦・スリープ・積分は Chrono のまま。
    // 接触の相手は「ゴースト」の固定ボディ（空の衝突モデル、Bullet には
    // 登録しない）で、その physId が接触の報告（activeContacts）に出る。
    // 対象は動く（非固定の）ボディで、形の記録（proxy）は add* が作り、追加の通知で届ける:
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
    // 担当の入れ物（Chrono の系と名簿。中身は PhysicsParts.h）。
    struct Parts;
    // 壊れる順番（ジョイント・ケーブル・荷重など）は Parts の中で持つ。
    std::unique_ptr<Parts> p_;
};
