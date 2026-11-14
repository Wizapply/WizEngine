#pragma once

#include <memory>
#include <string>
#include <vector>

// ノード式（計算式アセット。研究者が数式をノードで組む、スカラーの
// データフローグラフ）。特定の用途（車両など）には依存しない汎用の部品で、
// いまの使い手は制御器（control/ + ControlComponent）。
//
// 「ノードを Lua に変換して LuaJIT で回す」の中核で、ここには Lua も Chrono も
// 出てこない。持っているのは:
//   FormulaGraphDesc … 文書の値（ノード + ワイヤー）。<asset> の <formula>
//   FormulaProgram   … 検証と整列を済ませた線形の命令列。入力・出力は名前で
//                      結び（"joint:arm.angle" → I[0] のように）、状態を持つ
//                      ノードには状態スロットを割り当てる
//   FormulaInstance  … 1 実体の実行口。参照インタプリタ（C++）と LuaJIT
//                      （LuaFormula.h）の 2 通りがあり、同じ命令列から作るので
//                      結果は一致する（テストで確認）
// 数式のテキスト（"u = pid(ref - y, 2, 1, 0)"）との相互変換は FormulaText.h。
//
// ---- ノードの語彙 ------------------------------------------------------------
// 全部スカラー。ポートは入力 0..2、出力 0..1 の番号で、ワイヤーは
// (from, fromPort) → (to, port)。未接続の入力は 0（警告）。
//   in name=…        … 入力（名前は使う側が決める。制御器なら信号名）
//   out name=…       … 出力
//   const params=v   … 定数
//   dt               … ティックの dt
//   add sub mul div min max pow atan2 gt lt   … 2 入力
//   neg abs sqrt sin cos tan atan exp log sign … 1 入力
//   clamp(x, lo, hi)  lerp(a, b, t)  select(cond, a, b)
//   curve params="x0 y0 x1 y1 …"   … 折れ線（x 昇順）
//   lag(x, tau)       … 一次遅れ y += (x - y) * min(1, dt / tau)。状態 1
//   integrate(x) params="lo hi"（省略可）… 積分（前進オイラー: 出力はいまの
//                     状態、x dt の取り込みは命令列の最後）。**直達しない**ので
//                     delay と同じくフィードバックの輪を切れる（Simulink の
//                     積分器と同じ = 連続系 x' = f(x) を輪のまま組める）。状態 1
//
// ---- 制御ブロック（Simulink の基本ブロック相当。control/ が使う）-------------
// 係数は params（空白区切り、省略した値は既定）。状態は reset で 0 に戻る。
//   time               … 実行開始からの経過時間（最初の評価で 0）。状態 1
//   gain(x) params="k"                 … k x（既定 1）
//   saturate(x) params="lo hi"         … 範囲に収める（既定 -1 1）
//   deadzone(x) params="lo hi"         … lo..hi は 0、外は境界からの差（既定 -0.1 0.1）
//   delay(x) params="init"             … 単位遅延 z^-1。出力は前回の入力
//                                         （最初は init）。**直達しない**ので
//                                         フィードバックの輪はここで切れる
//                                         （循環の検査は delay の入力側を辿らない）
//   derivative(x)                      … (x - 前回の x) / dt（最初は 0）。状態 2
//   ratelimit(x) params="rise fall"    … 変化率を 1 秒あたり rise / fall に制限
//                                         （0 以下 = その向きは制限なし）。状態 2
//   pid(e) params="kp ki kd lo hi tf"  … 並列形 PID（既定 1 0 0 0 0 0）。
//                                         u = kp e + ∫ki e dt + kd de/dt。
//                                         lo < hi なら出力を制限し、飽和中は
//                                         飽和を深める向きの積分を止める
//                                         （アンチワインドアップ）。tf > 0 なら
//                                         微分に時定数 tf の一次フィルタ。状態 4
namespace wizengine {
namespace formula {

struct FormulaNodeDesc {
    int id = 0;
    std::string kind;
    std::string name;            // in / out の名前
    std::vector<double> params;  // const の値、curve の点列、integrate の範囲
    double x = 0.0, y = 0.0;     // ノードエディタ上の位置（実行には無関係）
};

struct FormulaWireDesc {
    int from = 0;
    int fromPort = 0;
    int to = 0;
    int port = 0;
};

struct FormulaGraphDesc {
    std::string name;
    std::vector<FormulaNodeDesc> nodes;
    std::vector<FormulaWireDesc> wires;
    // 数式のテキストから起こしたときの元のテキスト（FormulaText.h）。コメントや
    // 名前付きの定数を保つためだけに持つ（実行はノードとワイヤー）。ノード /
    // ワイヤーを編集したら空にする（グラフとずれたテキストを見せないため。
    // 位置だけの移動は保つ）。空なら式エディタは formulaToText で起こし直す。
    std::string source;
};

// ノード語彙の定義（名前・入出力の数・状態の有無）。未知の名前は nullptr。
struct FormulaKindInfo {
    const char* name;
    int inputs;
    int outputs;
    int states;
};
const FormulaKindInfo* formulaKind(const std::string& kind);
// 入力が出力へ直達しないノード（delay / integrate）。ワイヤーの循環検査と整列は、
// このノードへ入る線を「同じティックの依存」として数えない。
bool formulaBreaksFeedback(const std::string& kind);
// 語彙の一覧（UI の追加メニュー用）。
const std::vector<FormulaKindInfo>& formulaKinds();

// ---- 命令列 ------------------------------------------------------------------
enum class FormulaOp {
    In, Out, Const, Dt,
    Add, Sub, Mul, Div, Min, Max, Pow, Atan2, Gt, Lt,
    Neg, Abs, Sqrt, Sin, Cos, Tan, Atan, Exp, Log, Sign,
    Clamp, Lerp, Select, Curve, Lag, Integrate,
    // 制御ブロック
    Time, Gain, Saturate, Deadzone, Delay, Derivative, RateLimit, Pid,
    // delay / integrate の取り込み（命令列の最後。in[0] = 入力、index = 状態の先頭）
    DelayStore, IntegrateStore,
};

struct FormulaInstr {
    FormulaOp op = FormulaOp::Const;
    int nodeId = 0;
    int in[3] = {-1, -1, -1};   // 値スロット（-1 = 未接続 → 0）
    int out[2] = {-1, -1};      // 値スロット
    int index = -1;             // In / Out: 入出力の番号。状態を持つノード: 状態の先頭
    int paramBegin = 0;         // params_ の範囲（Const / Curve / Integrate / 制御ブロック）
    int paramCount = 0;
};

class FormulaInstance {
public:
    virtual ~FormulaInstance() = default;
    // in は inputs 個、out は outputs 個。失敗（実行時エラー）で false。
    virtual bool eval(const double* in, double* out, double dt) = 0;
    virtual void reset() = 0;
    virtual const char* backend() const = 0;
};

// 実体（インタプリタ / LuaJIT）はプログラムを shared_ptr で持つ。必ず
// compile() で作る（make_shared 経由 = shared_from_this が使える）。
class FormulaProgram : public std::enable_shared_from_this<FormulaProgram> {
public:
    // 検証と整列。errors が空でなければ実行できない（循環・未知のノード）。
    // warnings は「動くけれど意図と違うかもしれない」内容（未接続・未知の
    // 入出力名）。inputs / outputs は使う側が決める名前の並び。
    static std::shared_ptr<FormulaProgram> compile(
        const FormulaGraphDesc& graph, const std::vector<std::string>& inputs,
        const std::vector<std::string>& outputs, std::vector<std::string>& errors,
        std::vector<std::string>& warnings);

    const std::string& name() const { return name_; }
    const std::vector<FormulaInstr>& instructions() const { return instrs_; }
    const std::vector<double>& params() const { return params_; }
    int inputCount() const { return inputs_; }
    int outputCount() const { return outputs_; }
    int slotCount() const { return slots_; }
    int stateCount() const { return states_; }

    // C++ の参照インタプリタで実体を作る。
    std::unique_ptr<FormulaInstance> interpret() const;
    // Lua ソース（チャンクは function(I, O, S, dt) を返す）。LuaFormula が使う。
    std::string luaSource() const;

private:
    std::string name_;
    std::vector<FormulaInstr> instrs_;
    std::vector<double> params_;
    int inputs_ = 0;
    int outputs_ = 0;
    int slots_ = 0;
    int states_ = 0;
};

// 実行に効く中身（ノードの種類・名前・係数とワイヤー）の要約。ノードの
// 表示位置は含まない = ノードエディタでノードを動かしただけでは変わらない。
// 実行側（車両・制御器）が「作り直しが要るか」をこれで判定する（作り直すと
// 状態 = PID の積分や車の走行状態がリセットされるため）。
std::string formulaSignature(const FormulaGraphDesc& graph);

// 折れ線の補間（curve ノードと同じ定義。Lua 側も同じ式で書く）。
double formulaCurve(const double* xy, int count, double x);

}  // namespace formula
}  // namespace wizengine
