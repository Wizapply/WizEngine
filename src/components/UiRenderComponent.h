#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "scene/SceneComponent.h"
#include "scene/UiState.h"
#include "ui/FontAtlas.h"
#include "ui/UiMeshFormat.h"  // UiMeshData
#include "ui/UiPlane.h"       // CanvasPlane

namespace wizengine {
class Renderer;
}

// UI（Canvas）を描画側へ反映する（RENDER スレッドの onRender と onRenderUnlocked）。
// overlay は映像に重ねる UI のビューに、camera / world は主のシーンの板として描く
// （板の置き方は ui/UiPlane.h と Scene::canvasPlane。中身は全ビューで 1 つ、姿勢は
// ビューごとに毎フレーム = camera の板は描くフレームのカメラに付く）。
//
// 仕事は 2 つの口に分ける: onRender（オブジェクトのロックの中）はカメラの有効と
// ビュー番号を写すだけ。重い仕事（フォント・画像の読み込み、字の SDF 化、メッシュの
// 組み立てと転送）は全部 onRenderUnlocked（ロックの外。Renderer は使ってよく、Scene の
// オブジェクト一覧は読まない = 要るものは onRender の写しを使う）。
// UiState の設計値は版（copyIfChanged。UiState の末端のロック）で写し、写しを読むだけ
// （UiState の内部は持たない）。Canvas × ビューごとにメッシュを組んで Renderer へ送る。
// 毎フレームの仕事は「その Canvas の材料の版（Snapshot::contentsRevision）・そのビューの
// 縦横比・字の置き場の世代」のどれかが変わったときだけ（ほかの Canvas のドラッグでは組まない）。
// 字の置き場（FontAtlas）とフォント・画像のテクスチャの持ち主もここ（RENDER 専用）。
// シミュレート中のボタンの見た目（ポインタが上にある・押している）は、UiState の実行時の
// ポインタ（カメラごと）を重ね順の最後の段（ui::resolve の pointer）に入れて組む。ポインタは
// カメラごとなので、そのカメラのビューの中身だけが変わる（組み直しの条件にもビューごとに入る）。
// ノード（SetUiProp）の上書きは「ノード」の段（ui::resolveElements）で、上書きの版が変わった
// Canvas だけを組み直す。
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
        std::uint64_t revision = 0;  // 組んだ材料の版（Snapshot::contentsRevision）
        double canvasWidth = -1.0;  // canvasRect の幅（= 基準の高さ × 縦横比）
        int generation = -1;        // 字の置き場の世代
        // そのビューのカメラのポインタ（この Canvas の上のボタンの id。-1 = 無し）
        int hover = -1, pressed = -1;
        std::uint64_t overrides = 0;  // ノードの上書きの版（Runtime::overridesRevision）
    };
    struct CanvasSlot {
        std::size_t rendererId = 0;
        // 板（camera / world）として作ったか。overlay ⇄ 板が変わったら作り直す。板の中身は
        // 全ビューで 1 つ（built には kPlateBuildKey の 1 つだけ）、姿勢はビューごと。
        bool plate = false;
        std::vector<std::size_t> views;            // setUiCanvasViews に渡した一覧
        int rank = -1;  // setUiCanvasOrder に渡した前後の順位（奥から 0, 1, 2, ...）
        std::map<std::size_t, ViewBuild> built;    // ビュー → 送った条件
    };

    // 板の Canvas の、カメラ（スロット番号）から見た板（Scene::canvasPlane を包む）。
    using PlaneFn = std::function<bool(const wizengine::ui::CanvasDesc&, std::size_t,
                                       wizengine::ui::CanvasPlane&)>;
    // 組み直しを決める材料の版（buildPlate へ渡す組）。
    struct Material {
        std::uint64_t revision = 0;   // Snapshot::contentsRevision
        std::uint64_t overrides = 0;  // ノードの上書きの版
        int generation = 0;           // 字の置き場の世代
    };

    // ロックの外の仕事の全部（onRenderUnlocked が Renderer と UI の状態と板の出し方だけを
    // 渡す）。
    void update(wizengine::Renderer& renderer, const wizengine::UiState& state,
                const PlaneFn& planeOf);
    // 板の Canvas を組んで送る（中身は全ビューで 1 つ。材料・ポインタが変わったときだけ）。
    void buildPlate(wizengine::Renderer& renderer, int index, CanvasSlot& slot,
                    const Material& material, bool all);
    // 板の姿勢を出すビューごとに送る（毎フレーム。camera の板はカメラに付く）。
    void placePlates(wizengine::Renderer& renderer, const PlaneFn& planeOf);
    // 板の Canvas の見た目に使うポインタ（どれかのビューのカメラが上に居ればそれ）。
    wizengine::UiState::Pointer plateCanvasPointer(const CanvasSlot& slot, int index) const;
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
    // 描く Canvas の前後の順位（ui::canvasBehind で奥から 0, 1, ...）を Renderer へ送る
    // （変わった物だけ。Renderer はこの整数だけで並べる）。
    void syncRanks(wizengine::Renderer& renderer);
    // 組み直しが要る（canvas, view）を組んで送る。all = 全部（字の置き場の組み直し）。
    void buildMeshes(wizengine::Renderer& renderer, bool all);
    // 字の置き場が溢れたとき: 全 Canvas を（同じ幅は 1 回だけ）組んで使っている字に
    // 印を付け、置き場を組み直す。
    void rebuildAtlas(wizengine::Renderer& renderer);
    // 写しに生きている Canvas があるか（写したときに anyAlive_ へ）。
    bool anyAliveCanvas() const;
    // 1 枚の Canvas（番号 index）を、あるビューの大きさとそのカメラのポインタで組む。
    wizengine::ui::UiMeshData buildOne(wizengine::Renderer& renderer, int index,
                                       const wizengine::ui::CanvasDesc& desc, double canvasWidth,
                                       const wizengine::UiState::Pointer& pointer);
    // ビュー view のカメラのポインタを Canvas index の分だけ（ほかの Canvas の上・エディタ中は
    // 無し）。
    wizengine::UiState::Pointer pointerOn(std::size_t view, int index) const;
    void uploadAtlas(wizengine::Renderer& renderer);
    // 選択枠: 出すときは専用の Canvas をエディタカメラのビューに置いてメッシュを送り、
    // 出さないときはどのビューにも置かない。条件が前回と同じなら何もしない。
    void updateFrame(wizengine::Renderer& renderer);

    // 選択枠を最後に送った条件（全部同じなら組み直さない。出さないときは既定値のまま）。
    struct FrameKey {
        std::uint64_t revision = 0;  // その Canvas の材料の版（Snapshot::contentsRevision）
        int canvas = -1, element = -1, view = -1, viewWidth = 0, viewHeight = 0;
        bool operator==(const FrameKey& o) const {
            return revision == o.revision && canvas == o.canvas && element == o.element &&
                   view == o.view && viewWidth == o.viewWidth && viewHeight == o.viewHeight;
        }
    };

    std::uint64_t version_ = 0;         // 最後に写した UiState の版
    std::uint64_t fontsRevision_ = 0;   // 最後に読んだフォントの宣言の版
    std::uint64_t loadGeneration_ = 0;  // 最後に見た UiState の読込の世代
    wizengine::UiState::Snapshot snap_;
    std::unique_ptr<wizengine::ui::FontAtlas> atlas_;
    int atlasTexture_ = -1;
    std::map<std::string, int> fontFiles_;        // 正規化したパス → フォントの番号（-1 = 読めない）
    std::map<std::string, int> images_;           // 正規化したパス → テクスチャ（-1 = 読めない）
    std::map<std::size_t, CanvasSlot> canvases_;  // UiState の Canvas 番号 → 入れ物
    bool anyAlive_ = false;  // 写しに生きている Canvas があるか（UI の無いシーンは何もしない）
    // カメラ（スロット番号）→ ビュー番号（無効・ビュー未定は -1）。onRender が写す。
    std::vector<int> cameraViews_;
    // シミュレート中か（onRender が写す。エディタ中はポインタの段を使わない）。
    bool simulating_ = false;
    // UiState の実行時の状態の写しと、その版: カメラ（スロット番号）→ ポインタと、ノードの
    // 上書き（版は別 = ボタンをまたいでも上書きは写し直さない）。
    std::vector<wizengine::UiState::Pointer> pointers_;
    std::uint64_t pointersVersion_ = 0;
    wizengine::UiState::NodeOverrideSet overrides_;
    std::uint64_t overridesVersion_ = 0;
    // 選択枠の対象（onRender が写す。-1 = 出さない = 選択が無い・エディタモードでない・
    // プレハブ編集中）とエディタカメラのスロット番号。
    int frameCanvas_ = -1, frameElement_ = -1;
    int frameCamera_ = 0;
    // 選択枠の Canvas（Renderer の番号。最初に出すときに作る）。
    bool frameCreated_ = false;
    std::size_t frameRendererId_ = 0;
    FrameKey frameSent_;
};
