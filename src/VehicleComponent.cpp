#include "VehicleComponent.h"

#include <cmath>
#include <string>

#include "EditorTypes.h"
#include "Log.h"
#include "Scene.h"

namespace veh = wizengine::vehicle;

namespace {

veh::Vec3 toVec(const chrono::ChVector3d& v) {
    return veh::Vec3{v.x(), v.y(), v.z()};
}
chrono::ChVector3d toChrono(const veh::Vec3& v) {
    return chrono::ChVector3d(v.x, v.y, v.z);
}

}  // namespace

VehicleComponent::VehicleComponent() {
    if (veh::LuaRuntime::available()) {
        lua_ = std::make_unique<veh::LuaRuntime>();
        if (lua_->ok()) {
            LOGI("vehicle", "node formulas: %s (jit %s)", lua_->version().c_str(),
                 lua_->jitEnabled() ? "on" : "OFF - formulas will run slowly");
        } else {
            LOGW("vehicle", "node formulas: LuaJIT state could not be created - "
                            "using the C++ interpreter");
            lua_.reset();
        }
    } else {
        LOGI("vehicle", "node formulas: built without LuaJIT - using the C++ interpreter");
    }
}

bool VehicleComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    (void)scene;
    (void)camIndex;
    const std::string cmd = msg.value("cmd", "");
    if (cmd != "drive") return false;
    // /input は誰でも叩けるので型を信じない（jsonNumber は型違いで既定値）。
    throttle_.store(wizengine::editor::jsonNumber(msg, "throttle", 0.0));
    brake_.store(wizengine::editor::jsonNumber(msg, "brake", 0.0));
    steer_.store(wizengine::editor::jsonNumber(msg, "steer", 0.0));
    handbrake_.store(wizengine::editor::jsonNumber(msg, "handbrake", 0.0));
    return true;
}

void VehicleComponent::onEditorStep(Scene& scene, double dt) {
    (void)dt;
    if (!models_.empty()) {
        models_.clear();
        LOGI("vehicle", "runtime cleared (editor mode)");
    }
    count_.store(0);
    // 設計値から「止まっている車」の車輪位置。毎パス作り直しても数本の
    // ベクタなので安い（置き直し・XML の適用にそのまま追従する）。
    std::map<std::size_t, std::vector<veh::WheelLocalPose>> vis;
    for (std::size_t i = 0; i < scene.objectCount(); ++i) {
        const GameObject& obj = scene.object(i);
        if (!obj.alive || !obj.desc.hasVehicle) continue;
        vis[i] = veh::VehicleModel::designWheelPoses(obj.desc.vehicle, obj.desc.mass);
    }
    std::lock_guard<std::mutex> lk(visMutex_);
    visuals_.swap(vis);
}

void VehicleComponent::onPhysicsStep(Scene& scene, double dt) {
    PhysicsWorld& physics = scene.physics();
    veh::VehicleInput input;
    input.throttle = throttle_.load();
    input.brake = brake_.load();
    input.steer = steer_.load();
    input.handbrake = handbrake_.load();

    // 計算式（またはタイヤの参照）が変わっていたら作り直す。式のコンパイルは
    // 生成時なので、これがノードエディタの編集を実行へ届ける唯一の経路。
    const std::uint64_t fv = scene.editor().formulaVersion();
    if (fv != formulaVersion_) {
        if (!models_.empty()) LOGI("vehicle", "formulas changed - rebuilding vehicles");
        models_.clear();
        reportedFailures_.clear();
        formulaVersion_ = fv;
    }
    std::vector<veh::FormulaGraphDesc> library;  // 生成するときだけ取る
    bool libraryLoaded = false;

    int count = 0;
    bool first = true;
    std::map<std::size_t, std::vector<veh::WheelLocalPose>> vis;
    // 物理スレッドはオブジェクト一覧の唯一の書き手なのでロックは要らない。
    for (std::size_t i = 0; i < scene.objectCount(); ++i) {
        GameObject& obj = scene.object(i);
        if (!obj.alive || !obj.desc.hasVehicle ||
            obj.physId == GameObject::kInvalidId) {
            continue;
        }
        if (obj.desc.fixed) {
            // 固定した車体は走らせないが、車輪は設計位置に出しておく。
            vis[i] = veh::VehicleModel::designWheelPoses(obj.desc.vehicle,
                                                         obj.desc.mass);
            continue;
        }
        auto it = models_.find(i);
        if (it == models_.end()) {
            if (!libraryLoaded) {
                library = scene.editor().formulaAssets();
                libraryLoaded = true;
            }
            it = models_.emplace(i, std::make_unique<veh::VehicleModel>(
                                        obj.desc.vehicle, lua_.get(), &library))
                     .first;
            LOGI("vehicle", "object %zu '%s': vehicle created (%zu wheels, %d ticks)",
                 i, obj.desc.name.c_str(), it->second->wheelCount(),
                 it->second->desc().ticks);
            for (const std::string& d : it->second->diagnostics()) {
                LOGI("vehicle", "  %s", d.c_str());
            }
            reportedFailures_[i] = 0;
        }
        veh::VehicleModel& model = *it->second;
        if (const int fails = model.formulaFailures(); fails > reportedFailures_[i]) {
            // 式が NaN や実行時エラーを出したら 1 回だけ知らせる（毎ステップ
            // 出すとログが埋まる）。そのティックは組み込みで代用している。
            if (reportedFailures_[i] == 0) {
                LOGW("vehicle", "object %zu '%s': tire formula failed (%d ticks so far) - "
                                "falling back to the built-in tire model for those ticks",
                     i, obj.desc.name.c_str(), fails);
            }
            reportedFailures_[i] = fails;
        }
        model.setInput(input);

        const BodyTransform t = physics.bodyTransform(obj.physId);
        veh::ChassisState chassis;
        chassis.position = veh::Vec3{t.px, t.py, t.pz};
        chassis.rotation = veh::Quat{t.qw, t.qx, t.qy, t.qz};
        chassis.velocity = toVec(physics.bodyVelocity(obj.physId));
        chassis.angularVelocity = toVec(physics.bodyAngularVelocity(obj.physId));
        chassis.mass = physics.bodyMass(obj.physId);

        const auto& forces = model.step(chassis, dt, veh::planeGroundQuery);
        for (const veh::ForceAtPoint& f : forces) {
            physics.applyForceAtPoint(obj.physId, toChrono(f.force),
                                      toChrono(f.point), dt);
        }
        vis[i] = model.wheelLocalPoses();
        ++count;
        if (first) {
            const veh::VehicleTelemetry& tm = model.telemetry();
            rpm_.store(tm.rpm);
            gear_.store(tm.gear);
            speed_.store(tm.speed);
            clutch_.store(tm.clutch);
            first = false;
        }
    }
    // 消えたオブジェクトの実行状態を片付ける。
    for (auto it = models_.begin(); it != models_.end();) {
        const std::size_t i = it->first;
        if (!scene.objectAlive(i) || !scene.object(i).desc.hasVehicle) {
            reportedFailures_.erase(i);
            it = models_.erase(it);
        } else {
            ++it;
        }
    }
    count_.store(count);
    std::lock_guard<std::mutex> lk(visMutex_);
    visuals_.swap(vis);
}

bool VehicleComponent::wheelLocalPoses(std::size_t objectIndex,
                                       std::vector<veh::WheelLocalPose>& out) {
    std::lock_guard<std::mutex> lk(visMutex_);
    const auto it = visuals_.find(objectIndex);
    if (it == visuals_.end()) return false;
    out = it->second;
    return true;
}

VehicleComponent::Snapshot VehicleComponent::snapshot() const {
    Snapshot s;
    s.count = count_.load();
    s.present = s.count > 0;
    s.rpm = rpm_.load();
    s.gear = gear_.load();
    s.speed = speed_.load();
    s.clutch = clutch_.load();
    return s;
}
