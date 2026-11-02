#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <math/mat4.h>

#include "scene/SceneComponent.h"
#include "terrain/HeightField.h"
#include "terrain/TerrainScatter.h"

namespace wizengine {
class Renderer;
}

// テレインの散布物（.terrain の TREE = 原型と配置）を描く SceneComponent。
// RENDER スレッド専用（onRender だけを使う）。
//
// 配置 1 つにスロット 1 つ: 原型のモデル（glTF の実体）か、モデルが無い・読め
// ないときの代役（円柱の幹 + 球の冠の形状スロット 2 つ）。姿勢は「テレインの
// 姿勢の変換（テレイン本体と同じ toFilament(pose)）× 地表の足元 × 向き（Y 軸）
// × 倍率」で、幹の寸法は terrain::scatterTrunk（幹の当たり判定と同じ値）、
// モデルの倍率は terrain::scatterModelScale。足元が穴なら隠す。
//
// 変化はポインタと変換の比較で知る: Scene::terrainFieldAndScatter で格子・姿勢・
// 散布物を組で取り、格子と散布物のポインタと姿勢の変換が前回と同じなら何もしない
// （散布物と格子は差し替えるたびに新しく作られる）。applyToRenderer はオブジェクト一覧のロックを持った
// まま onRender を呼ぶので、ここで取るのは terrainFieldMutex_ の末端ロックだけ。
//
// 実体は配置の数（kMaxScatterInstances まで）だけ持ち、減ったら余りを返す。
// 終了時の後始末は Renderer の破棄に任せる（PrefabComponent と同じ）。
class ScatterComponent : public SceneComponent {
public:
    void onRender(Scene& scene) override;

private:
    struct Slot {
        // 描きたいモデル（Renderer::loadModel の番号。kInvalidModel = 代役）。
        // 原型のモデルが変わったかはこれで比べる。
        std::size_t wanted = 0;
        bool model = false;     // id がモデルの実体（true）か代役の幹（false）か
        std::size_t id = 0;     // モデルの実体 / 代役の幹（円柱）の形状スロット
        std::size_t crown = 0;  // 代役の冠（球）の形状スロット
    };

    // モデルのパス → モデルの番号（読めなかったパスは kInvalidModel で覚える =
    // 同じパスは読み直さず、警告もパスごとに 1 回）。
    std::size_t modelFor(wizengine::Renderer& r, const std::string& file);
    Slot makeSlot(wizengine::Renderer& r, std::size_t wanted);
    void release(wizengine::Renderer& r, const Slot& s);
    void releaseAll(wizengine::Renderer& r);
    void hide(wizengine::Renderer& r, const Slot& s);
    // スロット s を配置 inst の姿勢に置く（足元が穴・格子の外なら隠す）。
    void place(wizengine::Renderer& r, const Slot& s, const wizengine::terrain::HeightField& field,
               const wizengine::terrain::ScatterPrototype& proto,
               const wizengine::terrain::ScatterInstance& inst,
               const filament::math::mat4f& frame);

    std::vector<Slot> slots_;  // 番号 = 配置の番号
    std::map<std::string, std::size_t> models_;
    // 前回描いた組（変化の検出用）。
    std::shared_ptr<const wizengine::terrain::TerrainScatter> drawnScatter_;
    std::shared_ptr<const wizengine::terrain::HeightField> drawnField_;
    filament::math::mat4f drawnTransform_;  // テレインの姿勢の変換（単位で始まる）
};
