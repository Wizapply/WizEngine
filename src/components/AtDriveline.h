#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "physics/ShaftTypes.h"
#include "transmission/AtControl.h"
#include "transmission/AtKinematics.h"
#include "transmission/AtSignals.h"
#include "transmission/AtTypes.h"

class PhysicsWorld;

// 遊星歯車式 AT を Chrono の 1D シャフト網として組み、毎ステップ動かす係。
// 動力学（慣性・遊星の拘束・クラッチの摩擦・トルコン・ばね・車両）はすべて Chrono が
// 解き、ここがするのは 3 つだけ:
//   build  … AtDesc から ShaftNetworkSpec を組んで PhysicsWorld へ足す
//   apply  … step の前に、TCU（transmission/AtControl）の判断で各クラッチの容量を決め、
//            軸に掛ける荷重（エンジンのトルク・油圧ポンプ・軸受と開いた要素の引きずり・
//            かみ合いと終減速の損失・車両の抵抗）を与える
//   read   … step の後の角速度と各部のトルクを AtReadout（trans:<名前>.<量>）に詰める
//
// シャフトの並び: 0 = エンジン軸（クランク側 + コンバータのカバー + 羽根車）、1 = ロックアップ /
// ダンパの 1 次側、2.. = 歯車列の軸（desc.shafts の並び。タービンは入力軸に、出力軸の
// 慣性は出力軸に足す）、続いて（load = vehicle のとき）終減速の後の軸と車輪 + 車両。
// （input = body のときは最後に、剛体へ直結した短い軸を足す）
// クラッチの並び: 締結要素（desc.elements）、ロックアップ、（vehicle）ブレーキ、パーキング、
// （body）剛体の軸 ⇄ エンジン軸の切り離し（inputs().couple。フレックスプレートを外すのに相当）。
// ばね: ダンパ、（vehicle）ドライブシャフト。歯車: （vehicle）終減速。
// 物理スレッド専用（ControlComponent が持つ）。
class AtDriveline {
public:
    explicit AtDriveline(const wizengine::transmission::AtDesc& desc);
    AtDriveline(const AtDriveline&) = delete;
    AtDriveline& operator=(const AtDriveline&) = delete;

    const wizengine::transmission::AtDesc& desc() const { return desc_; }
    const wizengine::transmission::AtKinematics& kinematics() const { return kin_; }
    bool ok() const { return kin_.ok(); }

    // engineBody: input = body のときの剛体の番号（PhysicsWorld の id。それ以外は
    // kInvalidId）、bodyAxis: その剛体のローカルの回転軸、engineW0: エンジン軸の初めの
    // 角速度 (rad/s。torque はアイドル回転、body は剛体の今の回転）。
    bool build(PhysicsWorld& physics, std::size_t engineBody, const double bodyAxis[3],
               double engineW0, std::string& error);
    bool built() const { return net_ != static_cast<std::size_t>(-1); }

    wizengine::transmission::AtInputs& inputs() { return in_; }
    void apply(PhysicsWorld& physics, double dt);
    void read(PhysicsWorld& physics, double dt);
    const wizengine::transmission::AtReadout& readout() const { return out_; }

private:
    double converterTorqueRatio(double sr) const;

    wizengine::transmission::AtDesc desc_;
    wizengine::transmission::AtKinematics kin_;
    wizengine::transmission::AtControl control_;
    wizengine::transmission::AtInputs in_;
    wizengine::transmission::AtReadout out_;
    wizengine::physics::ShaftNetworkState st_;
    std::size_t net_ = static_cast<std::size_t>(-1);

    // 並び（build で決める）
    int engine_ = 0, primary_ = 1, train0_ = 2, input_ = -1, output_ = -1, fd_ = -1, wheel_ = -1;
    int lockupClutch_ = -1, brakeClutch_ = -1, parkClutch_ = -1, coupleClutch_ = -1;
    int link_ = -1;  // input = body: 剛体へ直結した軸
    int damperSpring_ = -1, shaftSpring_ = -1, finalGear_ = -1;
    int engineMotor_ = -1, dynoMotor_ = -1, bodyLink_ = -1;
    std::size_t shaftCount_ = 0;

    // 前のステップの値（損失の見積もり・表示）
    double engApplied_ = 0.0;  // エンジン軸へ与えたトルク（input = torque）
    double pumpLoss_ = 0.0;    // 油圧ポンプの損失
    double speedPrev_ = 0.0;
    double effFilt_ = 0.0;
};
