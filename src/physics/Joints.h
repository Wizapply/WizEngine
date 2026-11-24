#pragma once

// ジョイント: 拘束・モータ・ばね・ブッシュの生成、反力の計測、破断と、Multicore 用の
// 手計算（JointsManual.cpp）。物理スレッド専用。公開の意味は PhysicsWorld.h の同じ
// 役目の宣言（addJoint ほか）。

#include <cstddef>
#include <memory>
#include <vector>

#include "physics/PhysicsTypes.h"

namespace chrono {
class ChFunction;
class ChLinkBase;
class ChLoadBase;
class ChLoadContainer;
}

namespace wizengine {
namespace physics {

class BodyMotion;
class BodyRegistry;
class PhysicsSystem;

class Joints {
public:
    Joints(PhysicsSystem& system, BodyRegistry& bodies, BodyMotion& motion)
        : system_(system), bodies_(bodies), motion_(motion) {}
    Joints(const Joints&) = delete;
    Joints& operator=(const Joints&) = delete;

    // 戻り値はジョイント番号（作れなければ kInvalidJoint）。
    std::size_t add(const JointSpec& spec);
    // 全部外す（シミュレートの停止）。破断の暖機の数も 0 に戻す。
    void removeAll();
    std::size_t count() const { return joints_.size(); }
    bool setMotorTarget(std::size_t joint, double target);
    bool reaction(std::size_t joint, double& force, double& torque) const;
    bool broken(std::size_t joint) const;
    std::vector<std::size_t> takeBroken();
    // ジョイントごとの反力をログへ（診断が NaN を見つけたステップに step から）。
    void logReactions() const;
    // 破断の判定（step の末尾。このステップの反力を見る）。
    void checkBreaks();
    // Multicore 用の手計算のモータ・ばね・可動範囲（step の先頭。Core では空）。
    void applyManual(double dt);

    // ジョイント用の荷重コンテナを作って系へ足す（積分に取り込む系でだけ）。呼ぶ時期は
    // PhysicsWorld::Parts::createSystem が決める: 残差を足す順が変わらないよう、ボディ用の
    // コンテナ（BodyLoads::createContainer）より前に系へ足す。
    void createContainer();
    // 系の作り直し: 一覧・外れた番号・荷重コンテナ・暖機の数を捨てる（番号が全部
    // 無効になるため。古い系を壊す前に呼ぶ）。
    void clear();

private:
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

    PhysicsSystem& system_;
    BodyRegistry& bodies_;
    BodyMotion& motion_;
    std::vector<JointRec> joints_;
    std::vector<std::size_t> brokenPending_;  // 直前の step で外れた番号
    // 荷重コンテナ（ChLoad。Core のみ）。ボディの定常荷重（BodyLoads）とは分けてある
    // - ジョイントは停止で全部捨てるため。
    std::shared_ptr<chrono::ChLoadContainer> jointLoads_;
    // removeAll / clear からの checkBreaks の回数（破断の暖機）。
    int breakSteps_ = 0;
};

}  // namespace physics
}  // namespace wizengine
