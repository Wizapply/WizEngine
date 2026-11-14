#pragma once

#include <string>
#include <vector>

// 制御器（ノード式をブロック線図として回すもの）の信号名。
//
// ノード式の in / out ノードの name が「シーンのどこを読むか / どこへ書くか」
// を表す。Chrono も Filament も知らない純粋な解析で、実際に値を読み書き
// するのは ControlComponent。単位はすべて SI（m, s, rad, N, N·m）- 文書の
// 角度は度だが、制御の信号は rad で統一する（Simulink と同じ）。
//
// ---- センサ（in ノードの name）-----------------------------------------------
//   time                       … シミュレート開始からの時間 (s)
//   body:<名前>.<量>           … オブジェクトの状態
//       x y z                  … 位置 (m、ワールド)
//       vx vy vz               … 速度 (m/s、ワールド)
//       wx wy wz               … 角速度 (rad/s、ワールド)
//       rx ry rz               … 姿勢のオイラー角 (rad、R = Rz*Ry*Rx。エディタの
//                                回転欄と同じ並び)
//       speed                  … 速さ |v| (m/s)
//       vfwd vlat              … 自分の向きで見た速度: 前（-Z）/ 右（+X）(m/s)
//   joint:<名前>.<量>          … ジョイント（body1 を body2 から見た相対運動、
//                                軸はジョイントの axis）
//       angle rate             … 軸まわりの角度 (rad、開始時 0。連続) / 角速度 (rad/s)
//       pos vel                … 軸方向の変位 (m、開始時 0) / 速度 (m/s)
//       force torque           … 拘束の反力 (N) / 反トルク (N·m)（大きさ）
//   vehicle:<名前>.<量>        … 車両（<vehicle> を持つオブジェクト）
//       speed rpm gear         … 前向きの速さ (m/s) / エンジン回転 / ギア (-1 = R)
//   param:<名前>               … 制御器のパラメータ（文書の <control><param>）。
//                                ブラウザのスライダーでシミュレート中に変えられる
//
// ---- アクチュエータ（out ノードの name）-------------------------------------
//   joint:<名前>.target        … モータの目標値（speed = rad/s or m/s、
//                                position = rad or m、force = N·m or N）
//   body:<名前>.fx fy fz       … 力 (N、ワールド。重心に)
//   body:<名前>.tx ty tz       … トルク (N·m、ワールド)
//   body:<名前>.lfx lfy lfz    … 力 (N、オブジェクトのローカル軸。推力など)
//   body:<名前>.ltx lty ltz    … トルク (N·m、ローカル軸)
//   vehicle:<名前>.throttle brake steer handbrake
//                              … 運転の入力（0..1、steer は -1..1 で + が左）。
//                                書いた項目だけブラウザの入力を置き換える
//   scope:<ラベル>             … 記録だけ（スコープに出る）
//
// 名前は「最初の : の前」が種類、「最後の . の後」が量、その間が対象の名前
// （オブジェクト / ジョイントの name。. を含んでもよい）。
namespace wizengine {
namespace control {

enum class Source { Time, Body, Joint, Vehicle, Scope, Param };

enum class Quantity {
    None,
    // body（センサ）
    X, Y, Z, Vx, Vy, Vz, Wx, Wy, Wz, Rx, Ry, Rz, Speed, VFwd, VLat,
    // joint（センサ）
    Angle, Rate, Pos, Vel, Force, Torque,
    // vehicle（センサ）
    VehSpeed, Rpm, Gear,
    // アクチュエータ
    Target,
    Fx, Fy, Fz, Tx, Ty, Tz, LFx, LFy, LFz, LTx, LTy, LTz,
    Throttle, Brake, Steer, Handbrake,
};

struct SignalRef {
    Source source = Source::Time;
    std::string target;   // オブジェクト / ジョイント / パラメータの名前、scope のラベル
    Quantity quantity = Quantity::None;
};

// 解析。読めなければ false で error に理由（英語、ログ用）。
bool parseSensor(const std::string& name, SignalRef& out, std::string& error);
bool parseActuator(const std::string& name, SignalRef& out, std::string& error);

// UI の候補用: 種類ごとの量の名前（"x", "vx", ...）。
const std::vector<std::string>& sensorQuantities(Source source);
const std::vector<std::string>& actuatorQuantities(Source source);

}  // namespace control
}  // namespace wizengine
