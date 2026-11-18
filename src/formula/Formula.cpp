#include "formula/Formula.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace wizengine {
namespace formula {

namespace {

// 語彙表。順番は UI のメニュー順でもある。
const std::vector<FormulaKindInfo>& kindTable() {
    static const std::vector<FormulaKindInfo> table = {
        {"in", 0, 1, 0},       {"out", 1, 0, 0},      {"const", 0, 1, 0},
        {"dt", 0, 1, 0},       {"add", 2, 1, 0},      {"sub", 2, 1, 0},
        {"mul", 2, 1, 0},      {"div", 2, 1, 0},      {"min", 2, 1, 0},
        {"max", 2, 1, 0},      {"pow", 2, 1, 0},      {"atan2", 2, 1, 0},
        {"gt", 2, 1, 0},       {"lt", 2, 1, 0},       {"neg", 1, 1, 0},
        {"abs", 1, 1, 0},      {"sqrt", 1, 1, 0},     {"sin", 1, 1, 0},
        {"cos", 1, 1, 0},      {"tan", 1, 1, 0},      {"atan", 1, 1, 0},
        {"exp", 1, 1, 0},      {"log", 1, 1, 0},      {"sign", 1, 1, 0},
        {"clamp", 3, 1, 0},    {"lerp", 3, 1, 0},     {"select", 3, 1, 0},
        {"curve", 1, 1, 0},    {"lag", 2, 1, 1},      {"integrate", 1, 1, 1},
        // 制御ブロック（Formula.h の「制御ブロック」）
        {"time", 0, 1, 1},     {"gain", 1, 1, 0},     {"saturate", 1, 1, 0},
        {"deadzone", 1, 1, 0}, {"delay", 1, 1, 2},    {"derivative", 1, 1, 2},
        {"ratelimit", 1, 1, 2}, {"pid", 1, 1, 4},
    };
    return table;
}

FormulaOp opFor(const std::string& kind) {
    static const std::map<std::string, FormulaOp> ops = {
        {"in", FormulaOp::In},         {"out", FormulaOp::Out},
        {"const", FormulaOp::Const},   {"dt", FormulaOp::Dt},
        {"add", FormulaOp::Add},       {"sub", FormulaOp::Sub},
        {"mul", FormulaOp::Mul},       {"div", FormulaOp::Div},
        {"min", FormulaOp::Min},       {"max", FormulaOp::Max},
        {"pow", FormulaOp::Pow},       {"atan2", FormulaOp::Atan2},
        {"gt", FormulaOp::Gt},         {"lt", FormulaOp::Lt},
        {"neg", FormulaOp::Neg},       {"abs", FormulaOp::Abs},
        {"sqrt", FormulaOp::Sqrt},     {"sin", FormulaOp::Sin},
        {"cos", FormulaOp::Cos},       {"tan", FormulaOp::Tan},
        {"atan", FormulaOp::Atan},     {"exp", FormulaOp::Exp},
        {"log", FormulaOp::Log},       {"sign", FormulaOp::Sign},
        {"clamp", FormulaOp::Clamp},   {"lerp", FormulaOp::Lerp},
        {"select", FormulaOp::Select}, {"curve", FormulaOp::Curve},
        {"lag", FormulaOp::Lag},       {"integrate", FormulaOp::Integrate},
        {"time", FormulaOp::Time},     {"gain", FormulaOp::Gain},
        {"saturate", FormulaOp::Saturate}, {"deadzone", FormulaOp::Deadzone},
        {"delay", FormulaOp::Delay},   {"derivative", FormulaOp::Derivative},
        {"ratelimit", FormulaOp::RateLimit}, {"pid", FormulaOp::Pid},
    };
    return ops.at(kind);
}

// 制御ブロックの係数の既定値（params の先頭から上書きする）。係数を持たない
// 種類は空。並びは Formula.h の説明と同じ。
const std::vector<double>* paramDefaults(FormulaOp op) {
    static const std::vector<double> gain = {1.0};
    static const std::vector<double> saturate = {-1.0, 1.0};
    static const std::vector<double> deadzone = {-0.1, 0.1};
    static const std::vector<double> delay = {0.0};
    static const std::vector<double> ratelimit = {1.0, 1.0};
    static const std::vector<double> pid = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    switch (op) {
        case FormulaOp::Gain: return &gain;
        case FormulaOp::Saturate: return &saturate;
        case FormulaOp::Deadzone: return &deadzone;
        case FormulaOp::Delay: return &delay;
        case FormulaOp::RateLimit: return &ratelimit;
        case FormulaOp::Pid: return &pid;
        default: return nullptr;
    }
}

// ---- 参照インタプリタ --------------------------------------------------------
class Interpreter : public FormulaInstance {
public:
    // プログラムは共有所有。参照で持つと、コンパイルした側のローカルが消えた
    // あとに実行して落ちる（VehicleModel の生成で実際に起きた）。
    explicit Interpreter(std::shared_ptr<const FormulaProgram> p)
        : owner_(std::move(p)), program_(*owner_) {
        slots_.assign(std::size_t(std::max(program_.slotCount(), 1)), 0.0);
        state_.assign(std::size_t(std::max(program_.stateCount(), 1)), 0.0);
    }
    void reset() override { std::fill(state_.begin(), state_.end(), 0.0); }
    const char* backend() const override { return "interpreter"; }

    bool eval(const double* in, double* out, double dt) override {
        const auto& params = program_.params();
        for (int i = 0; i < program_.outputCount(); ++i) out[i] = 0.0;
        auto v = [&](int slot) { return slot >= 0 ? slots_[std::size_t(slot)] : 0.0; };
        for (const FormulaInstr& k : program_.instructions()) {
            double r = 0.0;
            const double a = v(k.in[0]), b = v(k.in[1]), c = v(k.in[2]);
            switch (k.op) {
                case FormulaOp::In: r = in[k.index]; break;
                case FormulaOp::Out: out[k.index] = a; continue;
                case FormulaOp::Const: r = params[std::size_t(k.paramBegin)]; break;
                case FormulaOp::Dt: r = dt; break;
                case FormulaOp::Add: r = a + b; break;
                case FormulaOp::Sub: r = a - b; break;
                case FormulaOp::Mul: r = a * b; break;
                case FormulaOp::Div: r = a / b; break;
                case FormulaOp::Min: r = std::min(a, b); break;
                case FormulaOp::Max: r = std::max(a, b); break;
                case FormulaOp::Pow: r = std::pow(a, b); break;
                case FormulaOp::Atan2: r = std::atan2(a, b); break;
                case FormulaOp::Gt: r = a > b ? 1.0 : 0.0; break;
                case FormulaOp::Lt: r = a < b ? 1.0 : 0.0; break;
                case FormulaOp::Neg: r = -a; break;
                case FormulaOp::Abs: r = std::fabs(a); break;
                case FormulaOp::Sqrt: r = std::sqrt(a); break;
                case FormulaOp::Sin: r = std::sin(a); break;
                case FormulaOp::Cos: r = std::cos(a); break;
                case FormulaOp::Tan: r = std::tan(a); break;
                case FormulaOp::Atan: r = std::atan(a); break;
                case FormulaOp::Exp: r = std::exp(a); break;
                case FormulaOp::Log: r = std::log(a); break;
                case FormulaOp::Sign: r = a < 0.0 ? -1.0 : (a > 0.0 ? 1.0 : 0.0); break;
                case FormulaOp::Clamp: r = a < b ? b : (a > c ? c : a); break;
                case FormulaOp::Lerp: r = a + (b - a) * c; break;
                case FormulaOp::Select: r = a > 0.5 ? b : c; break;
                case FormulaOp::Curve:
                    r = formulaCurve(params.data() + k.paramBegin, k.paramCount / 2, a);
                    break;
                case FormulaOp::Lag: {
                    double& y = state_[std::size_t(k.index)];
                    const double alpha = b > 0.0 ? std::min(1.0, dt / b) : 1.0;
                    y += (a - y) * alpha;
                    r = y;
                    break;
                }
                // 積分は直達しない（出力 = いまの状態、取り込みは最後の
                // IntegrateStore）= 前進オイラー。輪を切れる（delay と同じ）。
                case FormulaOp::Integrate: r = state_[std::size_t(k.index)]; break;
                case FormulaOp::IntegrateStore: {
                    double& y = state_[std::size_t(k.index)];
                    y += a * dt;
                    if (k.paramCount >= 2) {
                        const double lo = params[std::size_t(k.paramBegin)];
                        const double hi = params[std::size_t(k.paramBegin) + 1];
                        y = y < lo ? lo : (y > hi ? hi : y);
                    }
                    continue;
                }
                // ---- 制御ブロック。式の並びは luaSource と 1 対 1（結果を
                // ビット単位で揃えるため、演算の順序も変えない）----
                case FormulaOp::Time: {
                    double& t = state_[std::size_t(k.index)];
                    r = t;
                    t = r + dt;
                    break;
                }
                case FormulaOp::Gain: r = params[std::size_t(k.paramBegin)] * a; break;
                case FormulaOp::Saturate: {
                    const double lo = params[std::size_t(k.paramBegin)];
                    const double hi = params[std::size_t(k.paramBegin) + 1];
                    r = a < lo ? lo : (a > hi ? hi : a);
                    break;
                }
                case FormulaOp::Deadzone: {
                    const double lo = params[std::size_t(k.paramBegin)];
                    const double hi = params[std::size_t(k.paramBegin) + 1];
                    r = a < lo ? a - lo : (a > hi ? a - hi : 0.0);
                    break;
                }
                case FormulaOp::Delay: {
                    const double* s = &state_[std::size_t(k.index)];
                    r = s[1] > 0.5 ? s[0] : params[std::size_t(k.paramBegin)];
                    break;
                }
                case FormulaOp::DelayStore: {
                    double* s = &state_[std::size_t(k.index)];
                    s[0] = a;
                    s[1] = 1.0;
                    continue;
                }
                case FormulaOp::Derivative: {
                    double* s = &state_[std::size_t(k.index)];
                    r = 0.0;
                    if (s[1] > 0.5 && dt > 0.0) r = (a - s[0]) / dt;
                    s[0] = a;
                    s[1] = 1.0;
                    break;
                }
                case FormulaOp::RateLimit: {
                    const double rise = params[std::size_t(k.paramBegin)];
                    const double fall = params[std::size_t(k.paramBegin) + 1];
                    double* s = &state_[std::size_t(k.index)];
                    double y = a;
                    if (s[1] > 0.5) {
                        const double d = a - s[0];
                        if (rise > 0.0 && d > rise * dt) y = s[0] + rise * dt;
                        else if (fall > 0.0 && d < -(fall * dt)) y = s[0] - fall * dt;
                    }
                    s[0] = y;
                    s[1] = 1.0;
                    r = y;
                    break;
                }
                case FormulaOp::Pid: {
                    const double* P = &params[std::size_t(k.paramBegin)];  // kp ki kd lo hi tf
                    double* s = &state_[std::size_t(k.index)];             // I e_prev d flag
                    const double e = a;
                    double draw = 0.0;
                    if (s[3] > 0.5 && dt > 0.0) draw = (e - s[1]) / dt;
                    double d = draw;
                    if (P[5] > 0.0) d = s[2] + (draw - s[2]) * (dt / (P[5] + dt));
                    double integ = s[0] + P[1] * e * dt;
                    double u = P[0] * e + integ + P[2] * d;
                    if (P[3] < P[4]) {
                        if (u > P[4]) {
                            u = P[4];
                            if (P[1] * e > 0.0) integ = s[0];
                        } else if (u < P[3]) {
                            u = P[3];
                            if (P[1] * e < 0.0) integ = s[0];
                        }
                    }
                    s[0] = integ;
                    s[1] = e;
                    s[2] = d;
                    s[3] = 1.0;
                    r = u;
                    break;
                }
            }
            if (k.out[0] >= 0) slots_[std::size_t(k.out[0])] = r;
        }
        for (int i = 0; i < program_.outputCount(); ++i) {
            if (!std::isfinite(out[i])) return false;
        }
        return true;
    }

private:
    std::shared_ptr<const FormulaProgram> owner_;
    const FormulaProgram& program_;
    std::vector<double> slots_;
    std::vector<double> state_;
};

std::string fmtDouble(double v) {
    char buf[64];
    if (std::isnan(v)) return "(0/0)";
    if (std::isinf(v)) return v > 0 ? "math.huge" : "(-math.huge)";
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string s(buf);
    // "1e+20" のような指数表記は Lua でもそのまま読める。整数に見える値には
    // ".0" を付けて実数であることを明示する（LuaJIT の型推論の揺れを避ける）。
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

}  // namespace

const FormulaKindInfo* formulaKind(const std::string& kind) {
    for (const FormulaKindInfo& k : kindTable()) {
        if (kind == k.name) return &k;
    }
    return nullptr;
}

const std::vector<FormulaKindInfo>& formulaKinds() { return kindTable(); }

bool formulaBreaksFeedback(const std::string& kind) {
    return kind == "delay" || kind == "integrate";
}

std::string formulaSignature(const FormulaGraphDesc& graph) {
    std::ostringstream o;
    o << graph.name << '|';
    for (const FormulaNodeDesc& n : graph.nodes) {
        o << n.id << ':' << n.kind << ':' << n.name << ':';
        for (double p : n.params) o << fmtDouble(p) << ',';
        o << ';';
    }
    o << '|';
    for (const FormulaWireDesc& w : graph.wires) {
        o << w.from << '.' << w.fromPort << '>' << w.to << '.' << w.port << ';';
    }
    return o.str();
}

double formulaCurve(const double* xy, int count, double x) {
    if (count <= 0) return 0.0;
    if (x <= xy[0]) return xy[1];
    if (x >= xy[(count - 1) * 2]) return xy[(count - 1) * 2 + 1];
    for (int i = 1; i < count; ++i) {
        const double x0 = xy[(i - 1) * 2], y0 = xy[(i - 1) * 2 + 1];
        const double x1 = xy[i * 2], y1 = xy[i * 2 + 1];
        if (x <= x1) {
            const double span = x1 - x0;
            const double t = span > 1e-12 ? (x - x0) / span : 0.0;
            return y0 + (y1 - y0) * t;
        }
    }
    return xy[(count - 1) * 2 + 1];
}

std::shared_ptr<FormulaProgram> FormulaProgram::compile(
    const FormulaGraphDesc& graph, const std::vector<std::string>& inputs,
    const std::vector<std::string>& outputs, std::vector<std::string>& errors,
    std::vector<std::string>& warnings) {
    auto prog = std::make_shared<FormulaProgram>();
    prog->name_ = graph.name;
    prog->inputs_ = int(inputs.size());
    prog->outputs_ = int(outputs.size());
    const std::string label = "formula \"" + graph.name + "\"";

    // ---- ノードの検査 ----
    std::map<int, const FormulaNodeDesc*> byId;
    for (const FormulaNodeDesc& n : graph.nodes) {
        if (byId.count(n.id)) {
            errors.push_back(label + ": node id " + std::to_string(n.id) + " is used twice");
            continue;
        }
        if (!formulaKind(n.kind)) {
            errors.push_back(label + ": node " + std::to_string(n.id) + " has unknown type \"" +
                             n.kind + "\"");
            continue;
        }
        byId[n.id] = &n;
    }
    if (!errors.empty()) return nullptr;

    // ---- 入力ポートの接続表: (to, port) → (from, fromPort) ----
    std::map<std::pair<int, int>, std::pair<int, int>> conn;
    for (const FormulaWireDesc& w : graph.wires) {
        const auto from = byId.find(w.from);
        const auto to = byId.find(w.to);
        if (from == byId.end() || to == byId.end()) {
            warnings.push_back(label + ": wire " + std::to_string(w.from) + " -> " +
                               std::to_string(w.to) + " refers to a missing node - ignored");
            continue;
        }
        const FormulaKindInfo* fk = formulaKind(from->second->kind);
        const FormulaKindInfo* tk = formulaKind(to->second->kind);
        if (w.fromPort < 0 || w.fromPort >= fk->outputs || w.port < 0 || w.port >= tk->inputs) {
            warnings.push_back(label + ": wire " + std::to_string(w.from) + " -> " +
                               std::to_string(w.to) + " uses a port that does not exist - ignored");
            continue;
        }
        const auto key = std::make_pair(w.to, w.port);
        if (conn.count(key)) {
            warnings.push_back(label + ": input " + std::to_string(w.port) + " of node " +
                               std::to_string(w.to) + " has two wires - using the first");
            continue;
        }
        conn[key] = std::make_pair(w.from, w.fromPort);
    }

    // ---- 整列（Kahn 法）。残ったノードは循環 ----
    std::map<int, int> indeg;
    std::map<int, std::vector<int>> succ;
    for (const auto& kv : byId) indeg[kv.first] = 0;
    for (const auto& kv : conn) {
        // delay の入力は「次のティック」の依存なので並びを縛らない（取り込みは
        // 命令列の最後の DelayStore）。これでフィードバックの輪を組める。
        if (formulaBreaksFeedback(byId[kv.first.first]->kind)) continue;
        indeg[kv.first.first] += 1;
        succ[kv.second.first].push_back(kv.first.first);
    }
    std::vector<int> order;
    std::vector<int> ready;
    for (const auto& kv : indeg) {
        if (kv.second == 0) ready.push_back(kv.first);
    }
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end());  // 決定的な順序
        const int id = ready.front();
        ready.erase(ready.begin());
        order.push_back(id);
        for (int s : succ[id]) {
            if (--indeg[s] == 0) ready.push_back(s);
        }
    }
    if (order.size() != byId.size()) {
        std::string cyc;
        for (const auto& kv : indeg) {
            if (kv.second > 0) cyc += (cyc.empty() ? "" : ", ") + std::to_string(kv.first);
        }
        errors.push_back(label + ": the graph has a cycle through nodes " + cyc +
                         " (put a delay or integrate node in the loop to break feedback)");
        return nullptr;
    }

    // ---- 命令列 ----
    std::map<std::pair<int, int>, int> slotOf;  // (node, outPort) → スロット
    int slots = 0, states = 0;
    std::set<std::string> outputsSeen;
    std::vector<FormulaInstr> delays;  // 取り込みを最後に回すノード（delay / integrate）
    for (int id : order) {
        const FormulaNodeDesc& n = *byId[id];
        const FormulaKindInfo* info = formulaKind(n.kind);
        FormulaInstr k;
        k.op = opFor(n.kind);
        k.nodeId = id;
        auto inputSlot = [&](int port) -> int {
            const auto c = conn.find(std::make_pair(id, port));
            if (c == conn.end()) {
                warnings.push_back(label + ": input " + std::to_string(port) + " of node " +
                                   std::to_string(id) + " (" + n.kind + ") is not connected - using 0");
                return -1;
            }
            return slotOf[c->second];
        };
        // delay / integrate の入力は最後（DelayStore / IntegrateStore）で解決
        // する: 入力元がまだ並んでいない（輪の中にいる）ことがあるため。
        if (!formulaBreaksFeedback(n.kind)) {
            for (int p = 0; p < std::min(info->inputs, 3); ++p) k.in[p] = inputSlot(p);
        }
        for (int p = 0; p < info->outputs; ++p) {
            k.out[p] = slots++;
            slotOf[std::make_pair(id, p)] = k.out[p];
        }
        if (k.op == FormulaOp::In) {
            const auto it = std::find(inputs.begin(), inputs.end(), n.name);
            if (it == inputs.end()) {
                warnings.push_back(label + ": input \"" + n.name + "\" is not provided by the "
                                   "caller - using 0");
                k.op = FormulaOp::Const;
                k.paramBegin = int(prog->params_.size());
                k.paramCount = 1;
                prog->params_.push_back(0.0);
            } else {
                k.index = int(it - inputs.begin());
            }
        } else if (k.op == FormulaOp::Out) {
            const auto it = std::find(outputs.begin(), outputs.end(), n.name);
            if (it == outputs.end()) {
                warnings.push_back(label + ": output \"" + n.name + "\" is not used by the "
                                   "caller - ignored");
                continue;
            }
            if (!outputsSeen.insert(n.name).second) {
                warnings.push_back(label + ": output \"" + n.name + "\" is written twice - "
                                   "using the first");
                continue;
            }
            k.index = int(it - outputs.begin());
        } else if (k.op == FormulaOp::Const) {
            k.paramBegin = int(prog->params_.size());
            k.paramCount = 1;
            prog->params_.push_back(n.params.empty() ? 0.0 : n.params[0]);
        } else if (k.op == FormulaOp::Curve) {
            const int count = int(n.params.size() / 2);
            if (count < 1) {
                warnings.push_back(label + ": curve node " + std::to_string(id) +
                                   " has no points - outputs 0");
            }
            bool sorted = true;
            for (int i = 1; i < count; ++i) {
                if (n.params[std::size_t(i) * 2] < n.params[std::size_t(i - 1) * 2]) sorted = false;
            }
            if (!sorted) {
                warnings.push_back(label + ": curve node " + std::to_string(id) +
                                   " points are not sorted by x - results are undefined");
            }
            k.paramBegin = int(prog->params_.size());
            k.paramCount = count * 2;
            prog->params_.insert(prog->params_.end(), n.params.begin(),
                                 n.params.begin() + count * 2);
        } else if (k.op == FormulaOp::Integrate) {
            k.index = states;
            states += info->states;
            if (n.params.size() >= 2) {
                k.paramBegin = int(prog->params_.size());
                k.paramCount = 2;
                prog->params_.push_back(std::min(n.params[0], n.params[1]));
                prog->params_.push_back(std::max(n.params[0], n.params[1]));
            }
        } else if (info->states > 0) {
            // lag と制御ブロック: 状態の先頭を割り当てる。
            k.index = states;
            states += info->states;
        }
        if (const std::vector<double>* defs = paramDefaults(k.op)) {
            // 係数: 既定値を params の先頭から上書き。範囲の 2 つは小さい順に。
            std::vector<double> v = *defs;
            for (std::size_t i = 0; i < v.size() && i < n.params.size(); ++i) {
                if (std::isfinite(n.params[i])) v[i] = n.params[i];
            }
            if ((k.op == FormulaOp::Saturate || k.op == FormulaOp::Deadzone) && v[0] > v[1]) {
                std::swap(v[0], v[1]);
            }
            if (n.params.size() > v.size()) {
                warnings.push_back(label + ": " + n.kind + " node " + std::to_string(id) +
                                   " has more params than it uses (" +
                                   std::to_string(v.size()) + ") - extra values ignored");
            }
            k.paramBegin = int(prog->params_.size());
            k.paramCount = int(v.size());
            prog->params_.insert(prog->params_.end(), v.begin(), v.end());
        }
        if (formulaBreaksFeedback(n.kind)) delays.push_back(k);
        prog->instrs_.push_back(k);
    }
    // delay / integrate の取り込みは全部の値が出そろった最後に。
    for (const FormulaInstr& d : delays) {
        FormulaInstr st = d;
        st.op = d.op == FormulaOp::Delay ? FormulaOp::DelayStore : FormulaOp::IntegrateStore;
        st.out[0] = -1;
        const auto c = conn.find(std::make_pair(d.nodeId, 0));
        if (c == conn.end()) {
            warnings.push_back(label + ": input 0 of node " + std::to_string(d.nodeId) + " (" +
                               byId[d.nodeId]->kind + ") is not connected - using 0");
        } else {
            st.in[0] = slotOf[c->second];
        }
        prog->instrs_.push_back(st);
    }
    for (const std::string& o : outputs) {
        if (!outputsSeen.count(o)) {
            warnings.push_back(label + ": output \"" + o + "\" is never written - stays 0");
        }
    }
    prog->slots_ = slots;
    prog->states_ = states;
    return prog;
}

std::unique_ptr<FormulaInstance> FormulaProgram::interpret() const {
    return std::make_unique<Interpreter>(shared_from_this());
}

std::string FormulaProgram::luaSource() const {
    std::ostringstream o;
    o << "-- generated from formula \"" << name_ << "\" (do not edit)\n"
         "local ffi = require(\"ffi\")\n"
         "local sin, cos, tan, atan, atan2, sqrt, exp, log, abs, min, max, pow =\n"
         "  math.sin, math.cos, math.tan, math.atan, math.atan2 or math.atan, math.sqrt,\n"
         "  math.exp, math.log, math.abs, math.min, math.max, math.pow or function(a, b) "
         "return a ^ b end\n"
         "local function curve(t, n, x)\n"
         "  if n <= 0 then return 0.0 end\n"
         "  if x <= t[0] then return t[1] end\n"
         "  if x >= t[(n - 1) * 2] then return t[(n - 1) * 2 + 1] end\n"
         "  for i = 1, n - 1 do\n"
         "    local x1 = t[i * 2]\n"
         "    if x <= x1 then\n"
         "      local x0, y0, y1 = t[(i - 1) * 2], t[(i - 1) * 2 + 1], t[i * 2 + 1]\n"
         "      local span = x1 - x0\n"
         "      local f = 0.0\n"
         "      if span > 1e-12 then f = (x - x0) / span end\n"
         "      return y0 + (y1 - y0) * f\n"
         "    end\n"
         "  end\n"
         "  return t[(n - 1) * 2 + 1]\n"
         "end\n";
    // 折れ線の点列はチャンクの読み込み時に 1 回だけ作る。
    for (const FormulaInstr& k : instrs_) {
        if (k.op != FormulaOp::Curve) continue;
        o << "local curve_" << k.nodeId << " = ffi.new(\"double[?]\", "
          << std::max(k.paramCount, 1) << ", {";
        for (int i = 0; i < k.paramCount; ++i) {
            if (i) o << ", ";
            o << fmtDouble(params_[std::size_t(k.paramBegin + i)]);
        }
        o << "})\n";
    }
    o << "return function(I, O, S, dt)\n";
    auto v = [&](int slot) { return slot >= 0 ? "v" + std::to_string(slot) : "0.0"; };
    for (const FormulaInstr& k : instrs_) {
        const std::string a = v(k.in[0]), b = v(k.in[1]), c = v(k.in[2]);
        // 係数 i（インライン化した定数）と状態 i の添字。
        auto P = [&](int i) { return fmtDouble(params_[std::size_t(k.paramBegin + i)]); };
        auto S = [&](int i) { return std::to_string(k.index + i); };
        const std::string r = k.out[0] >= 0 ? "  local v" + std::to_string(k.out[0]) + " = " : "  ";
        switch (k.op) {
            case FormulaOp::In: o << r << "I[" << k.index << "]\n"; break;
            case FormulaOp::Out: o << "  O[" << k.index << "] = " << a << "\n"; break;
            case FormulaOp::Const:
                o << r << fmtDouble(params_[std::size_t(k.paramBegin)]) << "\n";
                break;
            case FormulaOp::Dt: o << r << "dt\n"; break;
            case FormulaOp::Add: o << r << a << " + " << b << "\n"; break;
            case FormulaOp::Sub: o << r << a << " - " << b << "\n"; break;
            case FormulaOp::Mul: o << r << a << " * " << b << "\n"; break;
            case FormulaOp::Div: o << r << a << " / " << b << "\n"; break;
            case FormulaOp::Min: o << r << "min(" << a << ", " << b << ")\n"; break;
            case FormulaOp::Max: o << r << "max(" << a << ", " << b << ")\n"; break;
            case FormulaOp::Pow: o << r << "pow(" << a << ", " << b << ")\n"; break;
            case FormulaOp::Atan2: o << r << "atan2(" << a << ", " << b << ")\n"; break;
            case FormulaOp::Gt: o << r << "(" << a << " > " << b << ") and 1.0 or 0.0\n"; break;
            case FormulaOp::Lt: o << r << "(" << a << " < " << b << ") and 1.0 or 0.0\n"; break;
            case FormulaOp::Neg: o << r << "-" << a << "\n"; break;
            case FormulaOp::Abs: o << r << "abs(" << a << ")\n"; break;
            case FormulaOp::Sqrt: o << r << "sqrt(" << a << ")\n"; break;
            case FormulaOp::Sin: o << r << "sin(" << a << ")\n"; break;
            case FormulaOp::Cos: o << r << "cos(" << a << ")\n"; break;
            case FormulaOp::Tan: o << r << "tan(" << a << ")\n"; break;
            case FormulaOp::Atan: o << r << "atan(" << a << ")\n"; break;
            case FormulaOp::Exp: o << r << "exp(" << a << ")\n"; break;
            case FormulaOp::Log: o << r << "log(" << a << ")\n"; break;
            case FormulaOp::Sign:
                o << r << "(" << a << " < 0.0) and -1.0 or ((" << a << " > 0.0) and 1.0 or 0.0)\n";
                break;
            case FormulaOp::Clamp:
                o << r << "(" << a << " < " << b << ") and " << b << " or ((" << a << " > " << c
                  << ") and " << c << " or " << a << ")\n";
                break;
            case FormulaOp::Lerp: o << r << a << " + (" << b << " - " << a << ") * " << c << "\n"; break;
            case FormulaOp::Select: o << r << "(" << a << " > 0.5) and " << b << " or " << c << "\n"; break;
            case FormulaOp::Curve:
                o << r << "curve(curve_" << k.nodeId << ", " << (k.paramCount / 2) << ", " << a << ")\n";
                break;
            case FormulaOp::Lag:
                o << "  do local y = S[" << k.index << "]; local al = 1.0; if " << b
                  << " > 0.0 then al = min(1.0, dt / " << b << ") end; y = y + (" << a
                  << " - y) * al; S[" << k.index << "] = y end\n"
                  << r << "S[" << k.index << "]\n";
                break;
            case FormulaOp::Integrate:
                o << r << "S[" << k.index << "]\n";
                break;
            case FormulaOp::IntegrateStore:
                o << "  do local y = S[" << k.index << "] + " << a << " * dt";
                if (k.paramCount >= 2) {
                    const std::string lo = fmtDouble(params_[std::size_t(k.paramBegin)]);
                    const std::string hi = fmtDouble(params_[std::size_t(k.paramBegin) + 1]);
                    o << "; if y < " << lo << " then y = " << lo << " elseif y > " << hi
                      << " then y = " << hi << " end";
                }
                o << "; S[" << k.index << "] = y end\n";
                break;
            // ---- 制御ブロック（インタプリタと同じ式・同じ順序）----
            case FormulaOp::Time:
                o << r << "S[" << k.index << "]\n"
                  << "  S[" << k.index << "] = v" << k.out[0] << " + dt\n";
                break;
            case FormulaOp::Gain:
                o << r << P(0) << " * " << a << "\n";
                break;
            case FormulaOp::Saturate:
                o << r << "(" << a << " < " << P(0) << ") and " << P(0) << " or ((" << a << " > "
                  << P(1) << ") and " << P(1) << " or " << a << ")\n";
                break;
            case FormulaOp::Deadzone:
                o << r << "(" << a << " < " << P(0) << ") and (" << a << " - " << P(0) << ") or (("
                  << a << " > " << P(1) << ") and (" << a << " - " << P(1) << ") or 0.0)\n";
                break;
            case FormulaOp::Delay:
                o << r << "(S[" << S(1) << "] > 0.5) and S[" << S(0) << "] or " << P(0) << "\n";
                break;
            case FormulaOp::DelayStore:
                o << "  S[" << S(0) << "] = " << a << "; S[" << S(1) << "] = 1.0\n";
                break;
            case FormulaOp::Derivative:
                o << r << "0.0\n"
                  << "  if S[" << S(1) << "] > 0.5 and dt > 0.0 then v" << k.out[0] << " = ("
                  << a << " - S[" << S(0) << "]) / dt end\n"
                  << "  S[" << S(0) << "] = " << a << "; S[" << S(1) << "] = 1.0\n";
                break;
            case FormulaOp::RateLimit: {
                const std::string y = "v" + std::to_string(k.out[0]);
                const double rise = params_[std::size_t(k.paramBegin)];
                const double fall = params_[std::size_t(k.paramBegin) + 1];
                o << r << a << "\n"
                  << "  if S[" << S(1) << "] > 0.5 then local d = " << a << " - S[" << S(0)
                  << "]; ";
                // 0 以下の向きは制限なし（インタプリタは実行時に見るが、定数
                // なのでここで畳む - 結果は同じ）。
                if (rise > 0.0) {
                    o << "if d > " << P(0) << " * dt then " << y << " = S[" << S(0) << "] + "
                      << P(0) << " * dt";
                    if (fall > 0.0) o << " else";
                    else o << " end";
                }
                if (fall > 0.0) {
                    o << "if d < -(" << P(1) << " * dt) then " << y << " = S[" << S(0) << "] - "
                      << P(1) << " * dt end";
                }
                o << " end\n"
                  << "  S[" << S(0) << "] = " << y << "; S[" << S(1) << "] = 1.0\n";
                break;
            }
            case FormulaOp::Pid: {
                const std::string u = "v" + std::to_string(k.out[0]);
                const double tf = params_[std::size_t(k.paramBegin) + 5];
                const double lo = params_[std::size_t(k.paramBegin) + 3];
                const double hi = params_[std::size_t(k.paramBegin) + 4];
                o << "  local " << u << "\n"
                  << "  do\n"
                  << "    local e = " << a << "\n"
                  << "    local draw = 0.0\n"
                  << "    if S[" << S(3) << "] > 0.5 and dt > 0.0 then draw = (e - S[" << S(1)
                  << "]) / dt end\n"
                  << "    local d = draw\n";
                if (tf > 0.0) {
                    o << "    d = S[" << S(2) << "] + (draw - S[" << S(2) << "]) * (dt / (" << P(5)
                      << " + dt))\n";
                }
                o << "    local integ = S[" << S(0) << "] + " << P(1) << " * e * dt\n"
                  << "    local u = " << P(0) << " * e + integ + " << P(2) << " * d\n";
                if (lo < hi) {
                    o << "    if u > " << P(4) << " then u = " << P(4) << "; if " << P(1)
                      << " * e > 0.0 then integ = S[" << S(0) << "] end\n"
                      << "    elseif u < " << P(3) << " then u = " << P(3) << "; if " << P(1)
                      << " * e < 0.0 then integ = S[" << S(0) << "] end end\n";
                }
                o << "    S[" << S(0) << "] = integ; S[" << S(1) << "] = e; S[" << S(2)
                  << "] = d; S[" << S(3) << "] = 1.0\n"
                  << "    " << u << " = u\n"
                  << "  end\n";
                break;
            }
        }
    }
    o << "end\n";
    return o.str();
}

}  // namespace formula
}  // namespace wizengine
