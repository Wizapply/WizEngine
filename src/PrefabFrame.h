#pragma once

#include "EditorTypes.h"
#include "scene_math.h"
#include "vehicle/VehicleModel.h"

// プレハブの部品の「親フレーム」と、ワールド ⇄ ローカルの変換。
//
// 部品は付け先のオブジェクト（車体）に固定された見た目の子で、socket を
// 持つ部品はさらに車輪のフレーム（取り付け点から下がった車輪中心）に
// ぶら下がる。ここは**設計値**（エディタ中の姿勢 = BodyDesc の位置・回転、
// 車輪は静的な沈み込み）だけで計算する。ギズモ（ドラッグの適用）と
// ピック（部品の中心の投影）と描画の「エディタ中」がこれを共有するので、
// 同じ部品が別の場所に見えることがない。実行中（シミュレート）の車輪は
// VehicleComponent のスナップショットが正で、そちらは PrefabComponent が使う。
namespace prefabframe {

// 親フレーム（ワールド）。socket が車輪ならその車輪の中心・向き。
inline void parentFrame(const wizengine::editor::BodyDesc& body,
                        const wizengine::editor::PartDesc& part, scenemath::Vec3& p,
                        scenemath::Quat& q) {
    q = scenemath::quatFromEulerDegrees(body.rotation.x, body.rotation.y, body.rotation.z);
    p = scenemath::Vec3(body.position.x, body.position.y, body.position.z);
    int axle = 0, side = 0;
    if (body.hasVehicle && wizengine::editor::parseWheelSocket(part.socket, axle, side)) {
        const auto poses = wizengine::vehicle::VehicleModel::designWheelPoses(body.vehicle,
                                                                             body.mass);
        const std::size_t idx = std::size_t(axle) * 2 + (side > 0 ? 1 : 0);
        if (idx < poses.size()) {
            const auto& w = poses[idx];
            const scenemath::Vec3 local(w.attach.x, w.attach.y - w.drop, w.attach.z);
            p = p + q * local;
        }
    }
}

// 部品のワールド姿勢（位置と四元数）。
inline void partWorld(const wizengine::editor::BodyDesc& body,
                      const wizengine::editor::PartDesc& part, scenemath::Vec3& pos,
                      scenemath::Quat& rot) {
    scenemath::Vec3 p;
    scenemath::Quat q;
    parentFrame(body, part, p, q);
    const scenemath::Vec3 local(part.position.x, part.position.y, part.position.z);
    pos = p + q * local;
    rot = q * scenemath::quatFromEulerDegrees(part.rotation.x, part.rotation.y,
                                              part.rotation.z);
}

// ワールドの姿勢を部品のローカル（親フレーム基準）へ戻す。
inline void worldToLocal(const wizengine::editor::BodyDesc& body,
                         const wizengine::editor::PartDesc& part,
                         const scenemath::Vec3& worldPos, const scenemath::Quat& worldRot,
                         wizengine::editor::Vec3d& localPos,
                         wizengine::editor::Vec3d& localRotDeg) {
    scenemath::Vec3 p;
    scenemath::Quat q;
    parentFrame(body, part, p, q);
    const scenemath::Quat inv = q.inverse();
    const scenemath::Vec3 lp = inv * (worldPos - p);
    localPos = {lp.x(), lp.y(), lp.z()};
    const scenemath::Vec3 e = scenemath::eulerDegreesFromQuat(inv * worldRot);
    localRotDeg = {e.x(), e.y(), e.z()};
}

}  // namespace prefabframe
