// 車両モデルの単体テスト。Chrono の代わりに、ここにある小さな 6 自由度の
// 剛体積分器で車体を動かし、次を確かめる:
//   1. 静置: サスで 4 輪が支え、ピッチ・ロールが出ない
//   2. 直線加速: 0-100 km/h の所要時間と、速度が単調に伸びること
//   3. 制動: 止まり切り、逆走しないこと
//   4. 定常旋回: ヨーレートの符号と、横転しないこと
//   5. 後退: 停車中に S を踏み続けるとリバースに入り、実際に下がること
//   6. XML の往復: vehicleElement → vehicleFromXml で値が戻ること
//   7. ノード式: 既定の Magic Formula グラフが、C++ インタプリタでも LuaJIT
//      でも組み込みと同じ数字を出すこと。壊れたグラフが警告 / エラーになる
//      こと。ノード式を付けた車が、組み込みと同じ走りになること
// 走らせ方は tests/vehicle/CMakeLists.txt。--csv <file> で時系列を出す。
// --dump-formula で既定のタイヤ式を XML で出す（サンプルシーンに貼る用）。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "SceneXml.h"
#include "vehicle/Formula.h"
#include "vehicle/FormulaXml.h"
#include "vehicle/LuaFormula.h"
#include "vehicle/TireFormula.h"
#include "vehicle/VehicleModel.h"
#include "vehicle/VehicleXml.h"

using namespace wizengine::vehicle;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

// 箱の剛体（質量・寸法）。外力を点に掛けて semi-implicit Euler で積分する。
struct RigidBox {
    double mass = 1400.0;
    Vec3 size{1.8, 0.5, 4.2};  // x, y, z の辺
    Vec3 pos;
    Quat rot;
    Vec3 vel;
    Vec3 angVel;  // ワールド
    Vec3 forceAcc;
    Vec3 torqueAcc;

    Vec3 invInertiaLocal() const {
        const double a = size.x, b = size.y, c = size.z;
        return {12.0 / (mass * (b * b + c * c)), 12.0 / (mass * (a * a + c * c)),
                12.0 / (mass * (a * a + b * b))};
    }
    void applyForceAtPoint(const Vec3& f, const Vec3& p) {
        forceAcc += f;
        torqueAcc += (p - pos).cross(f);
    }
    void integrate(double dt, double gravity) {
        forceAcc += Vec3{0.0, -gravity * mass, 0.0};
        vel += forceAcc * (dt / mass);
        const Vec3 tl = rot.rotateBack(torqueAcc);
        const Vec3 inv = invInertiaLocal();
        const Vec3 dwl{tl.x * inv.x, tl.y * inv.y, tl.z * inv.z};
        angVel += rot.rotate(dwl) * dt;
        pos += vel * dt;
        const Quat dq{0.0, angVel.x, angVel.y, angVel.z};
        const Quat qd = dq * rot;
        rot = Quat{rot.w + 0.5 * qd.w * dt, rot.x + 0.5 * qd.x * dt,
                   rot.y + 0.5 * qd.y * dt, rot.z + 0.5 * qd.z * dt}
                  .normalized();
        forceAcc = Vec3{};
        torqueAcc = Vec3{};
    }
    ChassisState state() const {
        ChassisState s;
        s.position = pos;
        s.rotation = rot;
        s.velocity = vel;
        s.angularVelocity = angVel;
        s.mass = mass;
        return s;
    }
};

struct Sim {
    RigidBox body;
    VehicleModel model;
    double dt = 1.0 / 60.0;
    double time = 0.0;
    std::FILE* csv = nullptr;

    explicit Sim(const VehicleDesc& d) : model(d) {}

    void place(double y) {
        body.pos = Vec3{0.0, y, 0.0};
        body.rot = Quat{};
        body.vel = Vec3{};
        body.angVel = Vec3{};
        model.reset();
        time = 0.0;
    }
    void step() {
        const auto& forces = model.step(body.state(), dt, planeGroundQuery);
        for (const ForceAtPoint& f : forces) body.applyForceAtPoint(f.force, f.point);
        body.integrate(dt, 9.81);
        time += dt;
        if (csv) {
            const VehicleTelemetry& t = model.telemetry();
            std::fprintf(csv, "%.4f,%.3f,%.1f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                         time, t.speed, t.rpm, t.gear, t.clutch, body.pos.y,
                         t.wheels[0].load, t.wheels[2].load, t.wheels[2].slipRatio,
                         yawRate());
        }
    }
    void run(double seconds) {
        const int n = int(seconds / dt + 0.5);
        for (int i = 0; i < n; ++i) step();
    }
    double speed() const { return model.telemetry().speed; }
    double yawRate() const { return body.angVel.y; }
    double upDot() const { return body.rot.rotate(Vec3{0.0, 1.0, 0.0}).y; }
    // ピッチ角（度、前下がりが +）とロール角。
    double pitchDeg() const {
        const Vec3 f = body.rot.rotate(Vec3{0.0, 0.0, -1.0});
        return radToDeg(std::asin(-f.y));
    }
    double rollDeg() const {
        const Vec3 r = body.rot.rotate(Vec3{1.0, 0.0, 0.0});
        return radToDeg(std::asin(r.y));
    }
};

// 4 輪で車体を支えるときの重心高さ（静的な縮みを引いた値）。
double restHeight(const VehicleDesc& d, double mass) {
    const AxleDesc& a = d.axles[0];
    const double perWheel = mass * 9.81 / double(d.axles.size() * 2);
    const double x0 = perWheel / a.suspension.stiffness;
    return a.tire.radius + a.suspension.restLength - x0 - a.y;
}

}  // namespace

int main(int argc, char** argv) {
    const char* csvPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--csv") == 0 && i + 1 < argc) csvPath = argv[i + 1];
        if (std::strcmp(argv[i], "--bench") == 0) {
            // 1 ティックあたりのコスト: 組み込み / インタプリタ / LuaJIT。
            LuaRuntime lua;
            const FormulaGraphDesc graph = defaultTireFormulaGraph();
            std::vector<std::string> e, w;
            auto prog = FormulaProgram::compile(graph, tireFormulaInputs(), tireFormulaOutputs(), e, w);
            TireDesc tire;
            auto bench = [&](const char* label, std::unique_ptr<FormulaInstance> f) {
                WheelController wc(tire, SuspensionDesc{}, Vec3{0.8, -0.15, -1.3});
                wc.setTireFormula(std::move(f));
                ChassisState ch;
                ch.position = Vec3{0.0, 0.72, 0.0};
                ch.mass = 1400.0;
                ch.velocity = Vec3{0.3, 0.0, -12.0};
                wc.updateContact(ch, 0.02, planeGroundQuery, 350.0, 1.0 / 60.0);
                const int n = 2000000;
                double acc = 0.0;
                const auto t0 = std::chrono::steady_clock::now();
                for (int k = 0; k < n; ++k) {
                    acc += wc.tick(38.0 + 0.001 * (k % 100), 1.0 / 600.0);
                }
                const double ns = std::chrono::duration<double, std::nano>(
                                      std::chrono::steady_clock::now() - t0).count() / n;
                std::printf("  %-12s %7.1f ns/tick  (checksum %.3f)\n", label, ns, acc);
            };
            bench("builtin", nullptr);
            if (prog) bench("interpreter", prog->interpret());
            std::string err;
            if (prog && lua.ok()) {
                auto inst = lua.instantiate(prog, err);
                if (inst) bench("luajit", std::move(inst)); else std::printf("  luajit: %s\n", err.c_str());
            }
            return 0;
        }
        if (std::strcmp(argv[i], "--dump-formula") == 0) {
            std::printf("%s", wizengine::xml::write(
                                  formulaElement(defaultTireFormulaGraph()), false)
                                  .c_str());
            return 0;
        }
    }

    VehicleDesc desc = defaultVehicleDesc();
    const double h = restHeight(desc, 1400.0);
    std::printf("rest height %.3f m\n", h);

    // ---- 1. 静置 ---------------------------------------------------------
    std::printf("1. settle\n");
    {
        Sim sim(desc);
        sim.place(h + 0.05);
        sim.run(4.0);
        double loadSum = 0.0;
        for (const auto& w : sim.model.telemetry().wheels) loadSum += w.load;
        std::printf("  y=%.3f pitch=%.2f roll=%.2f loadSum=%.0f (mg=%.0f) speed=%.4f\n",
                    sim.body.pos.y, sim.pitchDeg(), sim.rollDeg(), loadSum,
                    1400.0 * 9.81, sim.speed());
        check(std::fabs(sim.body.pos.y - h) < 0.02, "ride height within 2 cm of static");
        check(std::fabs(sim.pitchDeg()) < 0.5 && std::fabs(sim.rollDeg()) < 0.5,
              "level within 0.5 deg");
        check(std::fabs(loadSum - 1400.0 * 9.81) < 200.0, "wheel loads carry the mass");
        check(std::fabs(sim.speed()) < 0.02 && sim.body.vel.length() < 0.02,
              "no creep while idling");
        const double rpm = sim.model.telemetry().rpm;
        std::printf("  idle rpm=%.0f\n", rpm);
        check(rpm > 700.0 && rpm < 1100.0, "engine idles near idle rpm");
    }

    // ---- 2. 直線加速 -------------------------------------------------------
    std::printf("2. full-throttle acceleration\n");
    {
        Sim sim(desc);
        if (csvPath) {
            sim.csv = std::fopen(csvPath, "w");
            if (sim.csv) {
                std::fprintf(sim.csv, "t,speed,rpm,gear,clutch,y,loadFL,loadRL,slipRL,yawRate\n");
            }
        }
        sim.place(h);
        sim.run(1.0);
        VehicleInput in;
        in.throttle = 1.0;
        sim.model.setInput(in);
        double t100 = -1.0;
        double prev = 0.0;
        bool monotone = true;
        double maxSlip = 0.0;
        for (int i = 0; i < 60 * 15; ++i) {
            sim.step();
            const double v = sim.speed();
            if (v < prev - 0.3) monotone = false;  // 変速のトルク断は許す
            prev = v;
            if (t100 < 0.0 && v >= 100.0 / 3.6) t100 = sim.time - 1.0;
            maxSlip = std::max(maxSlip, std::fabs(sim.model.telemetry().wheels[2].slipRatio));
            if (i % 60 == 59) {
                const auto& t = sim.model.telemetry();
                std::printf("  t=%4.1f v=%6.2f m/s (%5.1f km/h) rpm=%5.0f gear=%d clutch=%.2f pitch=%.2f\n",
                            sim.time - 1.0, v, v * 3.6, t.rpm, t.gear, t.clutch, sim.pitchDeg());
            }
        }
        std::printf("  0-100 km/h: %.2f s, max rear slip ratio %.2f\n", t100, maxSlip);
        check(t100 > 0.0 && t100 < 15.0, "reaches 100 km/h within 15 s");
        check(monotone, "speed never drops by more than 0.3 m/s");
        check(sim.upDot() > 0.99, "stays upright");
        check(std::fabs(sim.rollDeg()) < 1.0 && std::fabs(sim.body.pos.x) < 1.0,
              "drives straight");
        check(sim.model.telemetry().gear >= 3, "shifted up through the gears");
        if (sim.csv) std::fclose(sim.csv);
    }

    // ---- 3. 制動 -----------------------------------------------------------
    std::printf("3. braking from speed\n");
    {
        Sim sim(desc);
        sim.place(h);
        sim.run(0.5);
        // 車体に初速を与え、車輪も転がっている状態から始める。
        sim.body.vel = Vec3{0.0, 0.0, -25.0};
        sim.run(0.5);  // タイヤが車輪を回し切るまで
        const double v0 = sim.speed();
        VehicleInput in;
        in.brake = 1.0;
        sim.model.setInput(in);
        const double z0 = sim.body.pos.z;
        double tStop = -1.0;
        double dist = 0.0;
        bool reversed = false;
        for (int i = 0; i < 60 * 8; ++i) {
            sim.step();
            if (tStop < 0.0 && std::fabs(sim.speed()) < 0.05) {
                tStop = sim.time;
                dist = std::fabs(sim.body.pos.z - z0);
                // 止まったらペダルを離す（踏み続けるとリバースに入る仕様）。
                sim.model.setInput(VehicleInput{});
            }
            if (sim.speed() < -0.3) reversed = true;
        }
        std::printf("  v0=%.2f m/s stop distance=%.1f m, stopped after %.2f s, final v=%.3f\n",
                    v0, dist, tStop, sim.speed());
        check(v0 > 20.0, "wheels spun up to the chassis speed");
        check(tStop > 0.0, "comes to a stop");
        check(dist < 60.0, "stops within 60 m from ~25 m/s");
        check(!reversed, "does not reverse after stopping");
        check(sim.upDot() > 0.99, "stays upright while braking");
    }

    // ---- 4. 定常旋回 -------------------------------------------------------
    std::printf("4. steady cornering\n");
    {
        Sim sim(desc);
        sim.place(h);
        sim.run(0.5);
        sim.body.vel = Vec3{0.0, 0.0, -15.0};
        sim.run(0.5);
        VehicleInput in;
        in.throttle = 0.35;
        in.steer = 0.4;  // 左
        sim.model.setInput(in);
        double yawMax = 0.0;
        for (int i = 0; i < 60 * 6; ++i) {
            sim.step();
            yawMax = std::max(yawMax, sim.yawRate());
            if (i % 60 == 59) {
                std::printf("  t=%.1f v=%.2f yawRate=%.3f rad/s roll=%.2f deg x=%.1f z=%.1f\n",
                            sim.time, sim.speed(), sim.yawRate(), sim.rollDeg(),
                            sim.body.pos.x, sim.body.pos.z);
            }
        }
        check(sim.yawRate() > 0.1, "turns left (positive yaw rate) with positive steer");
        check(sim.upDot() > 0.98, "does not roll over");
        check(std::fabs(sim.speed()) > 5.0, "keeps rolling through the corner");
        check(sim.body.pos.x < -1.0, "path curves to the left");
    }

    // ---- 5. 後退 -----------------------------------------------------------
    std::printf("5. reverse\n");
    {
        Sim sim(desc);
        sim.place(h);
        sim.run(1.0);
        VehicleInput in;
        in.brake = 1.0;  // 停車中に S を踏み続ける
        sim.model.setInput(in);
        sim.run(4.0);
        std::printf("  gear=%d v=%.2f m/s z=%.2f\n", sim.model.telemetry().gear,
                    sim.speed(), sim.body.pos.z);
        check(sim.model.telemetry().gear == -1, "selects reverse");
        check(sim.speed() < -1.0, "moves backwards");
        // W で止まり、止まったら前進へ戻る。
        in.brake = 0.0;
        in.throttle = 1.0;
        sim.model.setInput(in);
        sim.run(4.0);
        std::printf("  gear=%d v=%.2f m/s\n", sim.model.telemetry().gear, sim.speed());
        check(sim.model.telemetry().gear >= 1 && sim.speed() > 1.0,
              "returns to forward drive");
    }

    // ---- 6. XML の往復 -----------------------------------------------------
    std::printf("6. xml round trip\n");
    {
        VehicleDesc d = defaultVehicleDesc();
        d.axles[1].diff.kind = DiffKind::Lsd;
        d.axles[1].diff.lockTorque = 250.0;
        d.engine.torqueCurve = defaultTorqueCurve();
        d.gearbox.gears = {3.0, 2.0, 1.0};
        d.ticks = 12;
        const std::string text = wizengine::xml::write(vehicleElement(d));
        wizengine::xml::Element parsed;
        std::string err;
        check(wizengine::xml::parse(text, parsed, err), "written xml parses back");
        std::vector<std::string> warnings;
        const VehicleDesc back = vehicleFromXml(parsed, &warnings);
        check(warnings.empty(), "no warnings on a clean document");
        check(back.axles.size() == 2 && back.axles[1].diff.kind == DiffKind::Lsd &&
                  std::fabs(back.axles[1].diff.lockTorque - 250.0) < 1e-6,
              "axle differential survives");
        check(back.gearbox.gears.size() == 3 && std::fabs(back.gearbox.gears[1] - 2.0) < 1e-9,
              "gear ratios survive");
        check(back.engine.torqueCurve.size() == d.engine.torqueCurve.size(),
              "torque curve survives");
        check(back.ticks == 12 && std::fabs(back.axles[0].steerDeg - 32.0) < 1e-9,
              "scalars survive");
        // 壊れ気味の文書: 未知の節と駆動軸なしは警告になる。
        const std::string bad =
            "<vehicle><turbo/><axle z=\"1\"/><axle z=\"-1\"/></vehicle>";
        wizengine::xml::Element badEl;
        check(wizengine::xml::parse(bad, badEl, err), "bad xml still parses");
        warnings.clear();
        vehicleFromXml(badEl, &warnings);
        check(warnings.size() == 2, "unknown element + no driven axle -> 2 warnings");
        for (const auto& w : warnings) std::printf("    warning: %s\n", w.c_str());
    }

    // ---- 7. ノード式 ---------------------------------------------------------
    std::printf("7. node formulas\n");
    {
        LuaRuntime lua;
        std::printf("  backend: %s, jit %s\n", lua.version().c_str(),
                    lua.jitEnabled() ? "on" : "off");
        check(!LuaRuntime::available() || lua.ok(), "lua runtime starts");

        // 既定グラフのコンパイル。
        const FormulaGraphDesc graph = defaultTireFormulaGraph();
        std::vector<std::string> errors, warnings;
        auto prog = FormulaProgram::compile(graph, tireFormulaInputs(), tireFormulaOutputs(),
                                            errors, warnings);
        for (const auto& e : errors) std::printf("    error: %s\n", e.c_str());
        for (const auto& w : warnings) std::printf("    warning: %s\n", w.c_str());
        check(prog != nullptr && errors.empty() && warnings.empty(),
              "default tire graph compiles cleanly");
        if (prog) {
            std::printf("  %zu instructions, %d slots, %d states\n",
                        prog->instructions().size(), prog->slotCount(), prog->stateCount());
        }

        // 3 つの実装で同じ入力列を流し、力を比べる（緩和長の状態も含めて）。
        if (prog) {
            TireDesc tire;
            auto makeWheel = [&](std::unique_ptr<FormulaInstance> f) {
                WheelController w(tire, SuspensionDesc{}, Vec3{0.8, -0.15, -1.3});
                w.setTireFormula(std::move(f));
                return w;
            };
            WheelController builtin = makeWheel(nullptr);
            WheelController interp = makeWheel(prog->interpret());
            std::string err;
            std::unique_ptr<FormulaInstance> luaInst;
            if (lua.ok()) luaInst = lua.instantiate(prog, err);
            if (lua.ok() && !luaInst) std::printf("    luajit: %s\n", err.c_str());
            check(!lua.ok() || luaInst != nullptr, "luajit instantiates the default graph");
            WheelController jit = makeWheel(luaInst ? std::move(luaInst) : prog->interpret());

            ChassisState ch;
            ch.position = Vec3{0.0, 0.72, 0.0};
            ch.mass = 1400.0;
            double maxDiffI = 0.0, maxDiffL = 0.0;
            const double dt = 1.0 / 60.0, tick = dt / 10.0;
            for (int step = 0; step < 240; ++step) {
                // 速度と横滑りを時間で振って、いろいろな κ / α を通す。
                const double t = step * dt;
                ch.velocity = Vec3{0.8 * std::sin(t * 2.0), 0.0, -(2.0 + 10.0 * t)};
                const double omega = (2.0 + 10.0 * t) / tire.radius * (1.0 + 0.4 * std::sin(t * 5.0));
                for (WheelController* w : {&builtin, &interp, &jit}) {
                    w->updateContact(ch, 0.05 * std::sin(t), planeGroundQuery, 350.0, dt);
                }
                for (int k = 0; k < 10; ++k) {
                    const double b = builtin.tick(omega, tick);
                    const double i = interp.tick(omega, tick);
                    const double l = jit.tick(omega, tick);
                    maxDiffI = std::max(maxDiffI, std::fabs(b - i));
                    maxDiffL = std::max(maxDiffL, std::fabs(b - l));
                }
                for (WheelController* w : {&builtin, &interp, &jit}) w->finishStep(10);
                const Vec3 fb = builtin.tireForce(), fi = interp.tireForce(), fl = jit.tireForce();
                maxDiffI = std::max(maxDiffI, (fb - fi).length());
                maxDiffL = std::max(maxDiffL, (fb - fl).length());
            }
            std::printf("  max |builtin - interpreter| = %.3g, |builtin - luajit| = %.3g "
                        "(N or N·m)\n", maxDiffI, maxDiffL);
            check(maxDiffI < 1e-6, "interpreter matches the built-in tire model");
            check(maxDiffL < 1e-6, "luajit matches the built-in tire model");
            check(interp.formulaFailures() == 0 && jit.formulaFailures() == 0,
                  "no formula failures");
        }

        // 壊れたグラフ: 循環はエラー、未接続と未知の名前は警告。
        {
            FormulaGraphDesc g;
            g.name = "bad";
            g.nodes.push_back({1, "add", "", {}, 0, 0});
            g.nodes.push_back({2, "mul", "", {}, 0, 0});
            g.nodes.push_back({3, "out", "fx", {}, 0, 0});
            g.wires.push_back({1, 0, 2, 0});
            g.wires.push_back({2, 0, 1, 0});
            g.wires.push_back({2, 0, 3, 0});
            std::vector<std::string> e2, w2;
            auto bad = FormulaProgram::compile(g, tireFormulaInputs(), tireFormulaOutputs(), e2, w2);
            check(bad == nullptr && e2.size() == 1, "a cycle is a compile error");
            for (const auto& e : e2) std::printf("    error: %s\n", e.c_str());

            FormulaGraphDesc h;
            h.name = "loose";
            h.nodes.push_back({1, "in", "nosuch", {}, 0, 0});
            h.nodes.push_back({2, "add", "", {}, 0, 0});
            h.nodes.push_back({3, "out", "fx", {}, 0, 0});
            h.wires.push_back({1, 0, 2, 0});
            h.wires.push_back({2, 0, 3, 0});
            std::vector<std::string> e3, w3;
            auto loose = FormulaProgram::compile(h, tireFormulaInputs(), tireFormulaOutputs(), e3, w3);
            check(loose != nullptr && e3.empty() && w3.size() == 3,
                  "unknown input + unconnected port + unwritten output -> 3 warnings");
            for (const auto& w : w3) std::printf("    warning: %s\n", w.c_str());
            if (loose) {
                auto inst = loose->interpret();
                double in[20] = {0}, out[2] = {9, 9};
                check(inst->eval(in, out, 0.01) && out[0] == 0.0 && out[1] == 0.0,
                      "a loose graph still runs and outputs 0");
            }
        }

        // XML の往復。
        {
            const std::string text = wizengine::xml::write(formulaElement(graph));
            wizengine::xml::Element el;
            std::string err;
            check(wizengine::xml::parse(text, el, err), "formula xml parses back");
            std::vector<std::string> w4;
            const FormulaGraphDesc back = formulaFromXml(el, &w4);
            check(w4.empty() && back.nodes.size() == graph.nodes.size() &&
                      back.wires.size() == graph.wires.size() && back.name == graph.name,
                  "formula survives the xml round trip");
            bool same = true;
            for (std::size_t i = 0; i < back.nodes.size() && i < graph.nodes.size(); ++i) {
                if (back.nodes[i].kind != graph.nodes[i].kind ||
                    back.nodes[i].id != graph.nodes[i].id ||
                    back.nodes[i].name != graph.nodes[i].name) same = false;
            }
            check(same, "nodes keep id / type / name");
        }

        // ノード式を付けた車 vs 組み込みの車: 同じ入力で同じ走り。
        {
            VehicleDesc withFormula = desc;
            withFormula.formulas.push_back(graph);
            for (AxleDesc& a : withFormula.axles) a.tire.formula = graph.name;
            Sim ref(desc);
            Sim sim(withFormula);
            sim.model = VehicleModel(withFormula, lua.ok() ? &lua : nullptr);
            for (const auto& d : sim.model.diagnostics()) std::printf("    %s\n", d.c_str());
            ref.place(h);
            sim.place(h);
            VehicleInput in;
            in.throttle = 1.0;
            in.steer = 0.2;
            ref.model.setInput(in);
            sim.model.setInput(in);
            double maxPos = 0.0;
            for (int i = 0; i < 60 * 6; ++i) {
                ref.step();
                sim.step();
                maxPos = std::max(maxPos, (ref.body.pos - sim.body.pos).length());
            }
            std::printf("  formula car vs built-in car: max position difference %.3g m over 6 s "
                        "(v=%.2f m/s)\n", maxPos, sim.speed());
            check(maxPos < 1e-6, "formula-driven car follows the same path");
            check(sim.model.formulaFailures() == 0, "no formula failures while driving");
        }
    }

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
