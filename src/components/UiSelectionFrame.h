#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "scene/UiState.h"
#include "ui/UiLayout.h"      // Rect
#include "ui/UiMeshFormat.h"  // UiMeshData
#include "ui/UiTypes.h"

class Scene;
namespace wizengine {
class Renderer;
}

// エディタの選択枠（選んでいる要素の枠とつまみ、ui/UiHandles.h の形）を、エディタカメラの
// ビューにだけ出す専用の Canvas として描く（どの UI よりも手前。後処理なし）。持ち主は
// UiRenderComponent で、その 2 つの口から呼ばれる:
//   copyTarget（onRender = オブジェクトのロックの中）… 選択とエディタカメラを写すだけ。
//   update（onRenderUnlocked = ロックの外）         … 組んで Renderer へ送る（Scene は受けない =
//                                                     オブジェクト一覧を読むコードが書けない）。
// 板（camera / world）の上の要素の枠も同じ Canvas に描く: 板の設計の画素で組んだ枠を、
// そのフレームのカメラで映像の画素へ写す（Scene::plateMeshToOverlay。カメラの後ろに回った
// 部分は切る。手前の物にも板の中身にも隠れない）。組み直すのは条件（中身と置き場所の版・
// ビューの大きさ・カメラの視点と注視点）が変わったときだけ。
// 直すきっかけ: 選択枠の見た目・出す条件を変えるとき（UI の中身の描き方とは別）。
class UiSelectionFrame {
public:
    // 板の上の選択枠を映像に重ねる口（Scene::plateMeshToOverlay / plateDesignPerScreenPx /
    // cameraEyeTarget を包む）。
    struct Projection {
        std::function<bool(const wizengine::ui::CanvasDesc&, std::size_t,
                           const wizengine::ui::Rect&, wizengine::ui::UiMeshData&)>
            toOverlay;
        std::function<bool(const wizengine::ui::CanvasDesc&, std::size_t, int, int,
                           const wizengine::ui::Rect&, double&)>
            scale;
        std::function<std::array<double, 6>(std::size_t)> cameraPose;
    };
    // scene の口を包んだ Projection（scene は update の間だけ生きていればよい）。
    static Projection projectionOf(const Scene& scene);

    // 選択枠の対象を写す（onRender）: UI の編集を受け付けるとき（Scene::editsUiFrom）に、
    // 要素を選んでいれば。今もあるかは update が写しで確かめる。
    void copyTarget(Scene& scene);
    // 出すときは専用の Canvas をエディタカメラのビューに置いてメッシュを送り、出さないときは
    // どのビューにも置かない。条件（Key）が前回と同じなら何もしない。snap = UI の設計値の写し、
    // cameraViews = カメラ（スロット番号）→ ビュー番号（-1 = 無効・未定）。
    void update(wizengine::Renderer& renderer, const wizengine::UiState::Snapshot& snap,
                const std::vector<int>& cameraViews, const Projection& project);

private:
    // 選択枠のメッシュ（Canvas desc の要素 at、ビュー view）と、その Canvas の基準の高さ
    // （overlay は desc の高さ、板の枠は映像の画素 = ビューの高さ）。出せなければ false
    // （板がカメラの後ろ・板が作れない）。
    bool build(wizengine::Renderer& renderer, const wizengine::UiState::Snapshot& snap,
               const wizengine::ui::CanvasDesc& desc,
               const std::vector<wizengine::ui::ElementDesc>& elements, std::size_t at, int view,
               const Projection& project, wizengine::ui::UiMeshData& mesh,
               double& designHeight) const;
    // 選択枠の Canvas をどのビューにも置かない（作っていなければ何もしない）。
    void hide(wizengine::Renderer& renderer);

    // 最後に送った条件（全部同じなら組み直さない。出さないときは既定値のまま）。
    struct Key {
        std::uint64_t revision = 0;  // その Canvas の材料の版（Snapshot::contentsRevision）
        std::uint64_t placement = 0;  // その Canvas の置き場所だけの版（CanvasItem::placementRevision）
        int canvas = -1, element = -1, view = -1, viewWidth = 0, viewHeight = 0;
        // 板の枠だけ: カメラの視点と注視点（cameraEyeTarget。overlay の枠は 0 のまま）。
        std::array<double, 6> camera{};
        bool operator==(const Key& o) const {
            const bool sameDesign =
                revision == o.revision && placement == o.placement && canvas == o.canvas &&
                element == o.element;
            const bool sameView = view == o.view && viewWidth == o.viewWidth &&
                                  viewHeight == o.viewHeight;
            return sameDesign && sameView && camera == o.camera;
        }
    };

    // 対象（copyTarget が写す。-1 = 出さない = 選択が無い・エディタモードでない・プレハブ
    // 編集中）とエディタカメラのスロット番号。
    int canvas_ = -1, element_ = -1;
    int camera_ = 0;
    // 枠の Canvas（Renderer の番号。最初に出すときに作る）。
    bool created_ = false;
    std::size_t rendererId_ = 0;
    Key sent_;
    int view_ = -1;              // 枠を置いているビュー（-1 = どこにも置いていない）
    double designHeight_ = 0.0;  // 枠の Canvas に渡した基準の高さ
};
