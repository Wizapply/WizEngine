#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlMath.h"
#include "control/ControlSignals.h"
#include "document/EditorTypes.h"
#include "scene/SceneComponent.h"
#include "formula/Formula.h"
#include "formula/LuaFormula.h"
#include "components/AtDriveline.h"
#include "fluid/Atmosphere.h"

class VehicleComponent;
class EffectsComponent;

// 制御器（Simulink のブロック線図に相当）を回す SceneComponent。
//
// 制御器 = 計算式アセット（ノード式）+ 実行周期（EditorState::controllers、
// 文書の <control><controller formula period/>）。式の in / out ノードの name
// がセンサ / アクチュエータ（control/ControlSignals.h）で、ここが Chrono の
// 状態を読んで式へ渡し、出力を Chrono へ書く。
//
//   onPhysicsStep (PHYSICS) … シミュレート中、物理の積分の前に:
//       1. 周期が来た制御器を評価（センサ = 直前のステップの結果）
//       2. 全制御器の出力を毎ステップ当てる（次の評価まで保持 = ゼロ次ホールド。
//          力・トルクは毎ステップ掛け続ける必要があるため）
//       3. 評価した時刻の入出力をスコープ用に記録
//   onEditorStep  (PHYSICS) … 実行状態を捨てる（次のシミュレート開始で作り直す）
//
// 変数（var:<名前>）: 制御器の間で共有する値の表（vars_）。出力を当てる
// （applyOutputs）ときに書くので、一覧の上の制御器が書いた値は同じステップの
// 下の制御器が読み、同じ式の中で「読んでから書く」と前回の値を読む = 状態を
// 持つ変数になる。開始・Reset で空に戻る（読むと 0）。
// パラメータへの書き込み（param:<名前> アクチュエータ）: 値が変わったときだけ
// 自分の写し（params_）へすぐ入れ（後の制御器が同じステップで読む）、ステップの
// 終わりにまとめて EditorState へ（制御盤のスライダーが動く・保存される）。
//
// 時刻の意味: センサは「前のステップの終わり」の値で、出力は「このステップ」
// に効く。プラント（Chrono）が状態を持つので、制御ループに代数ループは無い。
// 同じステップ内の計算は式の中で閉じる（式の中のフィードバックは delay で切る）。
//
// 作り直し: 式の中身（formulaSignature = 表示位置を除く）か制御器の一覧が
// 変わったときだけ。ノードを動かしただけでは PID の積分は消えない。
// シミュレートの開始・停止・Reset（Scene::runGeneration）では全部やり直す。
//
// 実行エンジン: LuaJIT（車両と同じノード式。Lua ステートはこの係が 1 つ持ち、
// 物理スレッド専用）。無いビルドは C++ のインタプリタ（結果は同じ）。
//
// 遊星歯車式 AT（文書の <control><transmission>）もここが持つ: AT ごとに
// AtDriveline（components/AtDriveline.h）が Chrono の 1D シャフト網を組み、動力学は
// Chrono が剛体と同じ系で解く。1 ステップの順番は
//   1. read  … 直前の積分の結果（角速度・各部のトルク）を trans:<名前>.<量> へ
//   2. 制御器を評価して出力を当てる（trans:<名前>.<入力> は次に書くまで保持）
//   3. apply … TCU が各クラッチの容量を決め、軸に荷重を与える
//   4. Chrono が積分（このコンポーネントの後）
// 作り直すのは transmissionVersion が変わったとき（読込・XML の適用）・開始・Reset・
// 物理の系の作り直し（PhysicsWorld::systemSerial）・つないだ剛体が作り直されたとき。
// エディタへ戻ると網を外す。
//
// HTTP スレッドからは statusJson（/scene の control）と samplesJson /
// samplesCsv（/control・/control.csv）を読む。共有するのは mutex_ の下の
// 状態表示と記録だけ（葉のロック）。
class ControlComponent : public SceneComponent {
public:
    explicit ControlComponent(VehicleComponent* vehicles);
    // パーティクルのエミッタ（emitter:<名前>.rate の書き先）。main が登録時に渡す。
    void setEffects(EffectsComponent* effects) { effects_ = effects; }
    ~ControlComponent() override;

    void onPhysicsStep(Scene& scene, double dt) override;
    void onEditorStep(Scene& scene, double dt) override;

    // ---- HTTP スレッド --------------------------------------------------------
    // 制御器ごとの状態（コンパイルの成否・警告・入出力の名前・評価回数）。
    nlohmann::json statusJson() const;
    // since より後の記録（seq はサーバー全体の通し番号）。run が変わったら
    // ブラウザは描き直す（シミュレートのやり直し）。
    std::string samplesJson(std::uint64_t since, std::size_t maxRows = 4000) const;
    // 1 つの制御器の記録を CSV で（time, 入力..., 出力...）。空 = 先頭の制御器。
    std::string samplesCsv(const std::string& formula) const;

private:
    struct Binding {
        wizengine::control::SignalRef ref;
        bool ok = false;
        int object = -1;  // オブジェクト番号（body / vehicle）
        int joint = -1;   // 文書のジョイント番号
        int emitter = -1; // エミッタの番号（emitter:<名前>）
        int audio = -1;   // オーディオソースの番号（audio:<名前>）
        int trans = -1;   // AT の番号（trans:<名前>）
        int transQty = -1;  // AT の量の番号（センサ = atSensorId、出力 = atInputId。transmission/AtSignals）
        double lastApplied = 0.0;  // joint target: 前回渡した値（同じなら渡さない）
        bool applied = false;
    };
    struct Runtime {
        wizengine::editor::ControllerDesc desc;
        std::string signature;
        std::shared_ptr<wizengine::formula::FormulaProgram> program;
        std::unique_ptr<wizengine::formula::FormulaInstance> instance;
        std::vector<std::string> inNames, outNames;
        std::vector<Binding> ins, outs;
        std::vector<double> inVals, outVals;
        std::vector<std::string> errors, warnings;
        double sinceRun = 0.0;  // 前回の評価からの経過 (s)
        bool ran = false;
        std::uint64_t execs = 0;
        int failures = 0;
    };

    void rebuild(Scene& scene);
    void compile(Scene& scene, Runtime& rt,
                 const std::vector<wizengine::formula::FormulaGraphDesc>& formulas);
    void bind(Scene& scene, Runtime& rt, Binding& b, bool sensor);
    double readSensor(Scene& scene, const Binding& b);
    void applyOutputs(Scene& scene, Runtime& rt, double dt);
    wizengine::control::BodyState bodyState(Scene& scene, int object);
    void publishStatus();
    void rebuildTransmissions(Scene& scene);
    bool transmissionsStale(Scene& scene) const;
    void applyTransmissions(Scene& scene, double dt);

    VehicleComponent* vehicles_;
    EffectsComponent* effects_ = nullptr;
    std::uint64_t emitterVersion_ = ~std::uint64_t(0);
    std::uint64_t audioVersion_ = ~std::uint64_t(0);
    // このステップに式が書いた audio:<名前>.volume / pitch（音源の番号 → 値。NaN = 書いて
    // いない）と、前のステップまでに上書きを渡した音源（書くのをやめたら消す）。
    struct AudioWrite {
        double volume = std::numeric_limits<double>::quiet_NaN();
        double pitch = std::numeric_limits<double>::quiet_NaN();
    };
    std::map<int, AudioWrite> audioWrites_;
    std::set<int> audioOverridden_;
    void flushAudioWrites(Scene& scene);
    std::unique_ptr<wizengine::formula::LuaRuntime> lua_;

    // ---- PHYSICS スレッド専用 ----
    struct TransRuntime {
        std::string name;
        std::unique_ptr<AtDriveline> driveline;  // 制御器の入力（inputs）もここが持つ
        bool running = false;                    // Chrono の網ができている
        std::string error;                       // 作れなかった理由（英語）
        int bodyObject = -1;                     // input = body のつなぎ先（オブジェクト番号）
        std::size_t bodyPhys = static_cast<std::size_t>(-1);
    };
    std::vector<TransRuntime> trans_;
    std::uint64_t transVersion_ = ~std::uint64_t(0);
    std::uint64_t transSerial_ = ~std::uint64_t(0);  // 網を作ったときの PhysicsWorld::systemSerial
    std::vector<Runtime> runtimes_;
    std::uint64_t generation_ = ~std::uint64_t(0);
    std::uint64_t controlVersion_ = ~std::uint64_t(0);
    std::uint64_t formulaVersion_ = ~std::uint64_t(0);
    // パラメータの値の写し（param:<名前> センサ。paramVersion が変わったときだけ取る）。
    std::uint64_t paramVersion_ = ~std::uint64_t(0);
    std::map<std::string, double> params_;
    std::map<std::string, double> paramWrites_;  // このステップに式が書いた param:
    std::map<std::string, double> vars_;         // 変数（var:<名前>）の表
    double time_ = 0.0;  // シミュレート開始からの時間（time センサ）
    // ジョイントの相対運動の基準（文書のジョイント番号 → 開始時の姿勢）。
    std::map<int, wizengine::control::JointMeasure> joints_;
    std::vector<std::pair<int, int>> jointEnds_;  // 文書のジョイント番号 → (bodyA, bodyB)
    // このステップで測った結果（同じジョイントを 2 度測らない = 角度の連続化が
    // 1 ステップに 1 回だけ進む）。
    std::map<int, wizengine::control::JointMeasure::Result> jointResults_;
    std::chrono::steady_clock::time_point lastStatus_{};
    bool jointStatsFetched_ = false;
    // 空気（air:<量>）: 読むのはステップの始めに 1 回取った状態、書いた値はステップの
    // 終わりに Scene::setAirOverride へまとめて渡す（毎ステップ作り直す = 書く制御器が
    // 無くなれば文書の値へ戻る）。
    bool airFetched_ = false;
    wizengine::fluid::AirState airState_;
    wizengine::fluid::AirOverride airWrites_;
    bool airSent_ = false;  // 前に空でない上書きを渡した（消すときに 1 回だけ渡し直す）
    std::vector<double> jointForce_, jointTorque_;

    // ---- 共有（mutex_ の下）----
    mutable std::mutex mutex_;
    nlohmann::json status_ = nlohmann::json::object();
    struct Series {
        std::string formula;
        std::vector<std::string> names;  // 入力名..., 出力名...
        std::deque<std::pair<std::uint64_t, std::vector<double>>> rows;  // (seq, [t, 値...])
    };
    std::vector<Series> series_;
    std::uint64_t seq_ = 0;
    std::uint64_t run_ = 0;  // 記録の世代（やり直すたびに進む）
};
