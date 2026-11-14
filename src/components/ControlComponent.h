#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlMath.h"
#include "control/ControlSignals.h"
#include "document/EditorTypes.h"
#include "scene/SceneComponent.h"
#include "formula/Formula.h"
#include "formula/LuaFormula.h"

class VehicleComponent;

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
// HTTP スレッドからは statusJson（/scene の control）と samplesJson /
// samplesCsv（/control・/control.csv）を読む。共有するのは mutex_ の下の
// 状態表示と記録だけ（葉のロック）。
class ControlComponent : public SceneComponent {
public:
    explicit ControlComponent(VehicleComponent* vehicles);
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

    VehicleComponent* vehicles_;
    std::unique_ptr<wizengine::formula::LuaRuntime> lua_;

    // ---- PHYSICS スレッド専用 ----
    std::vector<Runtime> runtimes_;
    std::uint64_t generation_ = ~std::uint64_t(0);
    std::uint64_t controlVersion_ = ~std::uint64_t(0);
    std::uint64_t formulaVersion_ = ~std::uint64_t(0);
    // パラメータの値の写し（param:<名前> センサ。paramVersion が変わったときだけ取る）。
    std::uint64_t paramVersion_ = ~std::uint64_t(0);
    std::map<std::string, double> params_;
    double time_ = 0.0;  // シミュレート開始からの時間（time センサ）
    // ジョイントの相対運動の基準（文書のジョイント番号 → 開始時の姿勢）。
    std::map<int, wizengine::control::JointMeasure> joints_;
    std::vector<std::pair<int, int>> jointEnds_;  // 文書のジョイント番号 → (bodyA, bodyB)
    // このステップで測った結果（同じジョイントを 2 度測らない = 角度の連続化が
    // 1 ステップに 1 回だけ進む）。
    std::map<int, wizengine::control::JointMeasure::Result> jointResults_;
    int statusTick_ = 0;
    bool jointStatsFetched_ = false;
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
