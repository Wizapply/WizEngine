#pragma once

#include <functional>
#include <memory>

#include "vehicle/Formula.h"
#include "vehicle/VehicleMath.h"
#include "vehicle/VehicleTypes.h"

// 1 輪ぶんの「サスペンション + 接地検出 + タイヤ摩擦」（NWH の WheelController
// 3D に相当）。車輪は剛体にしない: 車体からレイを下ろして地面までの距離で
// 縮み量を決め、ばね・ダンパの力を取り付け点に、タイヤの縦横力を接地点に
// 「車体へ掛ける力」として返す。
//
// 時計は 2 つある（CLAUDE.md「車両」の章）:
//   updateContact … 物理 1 ステップに 1 回。接地・縮み・接地点の速度を確定する
//                   （車体はステップ内で動かないので、ここは 1 回でよい）。
//   tick          … パワートレインの反復ごと。車輪の ω を受けてタイヤ力を
//                   更新し、車輪への反作用トルクを返す。ステップ末に平均を
//                   取って車体へ渡す（finishStep）。
namespace wizengine {
namespace vehicle {

// 地面への問い合わせ。origin から dir（単位ベクトル）へ maxDistance まで。
struct GroundHit {
    bool hit = false;
    double distance = 0.0;
    Vec3 point;
    Vec3 normal{0.0, 1.0, 0.0};
};
using GroundQuery = std::function<GroundHit(const Vec3& origin, const Vec3& dir,
                                            double maxDistance)>;
// y = 0 の無限平面（物理の床と同じ高さ）。
GroundHit planeGroundQuery(const Vec3& origin, const Vec3& dir,
                           double maxDistance);

// 車体の状態（Chrono から詰め替える）。角速度はワールド座標。
struct ChassisState {
    Vec3 position;
    Quat rotation;
    Vec3 velocity;
    Vec3 angularVelocity;
    double mass = 1000.0;
};

class WheelController {
public:
    WheelController(const TireDesc& tire, const SuspensionDesc& suspension,
                    const Vec3& localAttach);

    void reset();

    // 「スリップ → 力」をノード式で置き換える（nullptr = 組み込み）。入出力の
    // 約束は TireFormula.h。式が失敗（NaN・実行時エラー）したティックは
    // 組み込みで代用し、回数を formulaFailures() に数える。
    void setTireFormula(std::unique_ptr<FormulaInstance> formula);
    const FormulaInstance* tireFormula() const { return formula_.get(); }
    int formulaFailures() const { return formulaFailures_; }

    // ステップの先頭: 接地検出とサス力。steerRad は舵角（+ = 左）、
    // massShare はこの車輪が受け持つ車体質量（力の上限に使う）。
    void updateContact(const ChassisState& chassis, double steerRad,
                       const GroundQuery& ground, double massShare,
                       double stepDt);
    // アンチロールバーなど、外から荷重を足す（updateContact の後、tick の前）。
    void addLoad(double newtons);

    // パワートレインの 1 反復。車輪の ω（rad/s）を受けてタイヤ力を更新し、
    // 車輪に返る反作用トルク（Fx × r、ω を減らす向きが +）を返す。
    double tick(double omega, double tickDt);
    // ステップ末: 反復ぶんの力を平均して車体へ渡す値にする。
    void finishStep(int ticks);

    // ---- 出力（finishStep の後）------------------------------------------
    bool grounded() const { return grounded_; }
    double load() const { return load_; }                // Fz N
    double compression() const { return compression_; }  // m（サスの縮み）
    // ソフトタイヤの径方向の潰れ (m)。接地点は車輪中心から radius - deflection
    // の距離にある（剛タイヤなら 0）。
    double deflection() const { return deflection_; }
    const Vec3& contactNormal() const { return normal_; }
    Vec3 suspensionForce() const;   // 取り付け点に掛ける（ストラット軸方向）
    Vec3 suspensionPoint() const { return attachWorld_; }
    Vec3 tireForce() const;         // 接地点に掛ける（前後 + 横）
    Vec3 contactPoint() const { return contactPoint_; }
    // 転がり抵抗をトルクにしたもの（C_rr × Fz × r。ブレーキと同じ扱い）。
    double rollingTorque() const;
    double reactionTorque() const { return forceX_ * tire_.radius; }

    double radius() const { return tire_.radius; }
    double inertia() const { return tire_.inertia; }
    // 見た目用: 車輪中心（ワールド）と、車体ローカルの取り付け点。
    Vec3 wheelCenter() const;
    const Vec3& localAttach() const { return localAttach_; }
    double steerRad() const { return steerRad_; }
    void addSpin(double radians) { spinAngle_ += radians; }
    double spinAngle() const { return spinAngle_; }

    WheelTelemetry telemetry() const;

private:
    TireDesc tire_;
    SuspensionDesc sus_;
    Vec3 localAttach_;

    // updateContact が決める（ステップ内で不変）。
    bool grounded_ = false;
    double compression_ = 0.0;
    double deflection_ = 0.0;
    double load_ = 0.0;
    double steerRad_ = 0.0;
    double stepDt_ = 1.0 / 60.0;
    double massShare_ = 250.0;
    Vec3 attachWorld_;
    Vec3 strutUp_{0.0, 1.0, 0.0};  // 車体の上向き（ストラットの軸）
    Vec3 contactPoint_;
    Vec3 normal_{0.0, 1.0, 0.0};
    Vec3 forward_{0.0, 0.0, -1.0};  // 舵角込みの前向き（地面に沿う）
    Vec3 right_{1.0, 0.0, 0.0};
    double vx_ = 0.0, vy_ = 0.0;   // 接地点の速度（タイヤ座標）

    // tick が更新する状態。relaxX_ / relaxY_ は緩和長の一次遅れの状態、
    // forceX_ / forceY_ はそれに上限を掛けた「このティックで掛ける力」。
    double relaxX_ = 0.0, relaxY_ = 0.0;
    double forceX_ = 0.0, forceY_ = 0.0;
    double sumX_ = 0.0, sumY_ = 0.0;
    double avgX_ = 0.0, avgY_ = 0.0;
    double slipRatio_ = 0.0, slipAngle_ = 0.0;
    double omega_ = 0.0;
    double spinAngle_ = 0.0;

    std::unique_ptr<FormulaInstance> formula_;
    int formulaFailures_ = 0;
};

}  // namespace vehicle
}  // namespace wizengine
