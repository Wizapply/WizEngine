#pragma once

#include <string>
#include <vector>

#include "vehicle/Formula.h"

// 車両の設計値（VehicleDesc）と、実行時の入出力の型。
//
// NWH Vehicle Physics 2 の構成に倣った「グラフ型パワートレイン + 車輪ごとの
// WheelController」の設計図で、値だけを持つ（Chrono / Filament / JSON のどれも
// 出てこない）。シーン文書では <body> の中の <vehicle> 節に対応し
// （VehicleXml.h）、BodyDesc が hasVehicle / vehicle として持つ。
//
// ---- 座標の約束 ------------------------------------------------------------
//   車体ローカル: 前 = -Z、右 = +X、上 = +Y（右手系・Y up。Filament のカメラ
//   が -Z を向くのと同じ向き）。軸（axle）の z は「前向きの距離」= ローカル
//   -Z 方向に z だけ進んだ位置。左右の車輪は ±halfTrack（+X が右）。
//   角度は度、回転数は rpm、それ以外は SI（m, kg, N, s）。
//   舵角は +で左に切る（+Y まわりの正回転 = 上から見て反時計回り）。
//
// ---- 値の増やし方 ----------------------------------------------------------
//   ここに属性を足したら VehicleXml.cpp の書き出し / 読み込みの対（隣り合わ
//   せ）を両方直す。読み込みは必ず既定値付き（無い属性 = 既定値）で、古い
//   文書はそのまま読める。
namespace wizengine {
namespace vehicle {

// 折れ線（x 昇順）。エンジンのトルク曲線に使う。
struct CurvePoint {
    double x = 0.0;
    double y = 0.0;
};

// 線形補間。範囲外は端の値（空なら fallback）。
double sampleCurve(const std::vector<CurvePoint>& curve, double x,
                   double fallback = 1.0);

// エンジン。トルク = throttle × maxTorque × curve(rpm) − 摩擦。
struct EngineDesc {
    double maxTorque = 300.0;     // Nm（曲線の 1.0 に対応する値）
    double idleRpm = 900.0;       // アイドル制御の目標
    double maxRpm = 6500.0;       // レブリミッタ（超えたらスロットル 0）
    double inertia = 0.25;        // kg·m²（フライホイール込み）
    double friction = 0.03;       // Nm per rad/s（回転比例の損失 = エンジンブレーキ）
    double frictionConst = 10.0;  // Nm（回転によらない損失）
    // rpm → 0..1 の正規化トルク。空なら defaultTorqueCurve()。
    std::vector<CurvePoint> torqueCurve;
};
std::vector<CurvePoint> defaultTorqueCurve();

// クラッチ。自動クラッチ: エンジン回転が engageRpm から fullRpm の間で
// 0 → 1 につながる。maxTorque は伝えられる上限（滑り始めるトルク）。
struct ClutchDesc {
    double maxTorque = 500.0;  // Nm
    double engageRpm = 1300.0;
    double fullRpm = 2200.0;
};

// 変速機 + ファイナル。gears は 1 速から順。automatic は回転数で自動変速。
struct GearboxDesc {
    std::vector<double> gears{3.6, 2.1, 1.4, 1.0, 0.8};
    double reverse = 3.4;
    double finalDrive = 3.9;
    double efficiency = 0.92;
    double shiftUpRpm = 5800.0;
    double shiftDownRpm = 2300.0;
    double shiftTime = 0.25;  // s（この間はトルクを切る）
    bool automatic = true;
};

// デフの種類。Open = 半分ずつ、Locked = 左右を剛結、Lsd = 回転差に比例した
// ロックトルク（stiffness × Δω、上限 lockTorque）。
enum class DiffKind { Open, Locked, Lsd };

const char* diffKindName(DiffKind k);
DiffKind diffKindFromName(const std::string& s, DiffKind fallback);

struct DifferentialDesc {
    DiffKind kind = DiffKind::Open;
    double stiffness = 20.0;    // Nm per rad/s（Lsd）
    double lockTorque = 300.0;  // Nm（Lsd の上限）
    double bias = 0.5;          // センターデフだけ: 前軸へ回すトルクの割合
};

// タイヤ。Pacejka の簡易 Magic Formula（B, C, E。D = mu × Fz）を縦横で別に持つ。
struct TireDesc {
    double radius = 0.32;   // m
    double width = 0.22;    // m（見た目だけ。接地は 1 点）
    double inertia = 1.2;   // kg·m²
    double mu = 1.2;        // 摩擦係数のピーク（D）。ゲーム寄りに少し高め
    double bx = 12.0, cx = 1.65, ex = 0.97;  // 縦（スリップ比）
    double by = 12.0, cy = 1.4, ey = -1.0;   // 横（スリップ角・ラジアン）
    double relaxation = 0.15;  // 緩和長 m（0 = 即時）
    double rollingResistance = 0.015;  // 転がり抵抗係数
    // 「スリップ → 力」をノード式で置き換えるときの名前（VehicleDesc::formulas
    // の中の <formula name>）。空 = 組み込みの Magic Formula。
    std::string formula;
    // ---- ソフトタイヤ（vehicle/SoftTire.h）---------------------------------
    // soft = true でタイヤの見た目が質点ばねの変形メッシュになり、物理では
    // 径方向ばね（サスと直列。潰れ = 荷重 / stiffness、上限は半径の 45%）が
    // 効く。damping はメッシュのばねの減衰比、segments / rows はメッシュの
    // 分割（周方向 / 幅方向のトレッド列）、iterations はばね反復。
    bool soft = false;
    double stiffness = 150000.0;  // N/m
    double damping = 0.4;
    int segments = 24;
    int rows = 3;
    int iterations = 3;
};

// サスペンション（レイキャスト式）。restLength は伸びきった長さ、travel は
// 縮められる量（超えるとバンプストップ）。
struct SuspensionDesc {
    double restLength = 0.35;   // m
    double travel = 0.25;       // m
    double stiffness = 35000.0; // N/m
    double bump = 3500.0;       // N·s/m（縮み側のダンパ）
    double rebound = 4500.0;    // N·s/m（伸び側）
    double antiRoll = 6000.0;   // N/m（左右の縮み差に掛かる）
};

// 1 本の軸 = 左右 2 輪。
struct AxleDesc {
    double z = 1.3;          // 前向きの距離 m（+ = 前）
    double y = -0.15;        // 取り付け高さ（車体ローカル Y）
    double halfTrack = 0.8;  // 左右の車輪までの距離 m
    double steerDeg = 0.0;   // 最大舵角（0 = 操舵しない）
    double ackermann = 1.0;  // 0 = 平行、1 = 完全アッカーマン
    bool driven = false;     // 駆動軸か
    double brakeBias = 1.0;  // ブレーキトルクの倍率
    double handbrake = 0.0;  // ハンドブレーキの倍率（後輪に 1）
    DifferentialDesc diff;   // 左右の分配
    TireDesc tire;
    SuspensionDesc suspension;
};

struct VehicleDesc {
    std::vector<AxleDesc> axles;
    EngineDesc engine;
    ClutchDesc clutch;
    GearboxDesc gearbox;
    DifferentialDesc center;   // 駆動軸が 2 本以上のときの前後分配
    double brakeTorque = 2500.0;  // Nm（1 輪あたりの最大、× axle.brakeBias）
    double dragArea = 0.7;        // Cd × A（m²）。空気抵抗 0.5 ρ CdA v²
    double steerRate = 4.0;       // 舵の追従速度（1/s: フル舵まで 0.25 s）
    // 速度感応ステアリング: 最大舵角を 1 / (1 + (v / steerSpeedRef)²) 倍に
    // 絞る（この速度で半分）。キーボードのフル舵で高速に前輪が飽和して
    // 滑り出すのを防ぐ。0 = 絞らない。
    double steerSpeedRef = 12.0;  // m/s
    // トラクションコントロール: 駆動輪のスリップ比がこれを超えたら
    // スロットルを絞る。0 = 無効。
    double tractionControl = 0.25;
    double pedalRate = 6.0;       // ペダルの追従速度（1/s）
    int ticks = 10;               // 物理 1 ステップ内のパワートレイン反復数
    // タイヤ / サスの既定値（<vehicle> 直下に書いた <tire>/<suspension>）。
    // 各軸は自分の値を持つので、ここは文書の読み書きの都合だけ。
    TireDesc tireDefaults;
    SuspensionDesc suspensionDefaults;
    // ノード式（<vehicle> の <formula>）。タイヤが名前で参照する。
    std::vector<FormulaGraphDesc> formulas;
};

// 乗用車 1 台ぶんの既定構成（前輪操舵・後輪駆動の 2 軸）。
VehicleDesc defaultVehicleDesc();
// 常識的な範囲に丸める（文書と /input は誰でも書けるので信じない）。
VehicleDesc clampVehicle(VehicleDesc d);

// ---- 実行時の入力 ------------------------------------------------------------
// throttle = 前へ進むペダル、brake = 後ろ（減速）のペダル。自動モードでは
// 停車中に brake を踏み続けるとリバースに入り、その間は役割が入れ替わる
// （キーボードの W / S でそのまま運転できるように）。
struct VehicleInput {
    double throttle = 0.0;   // 0..1
    double brake = 0.0;      // 0..1
    double steer = 0.0;      // -1..1（+ = 左）
    double handbrake = 0.0;  // 0..1
};

// ---- 計測値（UI・ログ用）-----------------------------------------------------
struct WheelTelemetry {
    double compression = 0.0;   // m
    double load = 0.0;          // Fz N
    double angularVelocity = 0.0;  // rad/s
    double slipRatio = 0.0;
    double slipAngleDeg = 0.0;
    double forceX = 0.0, forceY = 0.0;  // N（タイヤ座標）
    double steerDeg = 0.0;
    double spinAngle = 0.0;     // 見た目の回転角（rad、積算）
    double deflection = 0.0;    // ソフトタイヤの径方向の潰れ (m)
    bool grounded = false;
};

struct VehicleTelemetry {
    double rpm = 0.0;
    int gear = 0;              // -1 = R, 0 = N, 1.. = 前進
    bool shifting = false;
    double speed = 0.0;        // m/s（前向きが +）
    double clutch = 0.0;       // 0..1 のつながり
    double engineTorque = 0.0; // Nm
    std::vector<WheelTelemetry> wheels;
};

}  // namespace vehicle
}  // namespace wizengine
