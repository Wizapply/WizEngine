#include "control/ControlTemplates.h"

#include "formula/FormulaText.h"

namespace wizengine {
namespace control {

namespace {

struct Entry {
    FormulaTemplate info;
    const char* text;
};

// 雛形の本体。数式の書式は FormulaText.h。ゲインは「とりあえず動く」値で、
// 実際の対象に合わせて書き換える前提（コメントに目安）。
const std::vector<Entry>& entries() {
    static const std::vector<Entry> t = {
        {{"empty", "空の計算式", "何も無いところから組む", ""}, ""},
        {{"pid_joint", "PID: ジョイントの角度を目標へ",
          "トルク指令のモータ（motor=\"force\"）を PID で回す", "joint"},
         "# PID でジョイント {joint} の角度を目標へ寄せる。\n"
         "# モータは motor=\"force\"（トルク指令）を想定。角度は rad。\n"
         "kp = 40; ki = 10; kd = 4    # ゲイン（慣性 1 kg m^2 程度の目安）\n"
         "umax = 50                   # トルクの上限 (N m)\n"
         "ref = 0.5 * (time > 1)      # 目標: 1 秒後に 0.5 rad へのステップ\n"
         "y = joint:{joint}.angle\n"
         "u = pid(ref - y, kp, ki, kd, -umax, umax, 0.02)\n"
         "joint:{joint}.target = u\n"
         "scope:ref = ref\n"
         "scope:angle = y\n"
         "scope:torque = u\n"},
        {{"height_body", "PD + 重力補償: 物体の高さを保つ",
          "上向きの力で物体を浮かせ、高さを目標に保つ", "body"},
         "# 物体 {body} を上向きの力で浮かせ、高さを目標に保つ。\n"
         "m = 1                       # 物体の質量 (kg) に合わせる\n"
         "g = 9.81\n"
         "kp = 30 * m; ki = 20 * m; kd = 12 * m\n"
         "ref = ratelimit(1 + (time > 2), 0.5, 0.5)   # 1 m → 2 m（0.5 m/s で）\n"
         "y = body:{body}.y\n"
         "f = pid(ref - y, kp, ki, kd, -20 * m, 20 * m, 0.02) + m * g\n"
         "body:{body}.fy = f\n"
         "scope:ref = ref\n"
         "scope:height = y\n"},
        {{"first_order", "一次遅れ系の応答", "K / (T s + 1) のステップ応答（プラントも式の中）",
          ""},
         "# 一次遅れ系 K / (T s + 1) のステップ応答。プラントも式の中で積分する\n"
         "# （シーンの物は使わない = 制御設計の机上検討）。\n"
         "K = 2; T = 0.5\n"
         "u = time > 1                # 1 秒後にステップ\n"
         "y = integrate((K * u - y) / T)\n"
         "scope:u = u\n"
         "scope:y = y\n"},
        {{"mass_spring", "質量-ばね-ダンパ", "m x'' + c x' + k x = F の自由振動とステップ応答", ""},
         "# 質量-ばね-ダンパ m x'' + c x' + k x = F。積分器 2 つの輪で解く。\n"
         "m = 1; k = 40; c = 2\n"
         "F = 10 * (time > 0.5)\n"
         "a = (F - c * v - k * x) / m\n"
         "v = integrate(a)\n"
         "x = integrate(v)\n"
         "scope:x = x\n"
         "scope:v = v\n"
         "scope:F = F\n"},
        {{"pi_plant", "PI 制御 + 一次遅れプラント", "閉ループの応答を式の中だけで確かめる", ""},
         "# PI 制御器と一次遅れのプラントの閉ループ（すべて式の中）。\n"
         "K = 1.5; T = 0.8            # プラント\n"
         "kp = 2; ki = 3              # 制御器\n"
         "ref = time > 0.5\n"
         "u = pid(ref - y, kp, ki, 0, -5, 5)\n"
         "y = integrate((K * u - y) / T)\n"
         "scope:ref = ref\n"
         "scope:y = y\n"
         "scope:u = u\n"},
        {{"signals", "信号発生器", "正弦波・矩形波・のこぎり波・ステップ", ""},
         "# 信号発生器。周波数と振幅を変えて使う。\n"
         "f = 0.5; A = 1              # 周波数 (Hz)・振幅\n"
         "w = 2 * pi * f\n"
         "scope:sine = A * sin(w * time)\n"
         "scope:square = A * sign(sin(w * time))\n"
         "scope:saw = A * 2 / pi * atan(tan(pi * f * time))   # のこぎり波（-A..A）\n"
         "scope:step = A * (time > 1)\n"},
    };
    return t;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
        s.replace(p, from.size(), to);
    }
}

}  // namespace

const std::vector<FormulaTemplate>& formulaTemplates() {
    static const std::vector<FormulaTemplate> list = [] {
        std::vector<FormulaTemplate> v;
        for (const Entry& e : entries()) v.push_back(e.info);
        return v;
    }();
    return list;
}

std::string templateText(const std::string& id, const std::string& target) {
    for (const Entry& e : entries()) {
        if (e.info.id != id) continue;
        std::string text = e.text;
        const std::string who = target.empty()
                                    ? std::string(e.info.needs == "joint" ? "hinge" : "box")
                                    : target;
        replaceAll(text, "{joint}", who);
        replaceAll(text, "{body}", who);
        return text;
    }
    return "";
}

bool templateGraph(const std::string& id, const std::string& name, const std::string& target,
                   wizengine::formula::FormulaGraphDesc& out, std::string& error) {
    bool known = false;
    for (const Entry& e : entries()) known = known || e.info.id == id;
    if (!known) {
        error = "unknown template \"" + id + "\"";
        return false;
    }
    const std::string text = templateText(id, target);
    if (text.empty()) {  // 空の計算式
        out = wizengine::formula::FormulaGraphDesc{};
        out.name = name;
        return true;
    }
    if (!wizengine::formula::formulaFromText(text, name, out, error)) return false;
    out.source = text;  // 元のテキスト（コメント・名前付き定数）も残す
    return true;
}

}  // namespace control
}  // namespace wizengine
