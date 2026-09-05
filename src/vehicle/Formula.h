#pragma once

#include <memory>
#include <string>
#include <vector>

// ノード式（研究者が数式を組み替えるための、スカラーのデータフローグラフ）。
//
// 「ノードを Lua に変換して LuaJIT で回す」の中核で、ここには Lua も Chrono も
// 出てこない。持っているのは:
//   FormulaGraphDesc … 文書の値（ノード + ワイヤー）。<vehicle> の <formula>
//   FormulaProgram   … 検証と整列を済ませた線形の命令列。入力・出力は名前で
//                      結び（"kappa" → I[3] のように）、状態を持つノード
//                      （lag / integrate）には状態スロットを割り当てる
//   FormulaInstance  … 1 実体（車輪 1 本ぶん）の実行口。参照インタプリタ
//                      （C++）と LuaJIT（LuaFormula.h）の 2 通りがあり、
//                      同じ命令列から作るので結果は一致する（テストで確認）
//
// ---- ノードの語彙 ------------------------------------------------------------
// 全部スカラー。ポートは入力 0..2、出力 0..1 の番号で、ワイヤーは
// (from, fromPort) → (to, port)。未接続の入力は 0（警告）。
//   in name=…        … 入力（名前は使う側が決める。タイヤなら kappa, alpha…）
//   out name=…       … 出力
//   const params=v   … 定数
//   dt               … ティックの dt
//   add sub mul div min max pow atan2 gt lt   … 2 入力
//   neg abs sqrt sin cos tan atan exp log sign … 1 入力
//   clamp(x, lo, hi)  lerp(a, b, t)  select(cond, a, b)
//   curve params="x0 y0 x1 y1 …"   … 折れ線（x 昇順）
//   lag(x, tau)       … 一次遅れ y += (x - y) * min(1, dt / tau)。状態 1
//   integrate(x) params="lo hi"（省略可）… y += x dt。状態 1
//   magic(s, B, C, E) … Pacejka: sin(C atan(B s - E (B s - atan(B s))))
//   ellipse(fx, fy, peak) → (fx', fy') … 合力を peak に収める。出力 2
namespace wizengine {
namespace vehicle {

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
};

// ノード語彙の定義（名前・入出力の数・状態の有無）。未知の名前は nullptr。
struct FormulaKindInfo {
    const char* name;
    int inputs;
    int outputs;
    int states;
};
const FormulaKindInfo* formulaKind(const std::string& kind);
// 語彙の一覧（UI の追加メニュー用）。
const std::vector<FormulaKindInfo>& formulaKinds();

// ---- 命令列 ------------------------------------------------------------------
enum class FormulaOp {
    In, Out, Const, Dt,
    Add, Sub, Mul, Div, Min, Max, Pow, Atan2, Gt, Lt,
    Neg, Abs, Sqrt, Sin, Cos, Tan, Atan, Exp, Log, Sign,
    Clamp, Lerp, Select, Curve, Lag, Integrate, Magic, Ellipse,
};

struct FormulaInstr {
    FormulaOp op = FormulaOp::Const;
    int nodeId = 0;
    int in[3] = {-1, -1, -1};   // 値スロット（-1 = 未接続 → 0）
    int out[2] = {-1, -1};      // 値スロット
    int index = -1;             // In / Out: 入出力の番号。Lag / Integrate: 状態番号
    int paramBegin = 0;         // params_ の範囲（Const / Curve / Integrate）
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

// 折れ線の補間（curve ノードと同じ定義。Lua 側も同じ式で書く）。
double formulaCurve(const double* xy, int count, double x);

}  // namespace vehicle
}  // namespace wizengine
