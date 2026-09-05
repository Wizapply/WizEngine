#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "document/EditorTypes.h"
#include "scene/SceneComponent.h"

class VehicleComponent;

// プレハブ（見た目の部品の集合）を描く SceneComponent。RENDER スレッド専用。
//
// オブジェクトごとに「どのプレハブを描くか」を決め、部品ぶんの形状スロット
// （箱・球・円柱）と glTF 実体を持つ:
//   * BodyDesc::prefab が付いていればそのアセット
//   * 付いていない車両は組み込みのクルマの見た目（PrefabDefaults.h）を
//     暗黙のプレハブとして描く（保存には出ない）
// 部品の姿勢は「描いている車体の姿勢（Scene::latestPose）× ソケット × 部品の
// ローカル」。ソケット（車輪）の姿勢は VehicleComponent が物理スレッドから
// 渡すスナップショット（シミュレート中は実際の縮み・舵・回転、エディタ中は
// 設計値）を使う。編集はプレハブ編集モード（EditorState::prefabEditObject）
// で、ギズモが部品を動かす（GizmoComponent）。
class PrefabComponent : public SceneComponent {
public:
    explicit PrefabComponent(VehicleComponent* vehicle) : vehicle_(vehicle) {}

    void onRender(Scene& scene) override;

private:
    struct Slot {
        std::size_t id = 0;
        bool model = false;  // glTF 実体（true）か形状スロット（false）か
    };
    struct Instance {
        std::string key;  // 何を描いているか（プレハブ名 + 版、または組み込みの寸法）
        std::vector<Slot> slots;
    };
    void release(Scene& scene, Instance& inst);

    VehicleComponent* vehicle_;
    std::map<std::size_t, Instance> instances_;  // キーはオブジェクト番号
    // プレハブアセットのキャッシュ（版が変わったときだけ取り直す）。
    std::uint64_t cachedVersion_ = ~std::uint64_t(0);
    std::vector<wizengine::editor::PrefabDesc> cache_;
};
