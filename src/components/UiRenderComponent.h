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
#include "ui/UiMeshFormat.h"  // UiMeshData

namespace wizengine {
class Renderer;
}

// UI（overlay の Canvas）を描画側へ反映する（RENDER スレッドの onRender と
// onRenderUnlocked）。
//
// 仕事は 2 つの口に分ける: onRender（オブジェクトのロックの中）はカメラの有効と
// ビュー番号を写すだけ。重い仕事（フォント・画像の読み込み、字の SDF 化、メッシュの
// 組み立てと転送）は全部 onRenderUnlocked（ロックの外。Renderer は使ってよく、Scene の
// オブジェクト一覧は読まない = 要るものは onRender の写しを使う）。
// UiState の設計値は版（copyIfChanged。UiState の末端のロック）で写し、写しを読むだけ
// （UiState の内部は持たない）。Canvas × ビューごとにメッシュを組んで Renderer へ送る。毎フレームの
// 仕事は「版・そのビューの縦横比・字の置き場の世代」のどれかが変わったときだけ。
// 字の置き場（FontAtlas）とフォント・画像のテクスチャの持ち主もここ（RENDER 専用）。
// エディタの選択枠（選んでいる要素の枠とつまみ、ui/UiHandles.h の形）も、エディタ
// カメラのビューにだけ出す専用の Canvas として描く（どの UI よりも手前。後処理なし）。
// onRender が選択とエディタカメラを写し、組み立ては onRenderUnlocked。
class UiRenderComponent : public SceneComponent {
public:
    UiRenderComponent();
    ~UiRenderComponent() override;
    void onRender(Scene& scene) override;
    void onRenderUnlocked(Scene& scene) override;

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

    // ロックの外の仕事の全部（onRenderUnlocked が Renderer と UI の状態だけを渡す）。
    void update(wizengine::Renderer& renderer, const wizengine::UiState& state);
    void ensureAtlas(wizengine::Renderer& renderer);
    // 版が変わったとき: 宣言のフォントを読み（ファイルごとに 1 回）、名前の対応を引き直す。
    // 読めなかったファイルを読み直すのは reloaded（読込の世代が進んだ）か、宣言から
    // 外れたときだけ。
    void syncFonts(bool reloaded);
    // 版が変わったとき: どの生きている Canvas も使わなくなった画像を返す。読めなかった
    // 記録は、使わなくなったときか reloaded のときに捨てる（次の組み立てでもう一度試す）。
    void releaseUnusedImages(wizengine::Renderer& renderer, bool reloaded);
    int imageTexture(wizengine::Renderer& renderer, const std::string& file);
    // 生きている Canvas の Renderer 側の入れ物・順序・高さ・出すビューを揃える。
    void syncCanvases(wizengine::Renderer& renderer, bool designChanged);
    // 組み直しが要る（canvas, view）を組んで送る。all = 全部（字の置き場の組み直し）。
    void buildMeshes(wizengine::Renderer& renderer, bool all);
    // 字の置き場が溢れたとき: 全 Canvas を（同じ幅は 1 回だけ）組んで使っている字に
    // 印を付け、置き場を組み直す。
    void rebuildAtlas(wizengine::Renderer& renderer);
    // 写しに生きている Canvas があるか（写したときに anyAlive_ へ）。
    bool anyAliveCanvas() const;
    // 1 枚の Canvas を、あるビューの大きさで組む。
    wizengine::ui::UiMeshData buildOne(wizengine::Renderer& renderer,
                                       const wizengine::ui::CanvasDesc& desc, double canvasWidth);
    void uploadAtlas(wizengine::Renderer& renderer);
    // 選択枠: 出すときは専用の Canvas をエディタカメラのビューに置いてメッシュを送り、
    // 出さないときはどのビューにも置かない。条件が前回と同じなら何もしない。
    void updateFrame(wizengine::Renderer& renderer);

    // 選択枠を最後に送った条件（全部同じなら組み直さない。出さないときは既定値のまま）。
    struct FrameKey {
        std::uint64_t version = 0;
        int canvas = -1, element = -1, view = -1, viewWidth = 0, viewHeight = 0;
        bool operator==(const FrameKey& o) const {
            return version == o.version && canvas == o.canvas && element == o.element &&
                   view == o.view && viewWidth == o.viewWidth && viewHeight == o.viewHeight;
        }
    };

    std::uint64_t version_ = 0;
    std::uint64_t loadGeneration_ = 0;  // 最後に見た UiState の読込の世代
    wizengine::UiState::Snapshot snap_;
    std::unique_ptr<wizengine::ui::FontAtlas> atlas_;
    int atlasTexture_ = -1;
    std::map<std::string, int> fontFiles_;        // 正規化したパス → フォントの番号（-1 = 読めない）
    std::map<std::string, int> images_;           // 正規化したパス → テクスチャ（-1 = 読めない）
    std::map<std::size_t, CanvasSlot> canvases_;  // UiState の Canvas 番号 → 入れ物
    // 描かない mode の警告を出した Canvas → そのときの mode（mode が変わったら出し直す）。
    std::map<std::size_t, wizengine::ui::CanvasMode> notDrawnWarned_;
    bool anyAlive_ = false;  // 写しに生きている Canvas があるか（UI の無いシーンは何もしない）
    // カメラ（スロット番号）→ ビュー番号（無効・ビュー未定は -1）。onRender が写す。
    std::vector<int> cameraViews_;
    // 選択枠の対象（onRender が写す。-1 = 出さない = 選択が無い・エディタモードでない・
    // プレハブ編集中）とエディタカメラのスロット番号。
    int frameCanvas_ = -1, frameElement_ = -1;
    int frameCamera_ = 0;
    // 選択枠の Canvas（Renderer の番号。最初に出すときに作る）。
    bool frameCreated_ = false;
    std::size_t frameRendererId_ = 0;
    FrameKey frameSent_;
};
