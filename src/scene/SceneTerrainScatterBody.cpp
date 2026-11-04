// テレインの散布物の幹の当たり判定（PHYSICS スレッド）: 当たる幹（立てた円柱）を
// 全部、1 個の固定ボディの複合形状にする。呼ぶきっかけ（何が変わったら組み直すか）は
// SceneTerrain.cpp の publishTerrainField だけが決める（一部の書き換えが幹の足元に
// 掛かるかの判定 terrainShapeTouchesTrunks はここ）。幹の形が前回と同じならボディは
// そのまま。「どの配置に幹があるか」と幹の寸法は terrain の
// scatterCollidingPrototype / scatterTrunk が決める。
#include "scene/SceneInternal.h"

#include <chrono>
#include <utility>
#include <vector>

using namespace chrono;
using namespace scene_detail;

namespace wt = wizengine::terrain;

namespace {

// 当たる幹の一覧。足元が穴・格子の外の配置は入らない（配置のデータは残るので、
// 埋めれば戻る）。
std::vector<wt::ScatterTrunk> collectTrunks(const wt::HeightField& field,
                                            const wt::TerrainScatter& scatter) {
    std::vector<wt::ScatterTrunk> trunks;
    for (const wt::ScatterInstance& inst : scatter.instances) {
        const wt::ScatterPrototype* proto = wt::scatterCollidingPrototype(scatter, inst);
        if (!proto) continue;
        wt::ScatterTrunk trunk;
        if (!wt::scatterTrunk(field, *proto, inst, trunk)) continue;
        trunks.push_back(trunk);
    }
    return trunks;
}

// 幹を、格子のローカル座標の円柱（軸 X の約束 = size は長さ・直径・直径）へ。
std::vector<PhysicsWorld::ExtraShape> toExtraShapes(const std::vector<wt::ScatterTrunk>& trunks) {
    const ChQuaternion<> upright = toChrono(wt::scatterTrunkUpright());
    std::vector<PhysicsWorld::ExtraShape> shapes;
    shapes.reserve(trunks.size());
    for (const wt::ScatterTrunk& trunk : trunks) {
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

void Scene::forgetScatterBody() {
    scatterPhysId_ = PhysicsWorld::kInvalidId;
    scatterTrunks_.clear();
}

void Scene::releaseScatterBody() {
    if (scatterPhysId_ == PhysicsWorld::kInvalidId) return;
    // 番号は退場の後で手放す: disableBody が投げたら番号と控えが残る。無効の印
    // （bodyActive）を立てる前に投げたなら、次が違う形のときだけ退場をやり直す（同じ形なら
    // kept）。立てた後（系から外す所）に投げたなら次は必ずここへ来て、disableBody はすぐ
    // 戻り、番号を手放すだけになる（系から外す処理はやり直さない。当たり判定は切れて
    // 地面の下へ退避済み）。
    physics_.disableBody(scatterPhysId_);
    forgetScatterBody();
}

bool Scene::terrainShapeTouchesTrunks(const wt::GridRect& cells) const {
    // terrainField_ / terrainScatter_ を書くのはこのスレッドだけ（読むのにロック不要）。
    if (!terrainField_ || !terrainScatter_) return false;
    return wt::scatterTouches(*terrainScatter_, *terrainField_, cells);
}

void Scene::rebuildScatterBody() {
    const auto failed = [this](const char* why) {
        LOGE("scene", "scatter collision: building the trunks failed (%s) - the trees are "
                      "visible but nothing collides with them", why);
        editor_.setStatus(std::string("散布物の幹の当たり判定を作り直せませんでした（") + why +
                          "）- 見た目だけ残します");
    };
    // 系の作り直しと、前回 addFrame が投げた後は番号が無効なので、比較を飛ばして必ず組む。
    const bool had = scatterPhysId_ != PhysicsWorld::kInvalidId;
    bool removed = false, built = false;
    try {
        const auto t0 = std::chrono::steady_clock::now();
        BodyTransform pose;
        std::shared_ptr<const wt::TerrainScatter> scatter;
        const std::shared_ptr<const wt::HeightField> field =
            copyTerrainFieldAndPose(pose, &scatter);
        std::vector<wt::ScatterTrunk> trunks;
        if (field && field->valid() && scatter) trunks = collectTrunks(*field, *scatter);
        // 形が前回と同じ: 退場ボディも wakeAll も増やさない。テレインの姿勢は比べない
        // （移動は placeScatterBody の経路）。退場の途中で投げて無効になったボディは、
        // 同じ形でも当たらないので残さず、組み直しへ進む。
        if (had && physics_.bodyActive(scatterPhysId_) && trunks == scatterTrunks_) {
            LOGI("scene", "scatter collision: %zu trunks unchanged - body #%zu kept",
                 trunks.size(), scatterPhysId_);
            return;
        }
        releaseScatterBody();
        removed = had;
        if (!trunks.empty()) {
            scatterPhysId_ = physics_.addFrame(1.0, chronoPos(pose), toChrono(pose),
                                               /*fixed*/ true, toExtraShapes(trunks), BodyOptions{},
                                               /*terrainContact*/ false);
            // 接触はテレインの番号で報告（イベントの衝突では地面と同じ扱い）。テレインの
            // 接触を張れないときは setAlias が番号を見て何もしない。
            physics_.setAlias(scatterPhysId_, terrainPhysId_);
            scatterTrunks_ = std::move(trunks);
            built = true;
            LOGI("scene", "scatter collision: %zu trunks (%zu placements) built in %.1f ms "
                          "(body #%zu)",
                 scatterTrunks_.size(), scatter->instances.size(), elapsedMs(t0), scatterPhysId_);
        }
    } catch (const std::exception& e) {
        failed(e.what());
    } catch (...) {
        failed("unknown exception");
    }
    // シミュレート中に幹が増減した: 眠った物は接触でしか起きないので全部起こす。
    // 例外で組めなかったときも、古い幹を外したなら起こす（try の外に置く理由）。
    if ((removed || built) && editor_.mode() == ed::AppMode::Simulate) {
        physics_.wakeAll();
    }
}

void Scene::placeScatterBody(const BodyTransform& pose) {
    if (scatterPhysId_ == PhysicsWorld::kInvalidId) return;
    physics_.placeBody(scatterPhysId_, chronoPos(pose), toChrono(pose));
}
