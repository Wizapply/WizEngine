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
// 板（camera / world）の上の要素の枠も同じ Canvas に描く: 板の設計の画素で組んだ枠の頂点を、
// そのフレームのカメラで映像の画素へ写す（Scene::plateToOverlay。手前の物にも板の中身にも
// 隠れない。カメラが動けば毎フレーム写し直す）。
// 直すきっかけ: 選択枠の見た目・出す条件を変えるとき（UI の中身の描き方とは別）。
class UiSelectionFrame {
public:
    // 板の上の選択枠を映像に重ねる口（Scene::plateToOverlay / plateDesignPerScreenPx を包む）。
    struct Projection {
        std::function<bool(const wizengine::ui::CanvasDesc&, std::size_t,
                           const wizengine::ui::Rect&, std::vector<std::array<double, 2>>&)>
            toOverlay;
        std::function<bool(const wizengine::ui::CanvasDesc&, std::size_t, int, int, double,
                           double, double&)>
            scale;
    };
    // scene の口を包んだ Projection（scene は update の間だけ生きていればよい）。
    static Projection projectionOf(const Scene& scene);

    // 選択枠の対象を写す（onRender）: UI の編集を受け付けるとき（Scene::editsUiFrom）に、
    // 要素を選んでいれば。今もあるかは update が写しで確かめる。
    void copyTarget(Scene& scene);
    // 出すときは専用の Canvas をエディタカメラのビューに置いてメッシュを送り、出さないときは
    // どのビューにも置かない。overlay の枠は条件が前回と同じなら何もしない（板の枠は毎フレーム
    // 写し直す。同じ中身は Renderer が送らない）。snap = UI の設計値の写し、cameraViews =
    // カメラ（スロット番号）→ ビュー番号（-1 = 無効・未定）。
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
        int canvas = -1, element = -1, view = -1, viewWidth = 0, viewHeight = 0;
        bool operator==(const Key& o) const {
            return revision == o.revision && canvas == o.canvas && element == o.element &&
                   view == o.view && viewWidth == o.viewWidth && viewHeight == o.viewHeight;
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
