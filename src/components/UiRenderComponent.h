#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "scene/SceneComponent.h"
#include "scene/UiState.h"
#include "ui/FontAtlas.h"

// UI（overlay の Canvas）を描画側へ反映する（RENDER スレッドの onRender だけ）。
//
// UiState の設計値は版（copyIfChanged）で写し、写しを読むだけ（UiState の内部は
// 持たない）。Canvas × ビューごとにメッシュを組んで Renderer へ送る。毎フレームの
// 仕事は「版・そのビューの縦横比・字の置き場の世代」のどれかが変わったときだけ。
// 字の置き場（FontAtlas）とフォント・画像のテクスチャの持ち主もここ（RENDER 専用）。
class UiRenderComponent : public SceneComponent {
public:
    UiRenderComponent();
    ~UiRenderComponent() override;
    void onRender(Scene& scene) override;

private:
    // Canvas 1 枚の、あるビューで最後に送った組み立ての条件。
    struct ViewBuild {
        std::uint64_t version = 0;
        double canvasWidth = -1.0;  // canvasRect の幅（= 基準の高さ × 縦横比）
        int generation = -1;        // 字の置き場の世代
    };
    struct CanvasSlot {
        std::size_t rendererId = 0;
        std::vector<std::size_t> views;            // setUiCanvasViews に渡した一覧
        std::map<std::size_t, ViewBuild> built;    // ビュー → 送った条件
    };

    void ensureAtlas(Scene& scene);
    // 版が変わったとき: 宣言のフォントを読み（ファイルごとに 1 回）、名前の対応を引き直す。
    void syncFonts();
    // 版が変わったとき: どの生きている Canvas も使わなくなった画像を返し、読めなかった
    // 記録を捨てる（次の組み立てでもう一度試す）。
    void releaseUnusedImages(Scene& scene);
    int imageTexture(Scene& scene, const std::string& file);
    // その Canvas の要素（<canvas ui> なら共用 UI の中身）。
    const std::vector<wizengine::ui::ElementDesc>& elementsOf(
        const wizengine::ui::CanvasDesc& desc) const;
    // 生きている Canvas の Renderer 側の入れ物・順序・高さ・出すビューを揃える。
    void syncCanvases(Scene& scene, bool designChanged);
    // 組み直しが要る（canvas, view）を組んで送る。all = 全部（字の置き場の組み直し）。
    void buildMeshes(Scene& scene, bool all);
    // 1 枚の Canvas を、あるビューの大きさで組む。
    wizengine::ui::UiMeshData buildOne(Scene& scene, const wizengine::ui::CanvasDesc& desc,
                                       double canvasWidth);
    void uploadAtlas(Scene& scene);

    std::uint64_t version_ = 0;
    wizengine::UiState::Snapshot snap_;
    std::unique_ptr<wizengine::ui::FontAtlas> atlas_;
    int atlasTexture_ = -1;
    std::map<std::string, int> fontFiles_;        // 正規化したパス → フォントの番号（-1 = 読めない）
    std::map<std::string, int> images_;           // 正規化したパス → テクスチャ（-1 = 読めない）
    std::map<std::size_t, CanvasSlot> canvases_;  // UiState の Canvas 番号 → 入れ物
    std::set<std::size_t> notDrawnWarned_;        // 描かない mode の警告を出した Canvas
};
