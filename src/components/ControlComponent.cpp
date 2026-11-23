#include "components/ControlComponent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

#include "components/VehicleComponent.h"
#include "components/EffectsComponent.h"
#include "core/Log.h"
#include "scene/Scene.h"

namespace ctl = wizengine::control;
namespace ed = wizengine::editor;
namespace veh = wizengine::vehicle;
namespace fml = wizengine::formula;

namespace {

// 記録の上限（制御器ごと）。240 Hz で約 3.5 分。
constexpr std::size_t kMaxRows = 50000;

ctl::Vec3 toVec(const chrono::ChVector3d& v) { return ctl::Vec3{v.x(), v.y(), v.z()}; }
chrono::ChVector3d toChrono(const ctl::Vec3& v) { return chrono::ChVector3d(v.x, v.y, v.z); }

const char* sourceName(ctl::Source s) {
    switch (s) {
        case ctl::Source::Time: return "time";
        case ctl::Source::Body: return "body";
        case ctl::Source::Joint: return "joint";
        case ctl::Source::Vehicle: return "vehicle";
        case ctl::Source::Scope: return "scope";
        case ctl::Source::Param: return "param";
        case ctl::Source::Emitter: return "emitter";
        case ctl::Source::Var: return "var";
        case ctl::Source::Transmission: return "trans";
        case ctl::Source::Air: return "air";
    }
    return "?";
}

// air:<量> の名前 → 上書きの項目（書ける量。ControlSignals の airActuatorNames と同じ）。
bool airField(const std::string& q, wizengine::fluid::AirOverride::Field& out) {
    using F = wizengine::fluid::AirOverride;
    static const std::pair<const char*, F::Field> table[] = {
        {"density", F::Density},   {"temperature", F::Temperature}, {"humidity", F::Humidity},
        {"pressure", F::Pressure}, {"altitude", F::Altitude},       {"wind_x", F::WindX},
        {"wind_y", F::WindY},      {"wind_z", F::WindZ},            {"drag", F::Drag},
        {"lift", F::Lift},         {"magnus", F::Magnus},
    };
    for (const auto& e : table) {
        if (q == e.first) {
            out = e.second;
            return true;
        }
    }
    return false;
}

// air:<量> の今の値（ControlSignals の airSensorNames と同じ名前）。
double airQuantity(const wizengine::fluid::AirState& a, const std::string& q) {
    if (q == "density") return a.density;
    if (q == "temperature") return a.temperature;
    if (q == "humidity") return a.humidity;
    if (q == "pressure") return a.pressure;
    if (q == "local_pressure") return a.localPressure;
    if (q == "altitude") return a.altitude;
    if (q == "wind_x") return a.wind.x;
    if (q == "wind_y") return a.wind.y;
    if (q == "wind_z") return a.wind.z;
    if (q == "drag") return a.drag;
    if (q == "lift") return a.lift;
    if (q == "magnus") return a.magnus;
    return 0.0;
}

}  // namespace

ControlComponent::ControlComponent(VehicleComponent* vehicles) : vehicles_(vehicles) {
    if (fml::LuaRuntime::available()) {
        lua_ = std::make_unique<fml::LuaRuntime>();
        if (lua_->ok()) {
            LOGI("control", "node formulas: %s (jit %s)", lua_->version().c_str(),
                 lua_->jitEnabled() ? "on" : "OFF - formulas will run slowly");
        } else {
            LOGW("control", "node formulas: LuaJIT state could not be created - "
                            "using the C++ interpreter");
            lua_.reset();
        }
    } else {
        LOGI("control", "node formulas: built without LuaJIT - using the C++ interpreter");
    }
}

ControlComponent::~ControlComponent() = default;

void ControlComponent::onEditorStep(Scene& scene, double dt) {
    (void)dt;
    if (runtimes_.empty() && joints_.empty() && trans_.empty()) return;
    runtimes_.clear();
    joints_.clear();
    // AT の Chrono のシャフト網は止めたら外す（次のシミュレート開始で作り直す）。
    if (!trans_.empty()) scene.physics().removeAllShaftNetworks();
    trans_.clear();
    transVersion_ = ~std::uint64_t(0);
    vars_.clear();
    time_ = 0.0;
    // 次のシミュレート開始で必ず作り直す。
    generation_ = ~std::uint64_t(0);
    controlVersion_ = ~std::uint64_t(0);
    formulaVersion_ = ~std::uint64_t(0);
    publishStatus();
}

void ControlComponent::onPhysicsStep(Scene& scene, double dt) {
    const std::uint64_t gen = scene.runGeneration();
    if (gen != generation_) {
        // シミュレートの開始・Reset: 状態（積分・遅延・ジョイントの基準）を捨て、
        // 記録も新しい世代にする。
        generation_ = gen;
        runtimes_.clear();
        joints_.clear();
        trans_.clear();
        transVersion_ = ~std::uint64_t(0);
        vars_.clear();
        time_ = 0.0;
        controlVersion_ = ~std::uint64_t(0);
        std::lock_guard<std::mutex> lk(mutex_);
        series_.clear();
        ++run_;
    }
    // パラメータの値（スライダー）。変わったときだけ写す = 毎ステップ
    // EditorState のロックを取らない。式は作り直さない。
    const std::uint64_t pv = scene.editor().paramVersion();
    if (pv != paramVersion_) {
        paramVersion_ = pv;
        params_.clear();
        for (const auto& p : scene.editor().params()) params_[p.name] = p.value;
    }
    // AT: 設計値が変わったら作り直す。制御器の結び付き（AT の番号）も変わるので、
    // 制御器もコンパイルし直す（PID の状態は捨てる。AT の定義を変えたのだから妥当）。
    // 物理の系の作り直し・つないだ剛体の作り直しでも網は消えるので作り直す。
    const std::uint64_t tv = scene.editor().transmissionVersion();
    if (tv != transVersion_ || transmissionsStale(scene)) {
        transVersion_ = tv;
        rebuildTransmissions(scene);
        runtimes_.clear();
        controlVersion_ = ~std::uint64_t(0);
    }
    // 直前の積分の結果を読む（センサ trans:<名前>.<量> はこの値 = 前のステップの結果）。
    for (auto& t : trans_) {
        if (t.running) t.driveline->read(scene.physics(), dt);
    }
    const std::uint64_t cv = scene.editor().controlVersion();
    const std::uint64_t fv = scene.editor().formulaVersion();
    const std::uint64_t ev = scene.editor().emitterVersion();
    if (cv != controlVersion_ || fv != formulaVersion_ || ev != emitterVersion_) {
        controlVersion_ = cv;
        formulaVersion_ = fv;
        emitterVersion_ = ev;
        rebuild(scene);
    }
    if (runtimes_.empty()) {
        // 書く制御器が無い = 空気は文書の値（前に書いていた分を消す）。
        if (airSent_) {
            airWrites_ = wizengine::fluid::AirOverride{};
            scene.setAirOverride(airWrites_);
            airSent_ = false;
        }
        applyTransmissions(scene, dt);
        time_ += dt;
        return;
    }

    // ジョイントの基準: この世代で最初のステップ（= 置いた姿勢）で全部取る。
    // 途中で制御器を作り直しても、角度は開始時からのまま続く。
    if (joints_.empty()) {
        const auto joints = scene.editor().joints();
        jointEnds_.assign(joints.size(), std::make_pair(-1, -1));
        for (std::size_t j = 0; j < joints.size(); ++j) {
            jointEnds_[j] = std::make_pair(joints[j].bodyA, joints[j].bodyB);
            ctl::JointMeasure m;
            const ed::Vec3d& ax = joints[j].axis;
            m.begin(bodyState(scene, joints[j].bodyA), bodyState(scene, joints[j].bodyB),
                    ctl::Vec3{ax.x, ax.y, ax.z});
            joints_[int(j)] = m;
        }
    }
    jointResults_.clear();
    jointStatsFetched_ = false;
    airFetched_ = false;
    airWrites_ = wizengine::fluid::AirOverride{};

    bool executed = false;
    for (std::size_t r = 0; r < runtimes_.size(); ++r) {
        Runtime& rt = runtimes_[r];
        if (!rt.desc.enabled || !rt.instance) continue;
        rt.sinceRun += dt;
        const double period = rt.desc.period;
        const bool due = !rt.ran || period <= 0.0 || rt.sinceRun >= period - 1e-9;
        if (due) {
            // 評価の dt は前回の評価からの実際の経過（周期が物理の dt の倍数で
            // なくても積分が合う）。最初の 1 回は名目の周期。
            const double execDt = rt.ran ? rt.sinceRun : (period > 0.0 ? period : dt);
            for (std::size_t i = 0; i < rt.ins.size(); ++i) {
                rt.inVals[i] = rt.ins[i].ok ? readSensor(scene, rt.ins[i]) : 0.0;
            }
            std::vector<double> out(rt.outVals.size(), 0.0);
            if (rt.instance->eval(rt.inVals.data(), out.data(), execDt)) {
                rt.outVals = std::move(out);
            } else {
                // NaN・実行時エラー: 出力は前回のまま（暴れた値を Chrono へ渡さない）。
                if (rt.failures++ == 0) {
                    LOGW("control", "controller '%s': evaluation failed (NaN or runtime "
                                    "error) - holding the previous outputs",
                         rt.desc.formula.c_str());
                }
            }
            rt.sinceRun = 0.0;
            rt.ran = true;
            ++rt.execs;
            executed = true;
            // 記録（時刻 = このステップの始まり = センサを読んだ時刻）。
            std::vector<double> row;
            row.reserve(1 + rt.inVals.size() + rt.outVals.size());
            row.push_back(time_);
            row.insert(row.end(), rt.inVals.begin(), rt.inVals.end());
            row.insert(row.end(), rt.outVals.begin(), rt.outVals.end());
            std::lock_guard<std::mutex> lk(mutex_);
            if (r < series_.size()) {
                Series& s = series_[r];
                s.rows.emplace_back(++seq_, std::move(row));
                if (s.rows.size() > kMaxRows) s.rows.pop_front();
            }
        }
        applyOutputs(scene, rt, dt);
    }
    // 空気: このステップに書いた値（無ければ空 = 文書の値）。流体・車両の係はこの後に
    // 回るので、同じステップから効く。
    scene.setAirOverride(airWrites_);
    airSent_ = airWrites_.any();
    // AT: 制御器が今書いた入力で TCU を回し、このステップの荷重とクラッチの容量を与える
    // （積分はこの後 Chrono が剛体と一緒に）。
    applyTransmissions(scene, dt);
    // 式が書いたパラメータを EditorState へ（制御盤のスライダー・保存）。版が進むので
    // 次のステップで params_ を写し直す = 範囲で丸めた値がここへ戻る。
    if (!paramWrites_.empty()) {
        scene.editor().setParamValues(paramWrites_);
        paramWrites_.clear();
    }
    time_ += dt;
    // 評価回数・変数の値などの表示は実時間で 0.2 秒に 1 回（物理のレートに
    // よらない。以前の「64 ステップに 1 回」は 30 Hz のシーンで 2 秒おきだった）。
    if (executed || !trans_.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatus_ >= std::chrono::milliseconds(200)) {
            lastStatus_ = now;
            publishStatus();
        }
    }
}

// ---- 作り直し -----------------------------------------------------------------
void ControlComponent::rebuild(Scene& scene) {
    const std::vector<ed::ControllerDesc> list = scene.editor().controllers();
    const std::vector<fml::FormulaGraphDesc> formulas = scene.editor().formulaAssets();
    std::vector<Runtime> next;
    next.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        const ed::ControllerDesc& c = list[i];
        const fml::FormulaGraphDesc* g = nullptr;
        for (const auto& f : formulas) {
            if (f.name == c.formula) g = &f;
        }
        const std::string sig = g ? fml::formulaSignature(*g) : std::string();
        // 中身が同じなら実行状態ごと引き継ぐ（周期・有効だけの変更、ノードの移動）。
        auto same = std::find_if(runtimes_.begin(), runtimes_.end(), [&](const Runtime& rt) {
            return rt.desc.formula == c.formula && rt.signature == sig && !sig.empty();
        });
        if (same != runtimes_.end()) {
            Runtime rt = std::move(*same);
            rt.desc = c;
            next.push_back(std::move(rt));
            continue;
        }
        Runtime rt;
        rt.desc = c;
        rt.signature = sig;
        if (!g) {
            rt.errors.push_back("formula \"" + c.formula + "\" does not exist");
        } else {
            compile(scene, rt, formulas);
        }
        for (const auto& e : rt.errors) {
            LOGW("control", "controller '%s': %s", c.formula.c_str(), e.c_str());
        }
        for (const auto& w : rt.warnings) {
            LOGI("control", "controller '%s': %s", c.formula.c_str(), w.c_str());
        }
        if (rt.instance) {
            LOGI("control", "controller '%s': %zu inputs, %zu outputs, period %s (%s)",
                 c.formula.c_str(), rt.ins.size(), rt.outs.size(),
                 c.period > 0.0 ? (std::to_string(c.period) + " s").c_str() : "every step",
                 rt.instance->backend());
        }
        next.push_back(std::move(rt));
    }
    runtimes_ = std::move(next);
    {
        // 記録の列は制御器の並びと 1 対 1。列の名前が変わった制御器は記録を
        // やり直す（同じなら続ける = 周期を変えてもグラフが途切れない）。
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<Series> s(runtimes_.size());
        for (std::size_t i = 0; i < runtimes_.size(); ++i) {
            s[i].formula = runtimes_[i].desc.formula;
            s[i].names = runtimes_[i].inNames;
            s[i].names.insert(s[i].names.end(), runtimes_[i].outNames.begin(),
                              runtimes_[i].outNames.end());
            for (auto& old : series_) {
                if (old.formula == s[i].formula && old.names == s[i].names) {
                    s[i].rows = std::move(old.rows);
                    break;
                }
            }
        }
        series_ = std::move(s);
    }
    publishStatus();
}

void ControlComponent::compile(Scene& scene, Runtime& rt,
                               const std::vector<fml::FormulaGraphDesc>& formulas) {
    const fml::FormulaGraphDesc* g = nullptr;
    for (const auto& f : formulas) {
        if (f.name == rt.desc.formula) g = &f;
    }
    if (!g) return;
    // 入出力の名前はグラフの in / out ノードから（ノード id 順、重複なし）。
    std::vector<const fml::FormulaNodeDesc*> nodes;
    for (const auto& n : g->nodes) nodes.push_back(&n);
    std::sort(nodes.begin(), nodes.end(),
              [](const fml::FormulaNodeDesc* a, const fml::FormulaNodeDesc* b) {
                  return a->id < b->id;
              });
    for (const fml::FormulaNodeDesc* n : nodes) {
        if (n->name.empty()) {
            if (n->kind == "in" || n->kind == "out") {
                rt.warnings.push_back(n->kind + " node " + std::to_string(n->id) +
                                      " has no name - ignored");
            }
            continue;
        }
        if (n->kind != "in" && n->kind != "out") continue;
        std::vector<std::string>& list = n->kind == "in" ? rt.inNames : rt.outNames;
        if (std::find(list.begin(), list.end(), n->name) == list.end()) {
            list.push_back(n->name);
        }
    }
    rt.program = fml::FormulaProgram::compile(*g, rt.inNames, rt.outNames, rt.errors,
                                              rt.warnings);
    if (!rt.program) return;
    std::string err;
    if (lua_) rt.instance = lua_->instantiate(rt.program, err);
    if (!rt.instance) {
        if (lua_) rt.warnings.push_back("LuaJIT: " + err + " - using the interpreter");
        rt.instance = rt.program->interpret();
    }
    rt.ins.resize(rt.inNames.size());
    rt.outs.resize(rt.outNames.size());
    for (std::size_t i = 0; i < rt.inNames.size(); ++i) {
        std::string e;
        if (!ctl::parseSensor(rt.inNames[i], rt.ins[i].ref, e)) {
            rt.warnings.push_back(e + " - reads 0");
            continue;
        }
        bind(scene, rt, rt.ins[i], true);
    }
    for (std::size_t i = 0; i < rt.outNames.size(); ++i) {
        std::string e;
        if (!ctl::parseActuator(rt.outNames[i], rt.outs[i].ref, e)) {
            rt.warnings.push_back(e + " - ignored");
            continue;
        }
        bind(scene, rt, rt.outs[i], false);
    }
    rt.inVals.assign(rt.inNames.size(), 0.0);
    rt.outVals.assign(rt.outNames.size(), 0.0);
}

void ControlComponent::bind(Scene& scene, Runtime& rt, Binding& b, bool sensor) {
    const ctl::SignalRef& r = b.ref;
    const std::string label = std::string(sourceName(r.source)) + ":" + r.target;
    switch (r.source) {
        case ctl::Source::Time:
        case ctl::Source::Scope:
            b.ok = true;
            return;
        case ctl::Source::Param: {
            // 値は毎ステップ名前で引く（params_ の写し）。後からパラメータを
            // 足しても読めるように ok にしておき、今無いことだけ伝える。
            b.ok = true;
            bool found = false;
            for (const auto& p : scene.editor().params()) found = found || p.name == r.target;
            if (!found) {
                rt.warnings.push_back(label + ": no parameter with that name - " +
                                      (sensor ? "reads 0" : "ignored") +
                                      " (add <param name=\"" + r.target + "\"> in <control>)");
            }
            return;
        }
        case ctl::Source::Var:
            b.ok = true;  // 宣言は要らない（最初に書くまでは 0）
            return;
        case ctl::Source::Air:
            b.ok = true;  // シーンに 1 つ（量の名前は parse で確かめ済み）
            return;
        case ctl::Source::Transmission: {
            for (std::size_t i = 0; i < trans_.size(); ++i) {
                if (trans_[i].name != r.target) continue;
                if (!trans_[i].running) {
                    rt.warnings.push_back(label + ": the transmission is not running (" +
                                          trans_[i].error + ") - " +
                                          (sensor ? "reads 0" : "ignored"));
                    return;
                }
                namespace at = wizengine::transmission;
                const at::AtDesc& td = trans_[i].driveline->desc();
                const int q = sensor ? at::atSensorId(td, r.sub) : at::atInputId(r.sub);
                if (q < 0) {
                    std::string list;
                    const auto names = sensor ? at::atSensorNames(td) : at::atInputNames();
                    for (const auto& n : names) list += (list.empty() ? "" : " ") + n;
                    rt.warnings.push_back(label + "." + r.sub + ": unknown " +
                                          (sensor ? "sensor" : "input") + " (" + list + ")");
                    return;
                }
                b.trans = int(i);
                b.transQty = q;
                b.ok = true;
                return;
            }
            rt.warnings.push_back(label + ": no transmission with that name - " +
                                  (sensor ? "reads 0" : "ignored") +
                                  " (add <transmission name=\"" + r.target + "\"> in <control>)");
            return;
        }
        case ctl::Source::Body:
        case ctl::Source::Vehicle: {
            // 物理スレッドはオブジェクト一覧の唯一の書き手なのでロックは要らない。
            int found = -1, count = 0;
            for (std::size_t i = 0; i < scene.objectCount(); ++i) {
                const GameObject& o = scene.object(i);
                if (!o.alive || o.desc.name != r.target) continue;
                if (found < 0) found = int(i);
                ++count;
            }
            if (found < 0) {
                rt.warnings.push_back(label + ": no object with that name - " +
                                      (sensor ? "reads 0" : "ignored"));
                return;
            }
            if (count > 1) {
                rt.warnings.push_back(label + ": " + std::to_string(count) +
                                      " objects share the name - using #" +
                                      std::to_string(found));
            }
            if (r.source == ctl::Source::Vehicle && !scene.object(std::size_t(found)).desc.hasVehicle) {
                rt.warnings.push_back(label + ": the object is not a vehicle - " +
                                      (sensor ? "reads 0" : "ignored"));
                return;
            }
            if (r.source == ctl::Source::Vehicle && !vehicles_) return;
            b.object = found;
            b.ok = true;
            return;
        }
        case ctl::Source::Emitter: {
            const auto emitters = scene.editor().emitters();
            for (std::size_t i = 0; i < emitters.size(); ++i) {
                if (emitters[i].name != r.target) continue;
                b.emitter = int(i);
                b.ok = effects_ != nullptr;
                return;
            }
            rt.warnings.push_back(label + ": no emitter with that name - ignored "
                                  "(add <emitter name=\"" + r.target + "\"> in <worldbody>)");
            return;
        }
        case ctl::Source::Joint: {
            const auto joints = scene.editor().joints();
            for (std::size_t j = 0; j < joints.size(); ++j) {
                if (joints[j].name != r.target) continue;
                b.joint = int(j);
                b.ok = true;
                if (!sensor && joints[j].motor == ed::MotorMode::None) {
                    rt.warnings.push_back(label + ": the joint has no motor - "
                                          "set motor=\"speed|position|force\" to drive it");
                    b.ok = false;
                }
                return;
            }
            rt.warnings.push_back(label + ": no joint with that name - " +
                                  (sensor ? "reads 0" : "ignored"));
            return;
        }
    }
}

// ---- センサ -------------------------------------------------------------------
ctl::BodyState ControlComponent::bodyState(Scene& scene, int object) {
    ctl::BodyState s;  // 地面（-1）・消えた物は単位姿勢で静止
    if (object < 0 || std::size_t(object) >= scene.objectCount()) return s;
    const GameObject& o = scene.object(std::size_t(object));
    if (!o.alive || o.physId == GameObject::kInvalidId) return s;
    PhysicsWorld& physics = scene.physics();
    const BodyTransform t = physics.bodyTransform(o.physId);
    s.p = ctl::Vec3{t.px, t.py, t.pz};
    s.q = ctl::Quat{t.qw, t.qx, t.qy, t.qz};
    s.v = toVec(physics.bodyVelocity(o.physId));
    s.w = toVec(physics.bodyAngularVelocity(o.physId));
    return s;
}

double ControlComponent::readSensor(Scene& scene, const Binding& b) {
    const ctl::SignalRef& r = b.ref;
    switch (r.source) {
        case ctl::Source::Time: return time_;
        case ctl::Source::Body: return ctl::bodyQuantity(r.quantity, bodyState(scene, b.object));
        case ctl::Source::Joint: {
            if (r.quantity == ctl::Quantity::Force || r.quantity == ctl::Quantity::Torque) {
                if (!jointStatsFetched_) {
                    jointStatsFetched_ = true;
                    const auto stats = scene.jointStats();
                    jointForce_.assign(stats.size(), 0.0);
                    jointTorque_.assign(stats.size(), 0.0);
                    for (std::size_t i = 0; i < stats.size(); ++i) {
                        jointForce_[i] = stats[i].force;
                        jointTorque_[i] = stats[i].torque;
                    }
                }
                const auto& v = r.quantity == ctl::Quantity::Force ? jointForce_ : jointTorque_;
                return std::size_t(b.joint) < v.size() ? v[std::size_t(b.joint)] : 0.0;
            }
            auto res = jointResults_.find(b.joint);
            if (res == jointResults_.end()) {
                const auto m = joints_.find(b.joint);
                if (m == joints_.end() || std::size_t(b.joint) >= jointEnds_.size()) return 0.0;
                const auto& ends = jointEnds_[std::size_t(b.joint)];
                res = jointResults_
                          .emplace(b.joint, m->second.measure(bodyState(scene, ends.first),
                                                              bodyState(scene, ends.second)))
                          .first;
            }
            switch (r.quantity) {
                case ctl::Quantity::Angle: return res->second.angle;
                case ctl::Quantity::Rate: return res->second.rate;
                case ctl::Quantity::Pos: return res->second.pos;
                case ctl::Quantity::Vel: return res->second.vel;
                default: return 0.0;
            }
        }
        case ctl::Source::Vehicle: {
            veh::VehicleTelemetry tm;
            if (!vehicles_ || !vehicles_->telemetry(std::size_t(b.object), tm)) return 0.0;
            switch (r.quantity) {
                case ctl::Quantity::VehSpeed: return tm.speed;
                case ctl::Quantity::Rpm: return tm.rpm;
                case ctl::Quantity::Gear: return double(tm.gear);
                default: return 0.0;
            }
        }
        case ctl::Source::Transmission:
            if (std::size_t(b.trans) >= trans_.size() || !trans_[std::size_t(b.trans)].running) {
                return 0.0;
            }
            return wizengine::transmission::atSensor(trans_[std::size_t(b.trans)].driveline->readout(),
                                                     b.transQty);
        case ctl::Source::Scope: return 0.0;
        case ctl::Source::Emitter: return 0.0;
        case ctl::Source::Param: {
            const auto it = params_.find(r.target);
            return it == params_.end() ? 0.0 : it->second;
        }
        case ctl::Source::Var: {
            const auto it = vars_.find(r.target);
            return it == vars_.end() ? 0.0 : it->second;
        }
        case ctl::Source::Air:
            // ステップの始めの空気（前のステップに書いた上書き込み）。
            if (!airFetched_) {
                airFetched_ = true;
                airState_ = scene.airNow();
            }
            return airQuantity(airState_, r.sub);
    }
    return 0.0;
}

// ---- アクチュエータ -----------------------------------------------------------
void ControlComponent::applyOutputs(Scene& scene, Runtime& rt, double dt) {
    PhysicsWorld& physics = scene.physics();
    struct Load {
        ctl::Vec3 force, torque;
    };
    std::map<int, Load> loads;
    struct Drive {
        int mask = 0;
        veh::VehicleInput in;
    };
    std::map<int, Drive> drives;
    for (std::size_t i = 0; i < rt.outs.size(); ++i) {
        Binding& b = rt.outs[i];
        const double v = rt.outVals[i];
        if (!b.ok || !std::isfinite(v)) continue;
        switch (b.ref.source) {
            case ctl::Source::Joint: {
                // 単位は SI のまま（文書の度 → rad の換算はしない。制御の信号は rad）。
                const std::size_t phys = scene.jointPhysId(std::size_t(b.joint));
                if (phys == PhysicsWorld::kInvalidJoint) break;
                if (b.applied && b.lastApplied == v) break;  // 同じ値は渡し直さない
                if (physics.setJointMotorTarget(phys, v)) {
                    b.applied = true;
                    b.lastApplied = v;
                }
                break;
            }
            case ctl::Source::Body: {
                Load& l = loads[b.object];
                const ctl::BodyState s = bodyState(scene, b.object);
                auto local = [&](double x, double y, double z) {
                    return ctl::rotate(s.q, ctl::Vec3{x, y, z});
                };
                switch (b.ref.quantity) {
                    case ctl::Quantity::Fx: l.force.x += v; break;
                    case ctl::Quantity::Fy: l.force.y += v; break;
                    case ctl::Quantity::Fz: l.force.z += v; break;
                    case ctl::Quantity::Tx: l.torque.x += v; break;
                    case ctl::Quantity::Ty: l.torque.y += v; break;
                    case ctl::Quantity::Tz: l.torque.z += v; break;
                    case ctl::Quantity::LFx: l.force = l.force + local(v, 0, 0); break;
                    case ctl::Quantity::LFy: l.force = l.force + local(0, v, 0); break;
                    case ctl::Quantity::LFz: l.force = l.force + local(0, 0, v); break;
                    case ctl::Quantity::LTx: l.torque = l.torque + local(v, 0, 0); break;
                    case ctl::Quantity::LTy: l.torque = l.torque + local(0, v, 0); break;
                    case ctl::Quantity::LTz: l.torque = l.torque + local(0, 0, v); break;
                    default: break;
                }
                break;
            }
            case ctl::Source::Emitter:
                // 見た目だけ（パーティクルの量）。毎ステップ書く = ゼロ次ホールド。
                if (effects_) effects_->setRate(b.emitter, v);
                break;
            case ctl::Source::Transmission:
                // 入力は保持（次に書くまで）。AT はこのステップの終わりに進む。
                if (std::size_t(b.trans) < trans_.size() && trans_[std::size_t(b.trans)].driveline) {
                    wizengine::transmission::atSetInput(
                        trans_[std::size_t(b.trans)].driveline->inputs(), b.transQty, v);
                }
                break;
            case ctl::Source::Var:
                // 表へすぐ書く = 後の制御器が同じステップで読む。
                vars_[b.ref.target] = v;
                break;
            case ctl::Source::Air: {
                // ステップの終わりにまとめて Scene へ（後の制御器が勝つ）。範囲は
                // resolveAir が丸める。
                wizengine::fluid::AirOverride::Field f;
                if (airField(b.ref.sub, f)) airWrites_.set(f, v);
                break;
            }
            case ctl::Source::Param:
                // 値が変わったときだけ書く（同じ値を出し続けている間はスライダーで
                // 上書きできる）。後の制御器が同じステップで読めるよう写しにも入れる。
                if (b.applied && b.lastApplied == v) break;
                b.applied = true;
                b.lastApplied = v;
                params_[b.ref.target] = v;
                paramWrites_[b.ref.target] = v;
                break;
            case ctl::Source::Vehicle: {
                Drive& d = drives[b.object];
                switch (b.ref.quantity) {
                    case ctl::Quantity::Throttle:
                        d.mask |= VehicleComponent::kDriveThrottle;
                        d.in.throttle = std::max(0.0, std::min(1.0, v));
                        break;
                    case ctl::Quantity::Brake:
                        d.mask |= VehicleComponent::kDriveBrake;
                        d.in.brake = std::max(0.0, std::min(1.0, v));
                        break;
                    case ctl::Quantity::Steer:
                        d.mask |= VehicleComponent::kDriveSteer;
                        d.in.steer = std::max(-1.0, std::min(1.0, v));
                        break;
                    case ctl::Quantity::Handbrake:
                        d.mask |= VehicleComponent::kDriveHandbrake;
                        d.in.handbrake = std::max(0.0, std::min(1.0, v));
                        break;
                    default: break;
                }
                break;
            }
            default: break;
        }
    }
    for (const auto& kv : loads) {
        const GameObject& o = scene.object(std::size_t(kv.first));
        if (!o.alive || o.physId == GameObject::kInvalidId) continue;
        const Load& l = kv.second;
        if (ctl::length(l.force) > 0.0) physics.applyForce(o.physId, toChrono(l.force), dt);
        if (ctl::length(l.torque) > 0.0) {
            // トルクの口は無いので、重心の両側に逆向きの力を掛ける（合力 0）。
            const ctl::BodyState s = bodyState(scene, kv.first);
            ctl::Vec3 offset, f;
            ctl::torqueAsCouple(l.torque, 0.5, offset, f);
            physics.applyForceAtPoint(o.physId, toChrono(f), toChrono(s.p + offset), dt);
            physics.applyForceAtPoint(o.physId, toChrono(f * -1.0), toChrono(s.p - offset), dt);
        }
    }
    if (vehicles_) {
        for (const auto& kv : drives) {
            vehicles_->setControlDrive(std::size_t(kv.first), kv.second.mask, kv.second.in);
        }
    }
}

// ---- AT ------------------------------------------------------------------------
void ControlComponent::rebuildTransmissions(Scene& scene) {
    namespace at = wizengine::transmission;
    PhysicsWorld& physics = scene.physics();
    // 前の網は外す（系を作り直した後なら一覧はもう空 = 何もしない）。
    physics.removeAllShaftNetworks();
    trans_.clear();
    transSerial_ = physics.systemSerial();
    for (const auto& d : scene.editor().transmissions()) {
        TransRuntime t;
        t.name = d.name;
        t.driveline = std::make_unique<AtDriveline>(d);
        const at::AtKinematics& kin = t.driveline->kinematics();
        for (const auto& e : kin.errors()) {
            LOGW("control", "transmission '%s': %s", d.name.c_str(), e.c_str());
        }
        for (const auto& w : kin.warnings()) {
            LOGI("control", "transmission '%s': %s", d.name.c_str(), w.c_str());
        }
        if (!kin.ok()) {
            t.error = kin.errors().empty() ? "invalid transmission" : kin.errors().front();
            trans_.push_back(std::move(t));
            continue;
        }
        // input = body: つなぐ剛体（名前で引く）。回転軸はワールドの向きを、今の姿勢で
        // 剛体のローカルへ直す。エンジン軸の初めの回転は剛体の今の回転。
        std::size_t bodyPhys = static_cast<std::size_t>(-1);
        double axisLocal[3] = {0.0, 0.0, 1.0};
        double w0 = d.engine.idle * 3.14159265358979323846 / 30.0;
        if (d.input != at::InputMode::Torque) w0 = 0.0;
        if (d.input == at::InputMode::Body) {
            for (std::size_t i = 0; i < scene.objectCount(); ++i) {
                const GameObject& o = scene.object(i);
                if (!o.alive || o.desc.name != d.engine.body) continue;
                t.bodyObject = int(i);
                bodyPhys = o.physId;
                break;
            }
            if (t.bodyObject >= 0 && bodyPhys != GameObject::kInvalidId) {
                const ctl::BodyState bs = bodyState(scene, t.bodyObject);
                const ctl::Vec3 a{d.engine.axis[0], d.engine.axis[1], d.engine.axis[2]};
                const ctl::Vec3 l = ctl::rotate(ctl::conj(bs.q), a);
                axisLocal[0] = l.x;
                axisLocal[1] = l.y;
                axisLocal[2] = l.z;
                w0 = bs.w.x * a.x + bs.w.y * a.y + bs.w.z * a.z;
            } else {
                bodyPhys = static_cast<std::size_t>(-1);
            }
        }
        t.bodyPhys = bodyPhys;
        std::string err;
        t.running = t.driveline->build(physics, bodyPhys, axisLocal, w0, err);
        if (!t.running) {
            t.error = err;
            LOGW("control", "transmission '%s': %s", d.name.c_str(), err.c_str());
        } else {
            std::string ratios;
            const auto& gears = kin.gears();
            for (std::size_t g = 0; g < gears.size(); ++g) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), "%s%s %.3f", ratios.empty() ? "" : ", ",
                              d.gears[g].name.c_str(), gears[g].ratio);
                ratios += buf;
            }
            LOGI("control", "transmission '%s': Chrono shaft network, input=%s load=%s, %zu gears "
                            "(%s)",
                 d.name.c_str(), at::inputModeName(d.input), at::loadModeName(d.load),
                 gears.size(), ratios.c_str());
            // Chrono のトルコンとねじりばねは陽的な力なので刻みが粗いと発散し、APGD は
            // シャフトのクラッチ（箱型の拘束）を解けない（AtDriveline の確認で 120 Hz・
            // APGD とも発散、200 Hz 以上の bb / psor で安定）。
            const double hz = scene.physicsHz() * std::max(1, scene.substeps());
            if (hz < 199.0) {
                LOGW("control", "transmission '%s': the physics runs at %.0f Hz - the shaft "
                                "network needs 200 Hz or more (raise <option rate> or substeps)",
                     d.name.c_str(), hz);
            }
            const std::string& solver = physics.solverName();
            if (solver == "apgd" || solver == "jacobi") {
                LOGW("control", "transmission '%s': solver '%s' does not handle the shaft "
                                "clutches well - use bb or psor",
                     d.name.c_str(), solver.c_str());
            }
        }
        trans_.push_back(std::move(t));
    }
}

bool ControlComponent::transmissionsStale(Scene& scene) const {
    if (trans_.empty()) return false;
    if (scene.physics().systemSerial() != transSerial_) return true;
    for (const auto& t : trans_) {
        if (t.bodyObject < 0) continue;
        if (std::size_t(t.bodyObject) >= scene.objectCount()) return true;
        const GameObject& o = scene.object(std::size_t(t.bodyObject));
        if (!o.alive || o.physId != t.bodyPhys) return true;
    }
    return false;
}

void ControlComponent::applyTransmissions(Scene& scene, double dt) {
    for (auto& t : trans_) {
        if (t.running) t.driveline->apply(scene.physics(), dt);
    }
}

// ---- HTTP ---------------------------------------------------------------------
void ControlComponent::publishStatus() {
    nlohmann::json list = nlohmann::json::array();
    for (const Runtime& rt : runtimes_) {
        nlohmann::json c;
        c["formula"] = rt.desc.formula;
        c["period"] = rt.desc.period;
        c["enabled"] = rt.desc.enabled;
        c["ok"] = rt.instance != nullptr;
        c["backend"] = rt.instance ? rt.instance->backend() : "";
        c["inputs"] = rt.inNames;
        c["outputs"] = rt.outNames;
        c["errors"] = rt.errors;
        c["warnings"] = rt.warnings;
        c["execs"] = rt.execs;
        c["failures"] = rt.failures;
        c["last"] = nlohmann::json::array();
        for (double v : rt.outVals) c["last"].push_back(std::isfinite(v) ? v : 0.0);
        list.push_back(std::move(c));
    }
    nlohmann::json vars = nlohmann::json::object();
    for (const auto& kv : vars_) vars[kv.first] = std::isfinite(kv.second) ? kv.second : 0.0;
    nlohmann::json trans = nlohmann::json::array();
    for (const auto& t : trans_) {
        nlohmann::json e;
        e["name"] = t.name;
        e["ok"] = t.running;
        e["error"] = t.error;
        if (t.running) {
            const auto& tm = t.driveline->readout();
            auto fin = [](double v) { return std::isfinite(v) ? v : 0.0; };
            e["gear"] = tm.gear;
            e["targetGear"] = tm.targetGear;
            e["shifting"] = tm.shifting;
            e["engineRpm"] = fin(tm.engineRpm);
            e["outputRpm"] = fin(tm.outputRpm);
            e["speedKmh"] = fin(tm.speed * 3.6);
            e["lockup"] = fin(tm.lockup);
            e["outputTorque"] = fin(tm.outputTorque);
        }
        trans.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lk(mutex_);
    status_ = nlohmann::json::object();
    status_["running"] = !runtimes_.empty();
    status_["controllers"] = std::move(list);
    status_["vars"] = std::move(vars);
    status_["transmissions"] = std::move(trans);
}

nlohmann::json ControlComponent::statusJson() const {
    std::lock_guard<std::mutex> lk(mutex_);
    nlohmann::json j = status_;
    j["run"] = run_;
    j["seq"] = seq_;
    return j;
}

std::string ControlComponent::samplesJson(std::uint64_t since, std::size_t maxRows) const {
    std::lock_guard<std::mutex> lk(mutex_);
    nlohmann::json j;
    j["run"] = run_;
    // since が未来（サーバーを再起動した等）なら最初から送る。
    if (since > seq_) since = 0;
    // seq の小さい順に最大 maxRows 行（複数の制御器の行を通し番号で混ぜる）。
    // 送った最後の番号を seq に返す = 次はそこから。
    std::vector<std::size_t> pos(series_.size(), 0);
    for (std::size_t s = 0; s < series_.size(); ++s) {
        const auto& rows = series_[s].rows;
        pos[s] = std::size_t(std::upper_bound(rows.begin(), rows.end(), since,
                                              [](std::uint64_t v, const auto& row) {
                                                  return v < row.first;
                                              }) -
                             rows.begin());
    }
    std::vector<nlohmann::json> out(series_.size(), nlohmann::json::array());
    std::uint64_t last = since;
    for (std::size_t n = 0; n < maxRows; ++n) {
        std::size_t best = series_.size();
        for (std::size_t s = 0; s < series_.size(); ++s) {
            if (pos[s] >= series_[s].rows.size()) continue;
            if (best == series_.size() ||
                series_[s].rows[pos[s]].first < series_[best].rows[pos[best]].first) {
                best = s;
            }
        }
        if (best == series_.size()) break;
        const auto& row = series_[best].rows[pos[best]++];
        nlohmann::json r = nlohmann::json::array();
        for (double v : row.second) r.push_back(std::isfinite(v) ? v : 0.0);
        out[best].push_back(std::move(r));
        last = row.first;
    }
    j["seq"] = last;
    j["series"] = nlohmann::json::array();
    for (std::size_t s = 0; s < series_.size(); ++s) {
        nlohmann::json e;
        e["formula"] = series_[s].formula;
        e["names"] = series_[s].names;
        e["rows"] = std::move(out[s]);
        j["series"].push_back(std::move(e));
    }
    return j.dump();
}

std::string ControlComponent::samplesCsv(const std::string& formula) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const Series* s = nullptr;
    for (const auto& c : series_) {
        if (formula.empty() || c.formula == formula) {
            s = &c;
            break;
        }
    }
    if (!s) return "";
    std::string out = "time";
    for (const auto& n : s->names) out += "," + n;
    out += "\n";
    char buf[48];
    for (const auto& row : s->rows) {
        for (std::size_t i = 0; i < row.second.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s%.9g", i ? "," : "", row.second[i]);
            out += buf;
        }
        out += "\n";
    }
    return out;
}
