#pragma once

#include <cstddef>
#include <vector>

#include "transmission/AtKinematics.h"
#include "transmission/AtTypes.h"

// AT の変速制御（TCU）と締結要素の油圧。純粋な計算で、動力学（Chrono のシャフト）は
// 知らない: 前のステップの回転・トルク（AtPlantState）と運転の入力（AtInputs）から、
// このステップの各要素とロックアップの容量（N·m）を決める。
//
// ---- 変速 ----------------------------------------------------------------------
// 変速線はタービン回転（その段のとき）で、開度の 1.5 乗で上げ線 upLight〜upFull /
// 下げ線 downLight〜downFull の間を動く。下げは「下げ線より上に収まるいちばん高い段」へ
// 飛び越す（1 要素の掛け替えで行ける段だけ）、キックダウンは踏み込んだ瞬間に上げ線の
// 手前のいちばん低い段、ブレーキ中は上げない、M レンジは 1 段ずつで回し過ぎの下げは断る。
// 隣の段とは締結要素 1 つの掛け替え（clutch-to-clutch）:
//   パワーオンの上げ / コーストの下げ … 準備（fill）→ トルク相（解放側を抜き、締結側へ
//       分担を移す。比は旧段のまま、出力トルクが落ちる）→ イナーシャ相（締結側を滑らせて
//       タービン回転を新段へ。回転の軌道を PI で追い、上げではエンジントルクを絞る）→ 同期
//   パワーオンの下げ / コーストの上げ … 解放側を滑らせてタービン回転を新段へ上げ
//       （イナーシャ相が先）、同期したら締結側へ掛け替える
// パワーオン / オフはエンジンが出しているトルクで決める（入力軸のトルクは急ブレーキで
// エンジンの慣性が押し込むぶんを含み、減速中でも正になるため）。
// ---- 油圧 ----------------------------------------------------------------------
// 要素はピストン室の充填（fill 秒は力が出ない）+ 一次遅れ（tau）。準備の油圧 kiss は
// 戻しばねを詰めるだけで力は出ない（効きは (p − kiss) / (1 − kiss)）。ロックアップは
// 一次遅れ（開けるのは締める 5 倍速い）。
namespace wizengine {
namespace transmission {

struct AtInputs {
    double engineTorque = 0.0;  // N·m（input = torque。燃焼・摩擦込みの正味）
    double engineRpm = 0.0;     // rpm（input = speed）
    double throttle = 0.0;      // 0..1（変速線・ロックアップ・ライン圧の判断）
    double brake = 0.0;         // 0..1（車両のブレーキ）
    int select = 3;             // 0 = P, 1 = R, 2 = N, 3 = D, 4 = M（手動）
    int manualGear = 1;         // select = M のときの段（1..）
    double outputRpm = 0.0;     // load = dyno: 出力軸の回転数（0 なら文書の dynoRpm）
    bool outputRpmSet = false;
    double grade = 0.0;         // 勾配 (rad、+ = 上り)
    double couple = 1.0;        // input = body: エンジン軸を剛体とつなぐか（0 = 切り離す）
};

// 前のステップの結果（プラントが渡す）。
struct AtPlantState {
    double wEngine = 0.0, wInput = 0.0, wOutput = 0.0;  // rad/s
    double kmh = 0.0;           // 車速（ロックアップの判断）
    double engineTorque = 0.0;  // エンジンが出したトルク（パワーオン / オフの判断）
    double inputTorque = 0.0;   // 歯車列の入力軸へ入ったトルク（分担の見積もり）
};

class AtControl {
public:
    // kin は AtControl より長く生きること（参照を持つ）。
    AtControl(const AtDesc& desc, const AtKinematics& kin);

    // 静止・N に戻す（N で締結する要素は最初から満圧）。
    void reset();
    void step(double dt, const AtInputs& in, const AtPlantState& ps);

    // 要素の今の容量（N·m、静摩擦。満圧の容量 × 油圧の効き）と油圧（0..1）。
    double elementCapacity(std::size_t e) const;
    double elementPressure(std::size_t e) const { return elements_[e].p; }
    double lockupCapacity() const { return desc_.lockup.capacity * lockupP_; }
    double lockupPressure() const { return lockupP_; }
    // エンジントルクの要求（イナーシャ相で絞る倍率。1 = 絞らない）。
    double torqueRequest() const { return request_; }
    double linePressure() const { return linePressure_; }  // bar

    int gear() const;        // 1..、-1 = R、0 = N / P
    int targetGear() const;
    bool shifting() const { return shift_.active; }
    int phase() const { return shift_.active ? shift_.phase : 0; }  // 1 準備 2 トルク 3 イナーシャ 4 同期

private:
    struct Element {
        double cmd = 0.0, fill = 0.0, p = 0.0;
    };
    struct Shift {
        bool active = false;
        bool garage = false;     // N ⇄ D / R など（要素の足し引き）
        int from = -1, to = -1;
        int oc = -1, og = -1;    // 掛け替えの締結側 / 解放側
        bool ocFirst = true, up = true, powerOn = true;
        int phase = 0;
        double t = 0.0;
        double wStart = 0.0, integ = 0.0, ogCmd0 = 1.0, ogHold = 1.0;
        std::vector<int> on, off;  // garage: 足す / 抜く要素
    };

    void runTcu(double dt, const AtInputs& in);
    void startShift(int to);
    void hydraulics(double dt);
    double eqRpm(int g) const;  // 段 g のときのタービン回転（今の出力回転から）
    double turbineSideInertia() const;

    AtDesc desc_;
    const AtKinematics& kin_;
    std::vector<Element> elements_;
    AtPlantState ps_;
    int cur_ = -1, want_ = -1;
    double sinceShift_ = 10.0;
    bool kickArmed_ = true;
    Shift shift_;
    double request_ = 1.0;
    double lockupCmd_ = 0.0, lockupP_ = 0.0;
    double linePressure_ = 0.0;
    double throttle_ = 0.0;  // このステップのアクセル（パワーオン / オフの判断）
};

}  // namespace transmission
}  // namespace wizengine
