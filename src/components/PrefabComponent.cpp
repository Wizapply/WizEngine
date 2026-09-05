#include "components/PrefabComponent.h"

#include <cmath>

#include <math/mat4.h>
#include <math/vec3.h>

#include "core/Log.h"
#include "scene/PrefabDefaults.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "components/VehicleComponent.h"
#include "scene/MathBridge.h"
#include "scene/SceneMath.h"
#include "vehicle/VehicleModel.h"

namespace ed = wizengine::editor;
namespace veh = wizengine::vehicle;

namespace {

// 部品ごとの見た目のスケール（単位メッシュに掛ける）。
filament::math::float3 partScale(const ed::PartDesc& p, float meshScale) {
    using filament::math::float3;
    switch (p.kind) {
        case ed::PartKind::Sphere: return float3{float(p.size.x)};
        case ed::PartKind::Cylinder:
            return float3{float(p.size.x), float(p.size.y), float(p.size.y)};
        case ed::PartKind::Mesh: return float3{float(p.size.x) * meshScale};
        case ed::PartKind::Box: break;
    }
    return float3{float(p.size.x), float(p.size.y), float(p.size.z)};
}

filament::math::mat4f rotationEuler(const ed::Vec3d& deg) {
    // scene_math と同じ順序（R = Rz * Ry * Rx）。
    const scenemath::Quat q = scenemath::quatFromEulerDegrees(deg.x, deg.y, deg.z);
    BodyTransform t{0, 0, 0, q.w(), q.x(), q.y(), q.z()};
    return toFilament(t);
}

}  // namespace

void PrefabComponent::release(Scene& scene, Instance& inst) {
    wizengine::Renderer& r = scene.renderer();
    for (const Slot& s : inst.slots) {
        if (s.model) r.releaseModelInstance(s.id);
        else r.removeShape(s.id);
    }
    inst.slots.clear();
    inst.key.clear();
}

void PrefabComponent::onRender(Scene& scene) {
    using filament::math::float3;
    using filament::math::mat4f;
    wizengine::Renderer& r = scene.renderer();
    EditorState& ed_ = scene.editor();

    // プレハブアセットは版が変わったときだけ取り直す（毎フレームのコピーと
    // ロックを避ける）。
    const std::uint64_t ver = ed_.prefabVersion();
    if (ver != cachedVersion_) {
        cache_ = ed_.prefabAssets();
        cachedVersion_ = ver;
    }

    // 消えたオブジェクトの実体を片付ける。
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (!scene.objectAlive(it->first)) {
            release(scene, it->second);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }

    for (std::size_t i = 0; i < scene.objectCount(); ++i) {
        if (!scene.objectAlive(i)) continue;
        const ed::BodyDesc& desc = scene.object(i).desc;

        // どのプレハブを描くか。
        const ed::PrefabDesc* prefab = nullptr;
        ed::PrefabDesc builtin;
        std::string key;
        if (!desc.prefab.empty()) {
            for (const auto& p : cache_) {
                if (p.name == desc.prefab) prefab = &p;
            }
            if (prefab) key = "asset:" + desc.prefab + "#" + std::to_string(ver);
        } else if (desc.hasVehicle) {
            builtin = ed::builtinCarPrefab(desc);
            prefab = &builtin;
            // 寸法や軸数が変わったら作り直す（部品の数・種類が変わる）。
            key = "builtin:" + std::to_string(desc.size.x) + "," +
                  std::to_string(desc.size.y) + "," + std::to_string(desc.size.z) + "," +
                  std::to_string(desc.vehicle.axles.size()) + "," +
                  ed::shapeName(desc.shape);
            for (const auto& a : desc.vehicle.axles) {
                key += ";" + std::to_string(a.tire.radius) + "/" + std::to_string(a.tire.width);
            }
        }
        auto it = instances_.find(i);
        if (!prefab) {
            if (it != instances_.end()) {
                release(scene, it->second);
                instances_.erase(it);
            }
            continue;
        }
        BodyTransform pose;
        if (!scene.latestPose(i, pose)) continue;  // 置いた直後のフレーム

        if (it == instances_.end()) it = instances_.emplace(i, Instance{}).first;
        Instance& inst = it->second;
        if (inst.key != key) {
            release(scene, inst);
            for (const ed::PartDesc& p : prefab->parts) {
                Slot s;
                if (p.kind == ed::PartKind::Mesh) {
                    const int mi = scene.meshIndexFor(p.mesh);
                    const std::size_t model = scene.meshModelId(mi);
                    if (model != GameObject::kInvalidId) {
                        s.id = r.addModelInstance(model);
                        s.model = true;
                    }
                }
                if (!s.model) {
                    wizengine::ShapeMesh mesh = wizengine::ShapeMesh::Box;
                    if (p.kind == ed::PartKind::Sphere || p.kind == ed::PartKind::Mesh) {
                        mesh = wizengine::ShapeMesh::Sphere;  // 読めないモデルは球で代役
                    } else if (p.kind == ed::PartKind::Cylinder) {
                        mesh = wizengine::ShapeMesh::Cylinder;
                    }
                    s.id = r.addShape(mesh);
                    r.setShapeColor(s.id, {p.color.r, p.color.g, p.color.b});
                }
                inst.slots.push_back(s);
            }
            inst.key = key;
        }

        // 車輪のソケット姿勢（車両なら VehicleComponent のスナップショット）。
        std::vector<veh::WheelLocalPose> wheels;
        bool haveWheels = false;
        if (desc.hasVehicle && vehicle_) haveWheels = vehicle_->wheelLocalPoses(i, wheels);
        if (desc.hasVehicle && !haveWheels) {
            wheels = veh::VehicleModel::designWheelPoses(desc.vehicle, desc.mass);
        }

        const mat4f chassis = toFilament(pose);
        for (std::size_t k = 0; k < prefab->parts.size() && k < inst.slots.size(); ++k) {
            const ed::PartDesc& p = prefab->parts[k];
            mat4f parent = chassis;
            int axle = 0, side = 0;
            if (ed::parseWheelSocket(p.socket, axle, side)) {
                const std::size_t wi = std::size_t(axle) * 2 + (side > 0 ? 1 : 0);
                if (wi < wheels.size()) {
                    const veh::WheelLocalPose& w = wheels[wi];
                    parent = chassis *
                             mat4f::translation(float3{float(w.attach.x),
                                                       float(w.attach.y - w.drop),
                                                       float(w.attach.z)}) *
                             mat4f::rotation(float(w.steerRad), float3{0.0f, 1.0f, 0.0f}) *
                             mat4f::rotation(float(-w.spinAngle), float3{1.0f, 0.0f, 0.0f});
                }
            }
            float meshScale = 1.0f;
            if (inst.slots[k].model) {
                const int mi = scene.meshIndexFor(p.mesh);
                if (mi >= 0) meshScale = float(scene.meshScale(mi));
            }
            const mat4f m =
                parent *
                mat4f::translation(float3{float(p.position.x), float(p.position.y),
                                          float(p.position.z)}) *
                rotationEuler(p.rotation) * mat4f::scaling(partScale(p, meshScale));
            if (inst.slots[k].model) r.setModelInstanceTransform(inst.slots[k].id, m);
            else r.setBoxTransform(inst.slots[k].id, m);
        }
    }
}
