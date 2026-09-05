#include "scene/PrefabDefaults.h"

#include <cmath>
#include <string>

namespace wizengine {
namespace editor {

namespace {
constexpr double kWindshieldTiltDeg = 35.0;  // フロントガラスの傾き（垂直から）
const Color3 kGlass{0.10f, 0.14f, 0.20f};
const Color3 kHeadlight{1.0f, 0.95f, 0.75f};
const Color3 kTaillight{0.85f, 0.03f, 0.03f};
const Color3 kTire{0.03f, 0.03f, 0.035f};
const Color3 kSpoke{0.75f, 0.75f, 0.78f};

PartDesc box(const std::string& name, double x, double y, double z, double sx, double sy,
             double sz, const Color3& c) {
    PartDesc p;
    p.name = name;
    p.kind = PartKind::Box;
    p.position = {x, y, z};
    p.size = {sx, sy, sz};
    p.color = c;
    return p;
}
}  // namespace

PrefabDesc builtinCarPrefab(const BodyDesc& body, const std::string& name) {
    PrefabDesc d;
    d.name = name;
    // 箱の車体だけに飾りを付ける（glTF の車体には自前のモデルがある前提）。
    if (body.shape == ShapeKind::Box) {
        const double sx = body.size.x, sy = body.size.y, sz = body.size.z;
        const double top = sy * 0.5;
        const double cabinH = sy * 0.9, cabinW = sx * 0.88, cabinL = sz * 0.42;
        const double cabinZ = sz * 0.12;  // 少し後ろ寄り（後ろ = +Z）
        const double cabinFront = cabinZ - cabinL * 0.5;
        d.parts.push_back(box("cabin", 0.0, top + cabinH * 0.5, cabinZ, cabinW, cabinH,
                              cabinL, kGlass));
        const double tilt = kWindshieldTiltDeg * 3.14159265358979323846 / 180.0;
        const double run = cabinH * std::tan(tilt);
        const double len = cabinH / std::cos(tilt);
        PartDesc glass = box("windshield", 0.0, top + cabinH * 0.5, cabinFront - run * 0.5,
                             cabinW * 0.98, len, 0.04, kGlass);
        glass.rotation = {kWindshieldTiltDeg, 0.0, 0.0};
        d.parts.push_back(glass);
        d.parts.push_back(box("cowl", 0.0, top + cabinH * 0.25, cabinFront - run * 0.25,
                              cabinW * 0.98, cabinH * 0.5, run * 0.5, kGlass));
        const double zFront = -sz * 0.5 - 0.015, zRear = sz * 0.5 + 0.015;
        d.parts.push_back(box("headlight_L", -sx * 0.33, sy * 0.15, zFront, sx * 0.22,
                              sy * 0.25, 0.03, kHeadlight));
        d.parts.push_back(box("headlight_R", sx * 0.33, sy * 0.15, zFront, sx * 0.22,
                              sy * 0.25, 0.03, kHeadlight));
        d.parts.push_back(box("taillight_L", -sx * 0.35, sy * 0.12, zRear, sx * 0.2,
                              sy * 0.2, 0.03, kTaillight));
        d.parts.push_back(box("taillight_R", sx * 0.35, sy * 0.12, zRear, sx * 0.2,
                              sy * 0.2, 0.03, kTaillight));
    }
    // 車輪: 軸ごとに左右。タイヤは円柱（軸 X）、スポークは十字の板 2 枚。
    if (body.hasVehicle) {
        for (std::size_t a = 0; a < body.vehicle.axles.size(); ++a) {
            const auto& axle = body.vehicle.axles[a];
            const double r = axle.tire.radius, w = axle.tire.width;
            for (int side = -1; side <= 1; side += 2) {
                const std::string suffix = std::to_string(a) + (side < 0 ? "L" : "R");
                const std::string socket =
                    "wheel:" + std::to_string(a) + (side < 0 ? ":L" : ":R");
                PartDesc tire;
                tire.name = "tire_" + suffix;
                tire.kind = PartKind::Cylinder;
                tire.size = {w, 2.0 * r, 2.0 * r};
                tire.color = kTire;
                tire.socket = socket;
                d.parts.push_back(tire);
                PartDesc spokeA = box("spoke_" + suffix + "_a", 0.0, 0.0, 0.0, w * 1.06,
                                      2.0 * r * 0.72, r * 0.16, kSpoke);
                spokeA.socket = socket;
                d.parts.push_back(spokeA);
                PartDesc spokeB = box("spoke_" + suffix + "_b", 0.0, 0.0, 0.0, w * 1.06,
                                      r * 0.16, 2.0 * r * 0.72, kSpoke);
                spokeB.socket = socket;
                d.parts.push_back(spokeB);
            }
        }
    }
    return d;
}

}  // namespace editor
}  // namespace wizengine
