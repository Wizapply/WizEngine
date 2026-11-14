#include "formula/FormulaText.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace wizengine {
namespace formula {

namespace {

// ---- 字句 ---------------------------------------------------------------------
enum class Tok { End, Newline, Number, Ident, Signal, String, Op };

struct Token {
    Tok kind = Tok::End;
    std::string text;  // Ident / Signal / String / Op
    double number = 0.0;
    int line = 1;
};

struct Lexer {
    const std::string& s;
    std::size_t i = 0;
    int line = 1;
    std::string error;

    explicit Lexer(const std::string& text) : s(text) {}

    static bool identStart(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }
    static bool identChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    bool run(std::vector<Token>& out) {
        while (i < s.size()) {
            const char c = s[i];
            if (c == '\n' || c == ';') {
                out.push_back(Token{Tok::Newline, "", 0.0, line});
                if (c == '\n') ++line;
                ++i;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
                continue;
            }
            if (c == '#' || (c == '/' && i + 1 < s.size() && s[i + 1] == '/')) {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && i + 1 < s.size() &&
                 std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
                const char* begin = s.c_str() + i;
                char* end = nullptr;
                const double v = std::strtod(begin, &end);
                i += std::size_t(end - begin);
                out.push_back(Token{Tok::Number, "", v, line});
                continue;
            }
            if (identStart(c)) {
                const std::size_t b = i;
                while (i < s.size() && identChar(s[i])) ++i;
                // 名前の直後に : が来たら信号名（"joint:arm.angle"）。: と . を含む
                // 続きまで 1 語として読む（- は引き算と区別できないので " " で囲む）。
                if (i < s.size() && s[i] == ':') {
                    while (i < s.size() && (identChar(s[i]) || s[i] == ':' || s[i] == '.')) ++i;
                    out.push_back(Token{Tok::Signal, s.substr(b, i - b), 0.0, line});
                } else {
                    out.push_back(Token{Tok::Ident, s.substr(b, i - b), 0.0, line});
                }
                continue;
            }
            if (c == '"') {
                const std::size_t b = ++i;
                while (i < s.size() && s[i] != '"' && s[i] != '\n') ++i;
                if (i >= s.size() || s[i] != '"') {
                    error = "line " + std::to_string(line) + ": unterminated string";
                    return false;
                }
                out.push_back(Token{Tok::String, s.substr(b, i - b), 0.0, line});
                ++i;
                continue;
            }
            if (std::string("+-*/^(),=<>").find(c) != std::string::npos) {
                out.push_back(Token{Tok::Op, std::string(1, c), 0.0, line});
                ++i;
                continue;
            }
            error = "line " + std::to_string(line) + ": unexpected character '" +
                    std::string(1, c) + "'";
            return false;
        }
        out.push_back(Token{Tok::Newline, "", 0.0, line});
        out.push_back(Token{Tok::End, "", 0.0, line});
        return true;
    }
};

// ---- 構文木 -------------------------------------------------------------------
struct Expr {
    enum Kind { Num, Name, Input, Call } kind = Num;
    double number = 0.0;
    std::string name;  // Name: 変数名 / Input: 入力名 / Call: 関数名（ノードの種類）
    std::vector<std::unique_ptr<Expr>> args;
    int line = 1;
};
using ExprP = std::unique_ptr<Expr>;

struct Statement {
    bool isOutput = false;
    std::string target;  // 変数名 or 出力名
    ExprP value;
    int line = 1;
};

// 関数名 → ノードの種類・信号の引数の数・係数の数（最小 / 最大）。
struct FuncInfo {
    const char* name;
    const char* kind;
    int inputs;      // 先頭から信号として配線する引数の数
    int minParams;   // その後ろの定数の引数（ノードの params）
    int maxParams;   // -1 = 偶数個いくらでも（curve）
};
const std::vector<FuncInfo>& funcTable() {
    static const std::vector<FuncInfo> t = {
        {"sin", "sin", 1, 0, 0},           {"cos", "cos", 1, 0, 0},
        {"tan", "tan", 1, 0, 0},           {"atan", "atan", 1, 0, 0},
        {"exp", "exp", 1, 0, 0},           {"log", "log", 1, 0, 0},
        {"sqrt", "sqrt", 1, 0, 0},         {"abs", "abs", 1, 0, 0},
        {"sign", "sign", 1, 0, 0},         {"neg", "neg", 1, 0, 0},
        {"min", "min", 2, 0, 0},           {"max", "max", 2, 0, 0},
        {"pow", "pow", 2, 0, 0},           {"atan2", "atan2", 2, 0, 0},
        {"add", "add", 2, 0, 0},           {"sub", "sub", 2, 0, 0},
        {"mul", "mul", 2, 0, 0},           {"div", "div", 2, 0, 0},
        {"gt", "gt", 2, 0, 0},             {"lt", "lt", 2, 0, 0},
        {"clamp", "clamp", 3, 0, 0},       {"lerp", "lerp", 3, 0, 0},
        {"select", "select", 3, 0, 0},     {"if", "select", 3, 0, 0},
        {"lag", "lag", 2, 0, 0},           {"derivative", "derivative", 1, 0, 0},
        {"integrate", "integrate", 1, 0, 2}, {"delay", "delay", 1, 0, 1},
        {"gain", "gain", 1, 1, 1},         {"saturate", "saturate", 1, 2, 2},
        {"deadzone", "deadzone", 1, 2, 2}, {"ratelimit", "ratelimit", 1, 2, 2},
        {"pid", "pid", 1, 0, 6},           {"curve", "curve", 1, 2, -1},
        {"time", "time", 0, 0, 0},         {"dt", "dt", 0, 0, 0},
    };
    return t;
}
const FuncInfo* findFunc(const std::string& name) {
    for (const FuncInfo& f : funcTable()) {
        if (name == f.name) return &f;
    }
    return nullptr;
}
// ノードの種類 → テキストに書くときの関数名（if は select の別名なので除く）。
const FuncInfo* funcForKind(const std::string& kind) {
    for (const FuncInfo& f : funcTable()) {
        if (kind == f.kind && std::string(f.name) != "if") return &f;
    }
    return nullptr;
}

bool reserved(const std::string& name) {
    return name == "time" || name == "dt" || name == "pi" || name == "out" ||
           findFunc(name) != nullptr;
}

// ---- 構文解析（再帰下降）------------------------------------------------------
struct Parser {
    const std::vector<Token>& t;
    std::size_t p = 0;
    std::string error;

    explicit Parser(const std::vector<Token>& tokens) : t(tokens) {}

    const Token& peek() const { return t[p]; }
    bool isOp(const char* op) const { return peek().kind == Tok::Op && peek().text == op; }
    bool fail(const std::string& msg) {
        if (error.empty()) error = "line " + std::to_string(peek().line) + ": " + msg;
        return false;
    }

    bool statements(std::vector<Statement>& out) {
        while (peek().kind != Tok::End) {
            if (peek().kind == Tok::Newline) {
                ++p;
                continue;
            }
            Statement st;
            st.line = peek().line;
            // 左辺: out 名前 / 信号名 / "名前" / 変数名
            if (peek().kind == Tok::Ident && peek().text == "out" && p + 1 < t.size() &&
                (t[p + 1].kind == Tok::Ident || t[p + 1].kind == Tok::Signal ||
                 t[p + 1].kind == Tok::String)) {
                ++p;
                st.isOutput = true;
                st.target = peek().text;
                ++p;
            } else if (peek().kind == Tok::Signal || peek().kind == Tok::String) {
                st.isOutput = true;
                st.target = peek().text;
                ++p;
            } else if (peek().kind == Tok::Ident) {
                st.target = peek().text;
                if (reserved(st.target)) {
                    return fail("\"" + st.target + "\" is a reserved name and cannot be assigned");
                }
                ++p;
            } else {
                return fail("expected a name to assign to (e.g. 'u = ...')");
            }
            if (st.target.empty()) return fail("the name to assign to is empty");
            if (!isOp("=")) return fail("expected '=' after \"" + st.target + "\"");
            ++p;
            st.value = expr();
            if (!st.value) return false;
            if (peek().kind != Tok::Newline) {
                return fail("unexpected '" + (peek().text.empty() ? std::string("?") : peek().text) +
                            "' after the expression");
            }
            out.push_back(std::move(st));
        }
        return true;
    }

    ExprP make(Expr::Kind k, int line) {
        auto e = std::make_unique<Expr>();
        e->kind = k;
        e->line = line;
        return e;
    }
    ExprP call(const char* fn, ExprP a, ExprP b = nullptr) {
        auto e = make(Expr::Call, a->line);
        e->name = fn;
        e->args.push_back(std::move(a));
        if (b) e->args.push_back(std::move(b));
        return e;
    }

    ExprP expr() {
        ExprP a = additive();
        if (!a) return nullptr;
        if (isOp(">") || isOp("<")) {
            const std::string op = peek().text;
            ++p;
            ExprP b = additive();
            if (!b) return nullptr;
            return call(op == ">" ? "gt" : "lt", std::move(a), std::move(b));
        }
        return a;
    }
    ExprP additive() {
        ExprP a = term();
        if (!a) return nullptr;
        while (isOp("+") || isOp("-")) {
            const std::string op = peek().text;
            ++p;
            ExprP b = term();
            if (!b) return nullptr;
            a = call(op == "+" ? "add" : "sub", std::move(a), std::move(b));
        }
        return a;
    }
    ExprP term() {
        ExprP a = unary();
        if (!a) return nullptr;
        while (isOp("*") || isOp("/")) {
            const std::string op = peek().text;
            ++p;
            ExprP b = unary();
            if (!b) return nullptr;
            a = call(op == "*" ? "mul" : "div", std::move(a), std::move(b));
        }
        return a;
    }
    ExprP unary() {
        if (isOp("-")) {
            ++p;
            ExprP a = unary();
            if (!a) return nullptr;
            return call("neg", std::move(a));
        }
        if (isOp("+")) {
            ++p;
            return unary();
        }
        return power();
    }
    ExprP power() {
        ExprP a = primary();
        if (!a) return nullptr;
        if (isOp("^")) {
            ++p;
            ExprP b = unary();  // 右結合（2^3^2 = 2^(3^2)）、指数に単項 - を許す
            if (!b) return nullptr;
            return call("pow", std::move(a), std::move(b));
        }
        return a;
    }
    ExprP primary() {
        const Token& k = peek();
        if (k.kind == Tok::Number) {
            ++p;
            auto e = make(Expr::Num, k.line);
            e->number = k.number;
            return e;
        }
        if (k.kind == Tok::Signal || k.kind == Tok::String) {
            ++p;
            auto e = make(Expr::Input, k.line);
            e->name = k.text;
            return e;
        }
        if (isOp("(")) {
            ++p;
            ExprP e = expr();
            if (!e) return nullptr;
            if (!isOp(")")) {
                fail("expected ')'");
                return nullptr;
            }
            ++p;
            return e;
        }
        if (k.kind == Tok::Ident) {
            const std::string name = k.text;
            const int line = k.line;
            ++p;
            if (isOp("(")) {
                ++p;
                if (!findFunc(name)) {
                    fail("unknown function \"" + name + "\"");
                    return nullptr;
                }
                auto e = make(Expr::Call, line);
                e->name = name;
                if (!isOp(")")) {
                    for (;;) {
                        ExprP a = expr();
                        if (!a) return nullptr;
                        e->args.push_back(std::move(a));
                        if (isOp(",")) {
                            ++p;
                            continue;
                        }
                        break;
                    }
                }
                if (!isOp(")")) {
                    fail("expected ')' or ',' in the call to " + name);
                    return nullptr;
                }
                ++p;
                return e;
            }
            if (name == "pi") {
                auto e = make(Expr::Num, line);
                e->number = 3.14159265358979323846;
                return e;
            }
            if (name == "time" || name == "dt") {
                auto e = make(Expr::Call, line);
                e->name = name;
                return e;
            }
            if (findFunc(name)) {
                fail("\"" + name + "\" is a function - write " + name + "(...)");
                return nullptr;
            }
            auto e = make(Expr::Name, line);
            e->name = name;
            return e;
        }
        fail(k.kind == Tok::Newline || k.kind == Tok::End ? "the expression is incomplete"
                                                           : "unexpected '" + k.text + "'");
        return nullptr;
    }
};

// 定数だけでできた式を畳む（数でなければ false）。変数も、その式が定数なら
// 定数として読む（kp = 40 と書いて pid(e, kp) に使える）。
using ConstVars = std::map<std::string, const Expr*>;
bool constValue(const Expr& e, double& out, const ConstVars* vars = nullptr, int depth = 0) {
    if (depth > 64) return false;  // 変数どうしの輪（a = b; b = a）
    if (e.kind == Expr::Num) {
        out = e.number;
        return true;
    }
    if (e.kind == Expr::Name && vars) {
        const auto it = vars->find(e.name);
        return it != vars->end() && constValue(*it->second, out, vars, depth + 1);
    }
    if (e.kind != Expr::Call) return false;
    std::vector<double> v;
    for (const auto& a : e.args) {
        double x = 0.0;
        if (!constValue(*a, x, vars, depth + 1)) return false;
        v.push_back(x);
    }
    const std::string& f = e.name;
    if (f == "neg" && v.size() == 1) out = -v[0];
    else if (f == "add" && v.size() == 2) out = v[0] + v[1];
    else if (f == "sub" && v.size() == 2) out = v[0] - v[1];
    else if (f == "mul" && v.size() == 2) out = v[0] * v[1];
    else if (f == "div" && v.size() == 2) out = v[0] / v[1];
    else if (f == "pow" && v.size() == 2) out = std::pow(v[0], v[1]);
    else return false;
    return std::isfinite(out);
}

// ---- グラフへ起こす ------------------------------------------------------------
struct Value {
    int node = -1;     // 出力元のノード（-1 = まだ = var か定数）
    std::string var;   // 後で決まる変数
    bool isConst = false;
    double number = 0.0;
};

struct Builder {
    FormulaGraphDesc& g;
    std::string error;
    int next = 1;
    std::map<std::string, int> inputs;  // 入力名 → in ノード
    std::map<double, int> consts;
    int timeNode = -1, dtNode = -1;
    const std::set<std::string>& vars;
    const ConstVars* constVars = nullptr;
    struct Pending {
        int to, port;
        std::string var;
        int line;
    };
    std::vector<Pending> pending;

    Builder(FormulaGraphDesc& graph, const std::set<std::string>& variables)
        : g(graph), vars(variables) {}

    int node(const std::string& kind, const std::string& name = "",
             std::vector<double> params = {}) {
        FormulaNodeDesc n;
        n.id = next++;
        n.kind = kind;
        n.name = name;
        n.params = std::move(params);
        g.nodes.push_back(n);
        return n.id;
    }
    int constNode(double v) {
        const auto it = consts.find(v);
        if (it != consts.end()) return it->second;
        return consts[v] = node("const", "", {v});
    }
    void wire(const Value& v, int to, int port, int line) {
        if (!v.var.empty()) {
            pending.push_back(Pending{to, port, v.var, line});
            return;
        }
        const int from = v.isConst ? constNode(v.number) : v.node;
        g.wires.push_back(FormulaWireDesc{from, 0, to, port});
    }

    bool emit(const Expr& e, Value& out) {
        double c = 0.0;
        if (constValue(e, c, constVars)) {
            out = Value{};
            out.isConst = true;
            out.number = c;
            return true;
        }
        switch (e.kind) {
            case Expr::Num:
                out = Value{};
                out.isConst = true;
                out.number = e.number;
                return true;
            case Expr::Name:
                out = Value{};
                if (vars.count(e.name)) {
                    out.var = e.name;
                    return true;
                }
                [[fallthrough]];
            case Expr::Input: {
                out = Value{};
                auto it = inputs.find(e.name);
                if (it == inputs.end()) it = inputs.emplace(e.name, node("in", e.name)).first;
                out.node = it->second;
                return true;
            }
            case Expr::Call: break;
        }
        const FuncInfo* f = findFunc(e.name);
        if (!f) return fail(e.line, "unknown function \"" + e.name + "\"");
        if (f->inputs == 0 && f->minParams == 0 && f->maxParams == 0) {
            // time / dt は 1 個だけ作って使い回す。
            int& slot = std::string(f->kind) == "time" ? timeNode : dtNode;
            if (slot < 0) slot = node(f->kind);
            out = Value{};
            out.node = slot;
            return true;
        }
        const int nargs = int(e.args.size());
        const int maxArgs = f->maxParams < 0 ? 1000 : f->inputs + f->maxParams;
        if (nargs < f->inputs + f->minParams || nargs > maxArgs) {
            std::string want = std::to_string(f->inputs + f->minParams);
            if (f->maxParams != f->minParams) {
                want += f->maxParams < 0 ? " or more" : " to " + std::to_string(maxArgs);
            }
            return fail(e.line, std::string(e.name) + "() takes " + want + " arguments, got " +
                                    std::to_string(nargs));
        }
        std::vector<double> params;
        for (int i = f->inputs; i < nargs; ++i) {
            double v = 0.0;
            if (!constValue(*e.args[std::size_t(i)], v, constVars)) {
                return fail(e.line, "argument " + std::to_string(i + 1) + " of " + e.name +
                                        "() must be a constant number (it becomes a "
                                        "parameter of the block)");
            }
            params.push_back(v);
        }
        if (f->maxParams < 0 && params.size() % 2 != 0) {
            return fail(e.line, std::string(e.name) + "() needs x y pairs after the input");
        }
        const int id = node(f->kind, "", params);
        for (int i = 0; i < f->inputs; ++i) {
            Value a;
            if (!emit(*e.args[std::size_t(i)], a)) return false;
            wire(a, id, i, e.line);
        }
        out = Value{};
        out.node = id;
        return true;
    }

    bool fail(int line, const std::string& msg) {
        if (error.empty()) error = "line " + std::to_string(line) + ": " + msg;
        return false;
    }
};

// 依存の深さで左から並べる（delay / integrate へ入る線は数えない = 輪を切る）。
void layout(FormulaGraphDesc& g) {
    std::map<int, const FormulaNodeDesc*> byId;
    for (const auto& n : g.nodes) byId[n.id] = &n;
    std::map<int, int> depth;
    std::function<int(int, int)> depthOf = [&](int id, int guard) -> int {
        const auto it = depth.find(id);
        if (it != depth.end()) return it->second;
        if (guard > 4096) return 0;
        int d = 0;
        const auto n = byId.find(id);
        if (n != byId.end() && !formulaBreaksFeedback(n->second->kind)) {
            for (const auto& w : g.wires) {
                if (w.to == id) d = std::max(d, depthOf(w.from, guard + 1) + 1);
            }
        }
        depth[id] = d;
        return d;
    };
    std::map<int, int> rows;
    for (auto& n : g.nodes) {
        int d = depthOf(n.id, 0);
        // 出力は一番右の列に揃える（読む人が右端を探せばよい）。
        if (n.kind == "out") d = -1;
        n.x = d < 0 ? 0 : 40 + d * 220;
        n.y = 0;
        if (d >= 0) n.y = 40 + 110 * rows[d]++;
    }
    int maxDepth = 0;
    for (const auto& kv : depth) maxDepth = std::max(maxDepth, kv.second);
    int row = 0;
    for (auto& n : g.nodes) {
        if (n.kind != "out") continue;
        n.x = 40 + (maxDepth + 1) * 220;
        n.y = 40 + 110 * row++;
    }
}

// ---- テキストへ起こす ----------------------------------------------------------
std::string fmtNumber(double v) {
    if (!std::isfinite(v)) return v != v ? "(0/0)" : (v > 0 ? "1e309" : "-1e309");
    char buf[40];
    for (int prec = 6; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

bool plainIdent(const std::string& s) {
    if (s.empty() || !Lexer::identStart(s[0])) return false;
    for (char c : s) {
        if (!Lexer::identChar(c)) return false;
    }
    return true;
}
bool plainSignal(const std::string& s) {
    const std::size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || !Lexer::identStart(s[0])) return false;
    for (std::size_t i = 0; i < colon; ++i) {
        if (!Lexer::identChar(s[i])) return false;
    }
    for (std::size_t i = colon; i < s.size(); ++i) {
        const char c = s[i];
        if (!Lexer::identChar(c) && c != ':' && c != '.') return false;
    }
    return true;
}
// 入力名・出力名の書き方: 素の名前（予約語でない）・信号名・"…"。
std::string nameToken(const std::string& s) {
    if (plainSignal(s)) return s;
    if (plainIdent(s) && !reserved(s)) return s;
    return "\"" + s + "\"";
}

}  // namespace

bool formulaFromText(const std::string& text, const std::string& name, FormulaGraphDesc& out,
                     std::string& error, std::vector<std::string>* warnings) {
    std::vector<Token> tokens;
    Lexer lex(text);
    if (!lex.run(tokens)) {
        error = lex.error;
        return false;
    }
    std::vector<Statement> stmts;
    Parser parser(tokens);
    if (!parser.statements(stmts)) {
        error = parser.error;
        return false;
    }
    // 変数（左辺の名前）を先に集める = 後で代入するものも参照できる。
    std::set<std::string> vars;
    std::set<std::string> outputs;
    std::map<std::string, int> assignedAt;
    for (const Statement& st : stmts) {
        if (st.isOutput) {
            if (!outputs.insert(st.target).second) {
                error = "line " + std::to_string(st.line) + ": output \"" + st.target +
                        "\" is written twice";
                return false;
            }
            continue;
        }
        if (assignedAt.count(st.target)) {
            error = "line " + std::to_string(st.line) + ": \"" + st.target +
                    "\" is assigned twice (first on line " +
                    std::to_string(assignedAt[st.target]) + ")";
            return false;
        }
        assignedAt[st.target] = st.line;
        vars.insert(st.target);
    }

    FormulaGraphDesc g;
    g.name = name;
    Builder b(g, vars);
    ConstVars constVars;
    for (const Statement& st : stmts) {
        if (!st.isOutput) constVars[st.target] = st.value.get();
    }
    b.constVars = &constVars;
    std::map<std::string, Value> varValue;
    for (const Statement& st : stmts) {
        Value v;
        if (!b.emit(*st.value, v)) {
            error = b.error;
            return false;
        }
        if (st.isOutput) {
            const int o = b.node("out", st.target);
            b.wire(v, o, 0, st.line);
        } else {
            varValue[st.target] = v;
        }
    }
    // 変数の参照をつなぐ（別名 a = b は辿る。辿って輪になるのは代数ループ）。
    std::set<std::string> used;
    for (const auto& pnd : b.pending) {
        std::string cur = pnd.var;
        std::set<std::string> seen;
        Value v = varValue[cur];
        used.insert(cur);
        while (!v.var.empty()) {
            if (!seen.insert(cur).second) {
                error = "line " + std::to_string(pnd.line) + ": \"" + pnd.var +
                        "\" is defined in terms of itself (algebraic loop)";
                return false;
            }
            cur = v.var;
            used.insert(cur);
            v = varValue[cur];
        }
        const int from = v.isConst ? b.constNode(v.number) : v.node;
        g.wires.push_back(FormulaWireDesc{from, 0, pnd.to, pnd.port});
    }
    if (warnings) {
        for (const auto& kv : assignedAt) {
            double cv = 0.0;
            const Expr* ex = constVars[kv.first];
            if (ex && constValue(*ex, cv, &constVars)) continue;  // 名前付きの定数
            if (!used.count(kv.first)) {
                warnings->push_back("line " + std::to_string(kv.second) + ": \"" + kv.first +
                                    "\" is assigned but never used");
            }
        }
    }
    if (outputs.empty() && warnings) {
        warnings->push_back("the formula has no outputs (write 'out name = ...' or a signal "
                            "name such as joint:arm.target = ...)");
    }
    layout(g);

    // 実行できるか（循環・接続）をここで確かめる。入出力は書いたとおり。
    std::vector<std::string> ins, outs;
    for (const auto& n : g.nodes) {
        if (n.kind == "in") ins.push_back(n.name);
        if (n.kind == "out") outs.push_back(n.name);
    }
    std::vector<std::string> errors, warns;
    if (!FormulaProgram::compile(g, ins, outs, errors, warns)) {
        error = errors.empty() ? "the formula cannot be compiled" : errors.front();
        return false;
    }
    if (warnings) warnings->insert(warnings->end(), warns.begin(), warns.end());
    out = std::move(g);
    return true;
}

std::string formulaToText(const FormulaGraphDesc& graph) {
    std::map<int, const FormulaNodeDesc*> byId;
    for (const auto& n : graph.nodes) {
        if (formulaKind(n.kind)) byId[n.id] = &n;
    }
    // (to, port) → from。同じ入力に 2 本あれば最初の 1 本（compile と同じ）。
    std::map<std::pair<int, int>, int> src;
    std::map<int, int> uses;
    std::map<int, bool> feedsBreaker;
    for (const auto& w : graph.wires) {
        if (!byId.count(w.from) || !byId.count(w.to) || w.fromPort != 0) continue;
        const auto key = std::make_pair(w.to, w.port);
        if (src.count(key)) continue;
        src[key] = w.from;
        ++uses[w.from];
        if (formulaBreaksFeedback(byId[w.to]->kind)) feedsBreaker[w.from] = true;
    }
    // 変数名（入力名と重ならないように）。
    std::set<std::string> taken;
    for (const auto& kv : byId) {
        if (kv.second->kind == "in" || kv.second->kind == "out") taken.insert(kv.second->name);
    }
    std::map<int, std::string> varName;
    auto nameOf = [&](int id) {
        auto it = varName.find(id);
        if (it != varName.end()) return it->second;
        std::string n = "v" + std::to_string(id);
        while (taken.count(n)) n += "_";
        taken.insert(n);
        return varName[id] = n;
    };
    // 変数にするノード: 2 回以上使う・delay / integrate の入力になる（後から
    // 参照される）・どこにも使われない（文として残す）。in / const / time / dt
    // は式の中に直接書く。
    auto inlineLeaf = [&](const FormulaNodeDesc& n) {
        return n.kind == "in" || n.kind == "const" || n.kind == "time" || n.kind == "dt";
    };
    auto named = [&](int id) {
        const FormulaNodeDesc& n = *byId[id];
        if (n.kind == "out" || inlineLeaf(n)) return false;
        return uses[id] != 1 || feedsBreaker[id];
    };

    // 式の文字列と優先順位（括弧を必要なところだけに付けるため）:
    //   0 = 比較、1 = + -、2 = * /、3 = 単項 -、4 = ^、5 = 名前・数・関数
    struct Text {
        std::string s;
        int prec = 5;
    };
    auto paren = [](const Text& t, bool need) { return need ? "(" + t.s + ")" : t.s; };
    std::function<Text(int)> exprOf;
    std::function<Text(int)> body = [&](int id) -> Text {
        const FormulaNodeDesc& n = *byId[id];
        const FormulaKindInfo* info = formulaKind(n.kind);
        auto arg = [&](int port) {
            const auto it = src.find(std::make_pair(id, port));
            return it == src.end() ? Text{"0", 5} : exprOf(it->second);
        };
        if (n.kind == "in") return Text{nameToken(n.name), 5};
        if (n.kind == "const") {
            const double v = n.params.empty() ? 0.0 : n.params[0];
            return Text{fmtNumber(v), v < 0.0 ? 3 : 5};
        }
        if (n.kind == "time") return Text{"time", 5};
        if (n.kind == "dt") return Text{"dt", 5};
        struct Infix {
            const char* op;
            int prec;
        };
        static const std::map<std::string, Infix> infix = {
            {"add", {" + ", 1}}, {"sub", {" - ", 1}}, {"mul", {" * ", 2}},
            {"div", {" / ", 2}}, {"gt", {" > ", 0}},  {"lt", {" < ", 0}},
            {"pow", {" ^ ", 4}}};
        const auto op = infix.find(n.kind);
        if (op != infix.end()) {
            const int pr = op->second.prec;
            const Text l = arg(0), r = arg(1);
            bool needL = l.prec < pr, needR = r.prec < pr;
            if (pr == 4) {  // べきは右結合: 左のべき・単項 - は括弧、右は単項 - まで可
                needL = l.prec <= 4;
                needR = r.prec < 3;
            } else if (pr == 0) {  // 比較は連ねられない
                needL = l.prec <= 0;
                needR = r.prec <= 0;
            } else if (n.kind == "sub" || n.kind == "div") {
                needR = r.prec <= pr;  // a - (b + c)、a / (b * c)
            }
            return Text{paren(l, needL) + op->second.op + paren(r, needR), pr};
        }
        if (n.kind == "neg") {
            const Text a0 = arg(0);
            return Text{"-" + paren(a0, a0.prec < 4), 3};
        }
        const FuncInfo* f = funcForKind(n.kind);
        std::string s = std::string(f ? f->name : n.kind.c_str()) + "(";
        for (int i = 0; i < info->inputs; ++i) s += (i ? ", " : "") + arg(i).s;
        // 係数（params）。curve は全部、それ以外は表の最大数まで。
        const std::size_t maxP = f && f->maxParams >= 0 ? std::size_t(f->maxParams)
                                                        : n.params.size();
        for (std::size_t i = 0; i < n.params.size() && i < maxP; ++i) {
            s += (info->inputs > 0 || i ? ", " : "") + fmtNumber(n.params[i]);
        }
        return Text{s + ")", 5};
    };
    exprOf = [&](int id) -> Text {
        if (named(id)) return Text{nameOf(id), 5};
        return body(id);
    };

    // 文の順: compile と同じ整列（delay / integrate へ入る線は数えない）。
    std::map<int, int> indeg;
    std::map<int, std::vector<int>> succ;
    for (const auto& kv : byId) indeg[kv.first] = 0;
    for (const auto& kv : src) {
        if (formulaBreaksFeedback(byId[kv.first.first]->kind)) continue;
        ++indeg[kv.first.first];
        succ[kv.second].push_back(kv.first.first);
    }
    std::vector<int> ready, order;
    for (const auto& kv : indeg) {
        if (kv.second == 0) ready.push_back(kv.first);
    }
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end());
        const int id = ready.front();
        ready.erase(ready.begin());
        order.push_back(id);
        for (int s : succ[id]) {
            if (--indeg[s] == 0) ready.push_back(s);
        }
    }
    for (const auto& kv : indeg) {  // 循環で残ったもの（壊れたグラフ）も落とさない
        if (std::find(order.begin(), order.end(), kv.first) == order.end()) {
            order.push_back(kv.first);
        }
    }

    std::ostringstream o;
    o << "# " << (graph.name.empty() ? std::string("formula") : graph.name) << "\n";
    for (int id : order) {
        const FormulaNodeDesc& n = *byId[id];
        if (n.kind == "out") {
            const auto it = src.find(std::make_pair(id, 0));
            const std::string rhs = it == src.end() ? "0" : exprOf(it->second).s;
            const std::string lhs = plainIdent(n.name) && !reserved(n.name)
                                        ? "out " + n.name
                                        : nameToken(n.name);
            o << lhs << " = " << rhs << "\n";
        } else if (named(id)) {
            o << nameOf(id) << " = " << body(id).s << "\n";
        }
    }
    return o.str();
}

}  // namespace formula
}  // namespace wizengine
