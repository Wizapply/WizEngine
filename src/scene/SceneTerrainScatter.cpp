// テレインの散布物（木や小物）のサーバー側の操作（PHYSICS スレッド）: 原型の編集
// （edit.terrain の scatter）。
// 値の検査と JSON は SceneInternal.h（sanitizeScatter / scatterToJson /
// prototypePatchFromJson）、.terrain との読み書きは SceneTerrainFile.cpp、散布物の
// 差し替えそのもの（ロック・ファイルの dirty）は SceneTerrain.cpp の
// publishTerrainField に任せる。
#include "scene/SceneInternal.h"

using namespace scene_detail;

namespace wt = wizengine::terrain;

// ---- 原型 ---------------------------------------------------------------------

void Scene::editTerrainPrototype(int index, const nlohmann::json& patch) {
    const std::shared_ptr<const wt::HeightField> field = terrainField();
    if (!field) {
        editor_.setStatus("テレインがありません（原型を変えられません）");
        return;
    }
    const std::size_t i = std::size_t(index);
    const std::string label = terrainPrototypeLabel(i);
    const std::shared_ptr<const wt::TerrainScatter> now = terrainScatter();
    wt::TerrainScatter next = *now;
    next.prototypes[i] = prototypePatchFromJson(patch, next.prototypes[i]);
    // 検査で消されたパスは適用しない（黙って代役にしない）。
    const std::string asked = next.prototypes[i].file;
    sanitizeScatter(next, "browser");
    if (next.prototypes[i].file != asked) {
        editor_.setStatus(label + " のモデルは " + terrainPathRuleText() + "で指定してください");
        return;
    }
    if (next == *now) {
        editor_.setStatus(label + " は変わっていません");
        return;
    }
    // 原型は Undo の対象にしない（筆のストロークも閉じない）。ステータスは publish の前。
    editor_.setStatus(label + " を更新（保存すると .terrain に書きます）");
    publishTerrainField(terrain_, field, TerrainHistory::Keep, TerrainFileState::Dirty, {},
                        false, nullptr, TerrainChanged::Both,
                        std::make_shared<const wt::TerrainScatter>(std::move(next)));
}
