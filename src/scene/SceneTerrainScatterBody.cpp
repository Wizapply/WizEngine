// テレインの散布物の幹の当たり判定（PHYSICS スレッド）: 当たる幹（立てた円柱）を
// 全部、1 個の固定ボディの複合形状にする。呼ぶきっかけ（何が変わったら組み直すか）は
// SceneTerrain.cpp の publishTerrainField だけが決める。「どの配置に幹があるか」と
// 幹の寸法は terrain の scatterCollidingPrototype / scatterTrunk が決める。
#include "scene/SceneInternal.h"

#include <chrono>
#include <vector>

using namespace chrono;
using namespace scene_detail;

namespace wt = wizengine::terrain;

namespace {

// 当たる幹を、格子のローカル座標の円柱（軸 X の約束 = size は長さ・直径・直径）へ。
// 足元が穴・格子の外の配置は入らない（配置のデータは残るので、埋めれば戻る）。
std::vector<PhysicsWorld::ExtraShape> trunkShapes(const wt::HeightField& field,
                                                  const wt::TerrainScatter& scatter) {
    const ChQuaternion<> upright = toChrono(wt::scatterTrunkUpright());
    std::vector<PhysicsWorld::ExtraShape> shapes;
    for (const wt::ScatterInstance& inst : scatter.instances) {
        const wt::ScatterPrototype* proto = wt::scatterCollidingPrototype(scatter, inst);
        if (!proto) continue;
        wt::ScatterTrunk trunk;
        if (!wt::scatterTrunk(field, *proto, inst, trunk)) continue;
        PhysicsWorld::ExtraShape s;
        s.cylinder = true;
        s.size = ChVector3d(trunk.height, 2.0 * trunk.radius, 2.0 * trunk.radius);
        s.pos = ChVector3d(trunk.x, trunk.centerY(), trunk.z);
        s.rot = upright;
        shapes.push_back(s);
    }
    return shapes;
}

}  // namespace

void Scene::releaseScatterBody() {
    if (scatterPhysId_ == PhysicsWorld::kInvalidId) return;
    // 番号は退場の後で手放す: 投げたら次の組み直しがもう一度退場させる（幹を二重にしない）。
    physics_.disableBody(scatterPhysId_);
    scatterPhysId_ = PhysicsWorld::kInvalidId;
}

void Scene::rebuildScatterBody() {
    const auto failed = [this](const char* why) {
        LOGE("scene", "scatter collision: building the trunks failed (%s) - the trees are "
                      "visible but nothing collides with them", why);
        editor_.setStatus(std::string("散布物の幹の当たり判定を作れませんでした（") + why +
                          "）- 見た目だけ残します");
    };
    try {
        const auto t0 = std::chrono::steady_clock::now();
        const bool had = scatterPhysId_ != PhysicsWorld::kInvalidId;
        releaseScatterBody();
        BodyTransform pose;
        std::shared_ptr<const wt::TerrainScatter> scatter;
        const std::shared_ptr<const wt::HeightField> field =
            copyTerrainFieldAndPose(pose, &scatter);
        std::vector<PhysicsWorld::ExtraShape> trunks;
        if (field && field->valid() && scatter) trunks = trunkShapes(*field, *scatter);
        if (!trunks.empty()) {
            scatterPhysId_ = physics_.addFrame(1.0, chronoPos(pose), toChrono(pose),
                                               /*fixed*/ true, trunks, BodyOptions{});
            // 接触はテレインの番号で報告（イベントの衝突では地面と同じ扱い）。テレインの
            // 接触を張れないときは setAlias が番号を見て何もしない。
            physics_.setAlias(scatterPhysId_, terrainPhysId_);
            LOGI("scene", "scatter collision: %zu trunks (%zu placements) built in %.1f ms "
                          "(body #%zu)",
                 trunks.size(), scatter->instances.size(), elapsedMs(t0), scatterPhysId_);
        }
        // シミュレート中に幹が増減した: 眠った物は接触でしか起きないので全部起こす。
        if ((had || !trunks.empty()) && editor_.mode() == ed::AppMode::Simulate) {
            physics_.wakeAll();
        }
    } catch (const std::exception& e) {
        failed(e.what());
    } catch (...) {
        failed("unknown exception");
    }
}

void Scene::placeScatterBody(const BodyTransform& pose) {
    if (scatterPhysId_ == PhysicsWorld::kInvalidId) return;
    physics_.placeBody(scatterPhysId_, chronoPos(pose), toChrono(pose));
}
