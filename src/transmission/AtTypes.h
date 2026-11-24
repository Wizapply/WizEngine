#pragma once

#include <string>
#include <vector>

// 遊星歯車式オートマチックトランスミッション（AT）の設計値。
//
// Chrono も Filament も知らない純粋な値型で、シーン文書の <control> の
// <transmission>（transmission/AtXml.h）と 1 対 1。動力学は Chrono の 1D シャフト
// （physics/ShaftNetworks、組むのは components/AtDriveline）、変速の判断と油圧は
// transmission/AtControl、段の変速比と要素の分担は transmission/AtKinematics。
//
// 駆動系のつながり（エンジン側 → 車輪）:
//
//   エンジン（クランク + フレックスプレート + コンバータのカバー + ポンプ羽根車）
//     ├─ トルクコンバータ（流体: ポンプ → タービン。トルク比 TR・容量係数 K）
//     └─ ロックアップクラッチ → ダンパの 1 次側 ─ ねじりばね ─┐
//   タービン = 歯車列の入力軸（inputShaft） ←────────────────┘
//   歯車列: 軸（shaft）・単純遊星（gearset: 太陽 / キャリヤ / 内歯車）・
//           締結要素（element: クラッチ = 軸と軸、ブレーキ = 軸とケース）
//   出力軸（outputShaft）─ 終減速 + プロペラ / ドライブシャフトのねじり ─ 車輪 + 車両
//
// 歯車列の形はすべてデータ（軸の名前・遊星の 3 部材・要素のつなぎ先・段ごとの
// 締結表）なので、別の AT（6 速の Lepelletier など）も同じ口で書ける。既定値の
// jr913eDesc() は JATCO JR913E（FR 用 9 速、ギア比 5.425 … 0.597 / R 4.799）。
namespace wizengine {
namespace transmission {

// エンジン側の与え方。
//   Torque … エンジン軸（クランク + フライホイール + コンバータのカバー）を AT の
//            シャフトとして持ち、トルクを入力で受ける（trans:<名前>.engine_torque）。
//   Speed  … エンジン軸の回転数を与える（trans:<名前>.engine_rpm。Chrono のシャフトの
//            速度モータ）。AT がエンジンから取ったトルクは load_torque。
//   Body   … エンジン軸をシーンの 3D の剛体（<engine body="crank" axis="0 0 1">）に
//            Chrono の ChShaftBodyRotation で直結する。トルクは Chrono の中でそのまま
//            行き来する（受け渡しの遅れが無い）。剛体の慣性はその剛体が持つので、
//            エンジン軸の慣性はコンバータのカバー + 羽根車（pumpInertia）だけ。
enum class InputMode { Torque, Speed, Body };
// 出力側。
//   Vehicle … 終減速 + 車輪 + 車両（転がり・空気・勾配・ブレーキ）。
//   Dyno    … 出力軸の回転数を与える（シャシダイナモ / 台上試験）。
enum class LoadMode { Vehicle, Dyno };
enum class ElementKind { Clutch, Brake };

struct ShaftDesc {
    std::string name;
    double inertia = 0.01;  // kg m²（ドラム・ハブ・歯車を含めたその軸ぶん）
    double drag = 0.0;      // ケースへの粘性損失 N·m·s/rad（軸受・かき混ぜ）
};

// 単純遊星（シングルピニオン）。Willis の式 ωs + k ωr = (1 + k) ωc、k = zr / zs。
struct GearsetDesc {
    std::string name;
    std::string sun, carrier, ring;  // つながる軸の名前
    int zs = 40, zr = 90;            // 太陽・内歯車の歯数
    int zp = 0;                      // 遊星の歯数（0 = (zr - zs) / 2 で求める。表示用）
    int planets = 4;
    double efficiency = 0.985;       // 太陽 ⇄ 遊星 ⇄ 内歯車のかみ合い 1 段ぶんの効率
};

// 湿式多板の締結要素。トルク容量 = capacity × 油圧の効き（0..1）× 摩擦係数の比。
struct ElementDesc {
    std::string name;
    ElementKind kind = ElementKind::Clutch;
    std::string a, b;          // クラッチは軸 a と b、ブレーキは軸 a とケース
    double capacity = 0.0;     // N·m（満圧・静摩擦）。0 = 自動（段ごとの分担 × rating × 1.4）
    double fill = 0.08;        // ピストン室を満たす時間 (s)。満ちるまで力は出ない
    double tau = 0.03;         // 満ちた後の油圧の一次遅れ (s)
    double dynamic = 0.85;     // すべり始めの動摩擦 / 静摩擦（すべり速度で 1 へ戻る）
    double drag = 0.003;       // 開放時の引きずり N·m·s/rad（相対回転に比例）
};

// 締結表の 1 行。name は "1".."9"（前進）・"R"（後退）。
struct GearDesc {
    std::string name;
    std::vector<std::string> elements;
};

// トルクコンバータ（Chrono の ChShaftsTorqueConverter）。T_pump = (n_p / K(SR))²
// （n は rpm。Chrono へは (rad/s)/√(N·m) に直して渡す）、T_turbine = TR(SR) T_pump、
// SR = n_t / n_p。K = 0 の点は「容量ゼロ」（SR = 1 で流体のトルクが消える）。表の点の間は
// 容量 1/K² を直線でつなぐ（components/AtDriveline が細かい点に引き直して渡す = トルクが
// SR = 1 へ向けてなめらかに 0 になる。K を直線でつなぐと 0 の手前でトルクが崖になり、
// Chrono が陽的に与える流体のトルクが振動する）。逆駆動（エンジンブレーキ。タービン >
// ポンプ）は Chrono の約束で SR を 1 で折り返して同じ表を引き、トルク比 1（流体継手）。
struct ConverterDesc {
    std::vector<double> sr = {0.0, 0.2, 0.4, 0.6, 0.7, 0.8, 0.85, 0.9, 0.95, 1.0};
    std::vector<double> k = {120, 121, 123, 128, 133, 142, 150, 168, 230, 0};
    std::vector<double> tr = {1.90, 1.71, 1.52, 1.33, 1.23, 1.12, 1.05, 1.0, 1.0, 1.0};
    double pumpInertia = 0.10;     // kg m²（カバー + 羽根車。エンジン側に足す）
    double turbineInertia = 0.05;  // kg m²（タービン。入力軸に足す）
};

// ロックアップクラッチとねじりダンパ（エンジンのトルク変動を吸う）。
struct LockupDesc {
    double capacity = 900.0;   // N·m（満圧）
    double tau = 0.25;         // 油圧の一次遅れ (s) = つながる柔らかさ
    int fromGear = 2;          // この段以上で締結
    double speed = 18.0;       // km/h 以上で締結（5 km/h のヒステリシス）
    double stiffness = 1500.0; // ダンパのねじり剛性 N·m/rad
    double damping = 3.0;      // N·m·s/rad
    double inertia = 0.03;     // 1 次側（ロックアップのピストン + ダンパの入力）kg m²
};

// エンジン（input = Torque のとき）。トルクは外から受ける（燃焼・摩擦込みの正味）。
struct EngineDesc {
    double inertia = 0.18;    // kg m²（クランク + フライホイール。コンバータは別）
    double idle = 650.0;      // rpm（これを下回るとアイドル制御が押す）
    double idleGain = 1.0;    // N·m / rpm（アイドル制御の比例ゲイン、上限 200 N·m）
    double redline = 7000.0;  // rpm（これより上は燃料カット = 正のトルクを捨てる）
    // input = Body: つなぐ剛体の名前と、回転軸（ワールドの向き。シミュレート開始時の
    // 剛体の姿勢でローカルへ直す）。+ の向きに回るのが正転。
    std::string body;
    double axis[3] = {0.0, 0.0, 1.0};
};

// 油圧ポンプ（エンジン駆動）。損失トルク = 吐出量 × 油圧 / (2π 効率)。
struct PumpDesc {
    double displacement = 14.0;  // cm³/rev
    double pressureMin = 4.0;    // bar（無負荷）
    double pressureMax = 18.0;   // bar（全負荷）
    double efficiency = 0.85;
};

// 変速の判断と変速中の油圧制御（TCU）。回転数は「その段のときのタービン回転」。
struct ShiftDesc {
    double upLight = 1500.0;    // rpm: アクセル 0 での上げ
    double upFull = 6500.0;     // rpm: アクセル全開での上げ
    double downLight = 1050.0;  // rpm: アクセル 0 での下げ
    double downFull = 3500.0;   // rpm: アクセル全開での下げ（上げ線 ÷ 最大の段差より下に）
    double kickdown = 0.9;      // アクセル開度: これ以上で飛び越しの下げ
    double minTime = 0.6;       // s: 前の変速からこれだけ経つまで次を判断しない
    double torquePhase = 0.12;  // s: トルク相（掛け替え）の長さ
    double inertiaPhase = 0.30; // s: イナーシャ相（回転合わせ）の目標の長さ
    double reduction = 0.4;     // イナーシャ相でエンジントルクを絞る割合（パワーオンの上げ）
    double kiss = 0.06;         // 準備の油圧（ピストンを詰めるだけ。力は出ない）
    double margin = 1.3;        // 保持の余裕（分担トルク × これ）
    double kp = 2.0;            // イナーシャ相の回転制御: N·m / (rad/s)
    double ki = 40.0;           // N·m / rad
    double engage = 0.5;        // s: N → D / R の締結の長さ
};

// 出力側（終減速・ドライブシャフト・車両、または台上の回転数）。
struct OutputDesc {
    double inertia = 0.05;          // kg m²（出力軸 + プロペラシャフト。出力軸に足す）
    double finalDrive = 3.133;
    double finalEfficiency = 0.97;
    double stiffness = 6000.0;      // N·m/rad（車輪側に換算したドライブシャフトのねじり）
    double damping = 60.0;          // N·m·s/rad
    double wheelRadius = 0.337;     // m
    double mass = 1700.0;           // kg（乗員込み）
    double wheelInertia = 4.8;      // kg m²（4 輪 + ブレーキ + ハブ）
    double crr = 0.012;             // 転がり抵抗係数
    double cda = 0.62;              // m²（Cd × 前面投影面積）
    double rho = 1.2;               // kg/m³
    double brake = 25000.0;         // N·m（ブレーキ全開の車輪トルク、4 輪合計）
    double dynoRpm = 0.0;           // load = Dyno: 出力軸の回転数の既定値
};

struct AtDesc {
    std::string name = "at";
    InputMode input = InputMode::Torque;
    LoadMode load = LoadMode::Vehicle;
    double rating = 700.0;  // 入力トルクの定格 N·m（要素の容量の自動決定に使う）
    std::string inputShaft = "input", outputShaft = "output";
    EngineDesc engine;
    ConverterDesc converter;
    LockupDesc lockup;
    PumpDesc pump;
    ShiftDesc shift;
    OutputDesc output;
    std::vector<ShaftDesc> shafts;
    std::vector<GearsetDesc> gearsets;
    std::vector<ElementDesc> elements;
    std::vector<GearDesc> gears;
    std::vector<std::string> neutral;  // N / P で締結しておく要素（D / R へすぐ入れる）
};

// JATCO JR913E（FR 9 速）。歯車列の形は 9G-TRONIC 系の 4 遊星 + 6 要素
// （ブレーキ A B C、クラッチ D E F。1 段 3 要素で、隣の段とは 1 要素の掛け替え）、
// 歯数は公表のギア比 10 個を小数 3 桁まで再現する組（README / CLAUDE.md）。
AtDesc jr913eDesc();

const char* inputModeName(InputMode m);
const char* loadModeName(LoadMode m);
const char* elementKindName(ElementKind k);
InputMode inputModeFromName(const std::string& s, InputMode fallback);
LoadMode loadModeFromName(const std::string& s, LoadMode fallback);
ElementKind elementKindFromName(const std::string& s, ElementKind fallback);

// 値の範囲を丸める（負の慣性・空の表など）。丸めた内容は warnings に英文で。
AtDesc clampAt(const AtDesc& in, std::vector<std::string>* warnings = nullptr);

// 段の名前 → 表示用の番号（"1".."9" = 1..9、"R" = -1、それ以外 = 0）。
int gearNumber(const std::string& name);

}  // namespace transmission
}  // namespace wizengine
