#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "document/SceneDocument.h"
#include "ui/UiLayout.h"  // NodeOverrides
#include "ui/UiTypes.h"

// UI の状態の唯一の持ち主（Scene は ui_ として 1 つ持つだけ）。
//
// 今持つのは設計値（文書の <canvas> / <ui> / <font>）。Canvas は削除しても番号を
// 詰めない（alive = false。ライトの LightItem と同じ流儀 = ノードやブラウザの
// 選択が別の物を指さない）。保存（toDocument）で生きている物だけを詰める。
// エディタの編集（Canvas 1 枚の追加・削除・置き換え）の口は持つが、ブラウザの
// 操作の読み方（JSON・部分更新・丸め）は UiOps が持つ。
// 実行時の状態（シミュレート中だけの物。設計値は書き換えない）: カメラごとのポインタ
// （ボタンの上にいる / 押している = 描く側が重ね順の最後の段に使う）と、押して離した
// クリックの受け渡し（INPUT が積み、PHYSICS が確かめて取り出す）と、ノード（SetUiProp）の
// 上書き（Canvas, 要素 id → 性質。PHYSICS が書き、描く側が重ね順の「ノード」の段に使う）。
// 版は設計値の version と別で、ポインタ（pointersVersion）と上書き（overridesVersion）も別。
// シミュレートの開始・停止・Reset で消える
// （resetRuntime）。アニメの上書きは後の PR でここへ足す。
//
// ファイル・JSON・XML は知らない（共用 UI のファイルは UiFiles、XML は ui/UiXml）。
// スレッド: 設計値を書くのは PHYSICS（読込・編集の Op）。RENDER / INPUT / HTTP は版を見て
// 変わったときだけ写す（copyIfChanged）。実行時の状態のうちポインタとクリックの積み込みは
// INPUT が書く（映像の上の押下・ホバーそのものなので、物理の 1 パスを待たずに見た目へ
// 届ける。ロックの下で書くので PHYSICS の取り出し・resetRuntime とは競らない）。mutex_ は
// 末端のロック（中で他のロックを取らない）なので、objects → editor → poses のどこからでも
// 取ってよい。
namespace wizengine {

class UiState {
public:
    // alive = false の枠は番号を残すだけで、desc は空（読み手は alive だけを見る）。
    struct CanvasItem {
        ui::CanvasDesc desc;
        bool alive = true;
        // 次にエディタで足す要素の id。増える一方（消した番号を使い回さない = ブラウザの
        // 古い選択・ドラッグが新しい要素に当たらない）。入れたとき・置き換えたときに
        // ui::nextElementId（今の最大の次）を下限に持ち上げる。文書には書かない。
        int nextElementId = 1;
        // 中身（要素とアニメ）の世代: 作ったとき・丸ごと入れ替えたとき（ui.xml の適用 =
        // replaceCanvasContents の wholesale）に新しい値になる。値は通し番号から配る（持ち主を
        // 別の UI に差し替えれば違う値になる）。ただし共用にしたとき（moveCanvasContentsToAsset）は
        // 引き継ぐので、Canvas とその UI が同じ値になりうる（同じ中身のまま = ドラッグは続く）。
        // 自分の中身を持つ Canvas のぶんで、共用 UI の中身は UiAssetState の同じ欄。映像の上の
        // ドラッグが「同じ id の別の要素」に書かないための目印で、描画は見ない。
        std::uint64_t contentsGeneration = 0;
        // この枠が最後に変わったときの版（version()。1 より大きい = 0 は「写していない」）。
        // 写し（copyIfChanged）と描く側（contentsRevision）が、変わった物だけを扱う目印。
        std::uint64_t revision = 0;
    };
    // シーンが宣言する UI（<asset> の <ui>。file があれば共用 = 中身は読込でファイルから
    // 入れる）。ファイルへ書く変更の目印は持たない（保存は読んだとき・書いたときの中身と
    // 比べる = Scene の libBaseline_）。
    struct UiAssetState {
        ui::UiAssetDesc desc;
        // 共用 UI のファイルが読めなかった（中身は空）。この中身は編集しない（保存で
        // ファイルへ書かれず、編集が黙って消えるため）。持ち主は変更の目印（Scene の
        // libBaseline_ に記録が無い = 読めない）で、ここはその写し（syncReadable が写す。
        // ほかのスレッドが読めるように UiState に置く）。
        bool unreadable = false;
        // 次にエディタで足す要素の id（CanvasItem::nextElementId と同じ約束。この UI を
        // 使う Canvas が何枚あっても 1 つ）。
        int nextElementId = 1;
        // 中身の世代（CanvasItem::contentsGeneration と同じ約束）。
        std::uint64_t contentsGeneration = 0;
        // 最後に変わったときの版（CanvasItem::revision と同じ約束）。
        std::uint64_t revision = 0;
    };
    // Canvas の要素の持ち主の様子（編集してよいかの規則は uiops::elementsEditable）。
    struct ContentsInfo {
        std::string asset;        // 空 = Canvas 自身。あり = 使っている UI（<canvas ui>）の名前
        bool declared = true;     // その名前の UI が宣言されているか
        bool unreadable = false;  // その UI のファイルが読めなかった
        int nextElementId = 1;    // 次にエディタで足す要素の id
        // 中身の持ち主（自分か使っている UI）の contentsGeneration（宣言の無い UI は 0）。
        std::uint64_t contentsGeneration = 0;
    };
    // 前に取った要素への参照（Canvas の番号・要素の id と、取ったときの世代）。後で「今も同じ
    // 要素か」を確かめる（refValid が唯一の確かめ）: 映像の上のドラッグとボタンのクリックが
    // 使う。世代は取ったときの写しの値（Snapshot::elementRef）。
    struct ElementRef {
        int canvas = -1, element = -1;
        std::uint64_t loadGeneration = 0;      // loadGeneration()
        std::uint64_t contentsGeneration = 0;  // その Canvas の中身の持ち主の世代
    };
    // 設計値の写し（描画・入力・ブラウザ API が版ごとに 1 回だけ取る）。
    struct Snapshot {
        std::uint64_t version = 0;
        // 読込の世代（loadGeneration()。写した時点の値）。
        std::uint64_t loadGeneration = 0;
        std::vector<CanvasItem> canvases;
        std::vector<UiAssetState> uiAssets;
        std::vector<ui::FontAssetDesc> fonts;
        std::uint64_t fontsRevision = 0;  // フォントの宣言が最後に変わった版

        // Canvas index を組む材料（自分の設計値・使っている UI の中身・フォントの宣言）が
        // 最後に変わった版（描く側がこの Canvas だけを組み直す目印。消えた物は 0）。
        // 版は 1 つの数で増える一方なので、どれかが変わればこの値も必ず変わる。
        std::uint64_t contentsRevision(int index) const;
        // 番号 index の Canvas が生きていればその設計値（範囲外・消えた物は nullptr）。
        const ui::CanvasDesc* liveCanvas(int index) const;
        // 名前 name の UI の宣言（無ければ nullptr）。
        const UiAssetState* uiAsset(const std::string& name) const;
        // 生きている Canvas index の要素の持ち主の様子（copyCanvasContents と同じ答え。
        // 消えた物は既定値）。
        ContentsInfo contentsInfo(int index) const;
        // Canvas が使う要素の並び（唯一の定義）: ui を書いていれば共用 UI の中身、
        // 無ければ自分の elements。宣言の無い共用 UI は空。
        const std::vector<ui::ElementDesc>& elementsOf(const ui::CanvasDesc& desc) const;
        // エディタの選択（Canvas の番号、要素の id。-1 = Canvas そのもの）が今もある物を
        // 指すか（選ぶときと、/scene が選択を返すときの確かめの唯一の定義）。
        bool selectionExists(int canvas, int element) const;
        // この写しの世代で (canvas, element) への参照を作る（Canvas が消えていれば中身の
        // 世代は 0 = refValid で必ず外れる）。
        ElementRef elementRef(int canvas, int element) const;
    };

    enum class LoadMode { Replace, Append };
    // fromDocument の結果。
    struct LoadResult {
        // 文書の Canvas 0 番が入った番号（読込のノードの番号をずらすため = lightBase）。
        std::size_t base = 0;
        // 名前が今のシーンと重なったので入れなかった UI とフォントの名前（取込）。
        std::vector<std::string> skippedUi, skippedFonts;
        // 同じ文書の中で 2 回目以降に出た UI の名前（先のものを入れ、後は入れない）。
        std::vector<std::string> duplicatedUi;
        // 渡した uiAssets と同じ並び: その宣言が入ったか（覚えを入れるのは入った物だけ）。
        std::vector<bool> enteredUi;
    };

    // 設計値の版（変わるたびに進む。0 は「まだ一度も写していない」に使える）。
    std::uint64_t version() const;
    // 読込の世代: 中身を丸ごと入れ替えたとき（fromDocument = 読込・シーンの XML の適用・
    // 取込と、clear）だけ進む。エディタの編集（Canvas の中身の XML の適用も）では進まない。
    // 映像の上のドラッグもこの世代が変われば止める。読めなかったフォント・画像を読み直す
    // 区切りにも使う（編集のたびに読み直すと、ドラッグの間ずっと無いファイルを読みに行き、
    // 警告も毎回出る）。
    std::uint64_t loadGeneration() const;
    // version と known が違えば out へ写して known を進め、true。同じなら何もしない。
    // out は前にこの口で埋めた写し（または空）を渡す: 変わった枠・宣言（revision が違う
    // 物）だけを写し直す（ドラッグの 1 パスで写すのは動かした Canvas 1 枚ぶん）。
    bool copyIfChanged(std::uint64_t& known, Snapshot& out) const;
    Snapshot snapshot() const;
    // 生きている Canvas index の設計値を 1 枚だけ out へ写す（範囲外・消えた物は false で
    // out は触らない）。全体の写し（snapshot）より軽い = 映像の上のドラッグのように、
    // 1 枚を何度も読み書きする口。
    bool copyCanvas(int index, ui::CanvasDesc& out) const;
    // 写さずにロックの下で答える口（PHYSICS の編集の確かめ。規則は Snapshot の同じ名前の
    // 関数と同じ）。
    bool selectionExists(int canvas, int element) const;
    // 参照が今も同じ要素を指すか（唯一の確かめ）: 読込の世代が今と同じ・Canvas が生きていて
    // 中身の持ち主の世代が同じ（中身を XML で丸ごと入れ替えた・持ち主が替わったら外れる）・
    // その id の要素が今もある。
    bool refValid(const ElementRef& ref) const;
    // 生きている Canvas に同じ名前があるか（except の番号は数えない。-1 = 全部）。
    bool canvasNameTaken(const std::string& name, int except) const;
    // 名前 name の UI の宣言があれば true と、その file。
    bool findUiAsset(const std::string& name, std::string& file) const;

    // 文書へ書く（生きている Canvas だけを詰め、共用 UI とフォントの宣言はそのまま）。
    // 戻り値 = Canvas の番号 → 文書の中の番号（消えた物は -1。保存で Canvas を
    // 番号で指すノードを付け替えるため。ライトの lightRemap と同じ形）。
    std::vector<int> toDocument(editor::SceneDocument& doc) const;
    // 文書から入れる。Replace（読込・XML の適用）は置き換え、Append（取込）は
    // 後ろへ足す（共用 UI とフォントは名前が重ならないものだけ。重なった名前は
    // LoadResult に返す。同じ文書の中で名前が 2 回出た UI は先のものを入れる）。
    // 共用 UI の中身は呼び手が先にファイルから入れておく（UiFiles）。読めなかった印は
    // 入れない（入れた後に syncReadable で変更の目印から写す）。
    LoadResult fromDocument(const std::vector<ui::CanvasDesc>& canvases,
                            const std::vector<ui::UiAssetDesc>& uiAssets,
                            const std::vector<ui::FontAssetDesc>& fonts, LoadMode mode);
    // 共用 UI（file あり）の「読めなかった」の印を readable(名前) から写し直す（印の唯一の
    // 書き手。readable は変更の目印に記録があるか = uifiles::syncReadable）。ローカルの UI は
    // 読めている。変わったときだけ版を進める。
    void syncReadable(const std::function<bool(const std::string& name)>& readable);
    // 全部消す（シーンの全消し）。
    void clear();

    // ---- エディタの編集（PHYSICS の UiOps と、映像の上のドラッグ = ----------
    // ---- UiInputComponent::onEditorStep が呼ぶ）-------------------------------
    // 後ろへ足して番号を返す。
    int addCanvas(ui::CanvasDesc desc);
    // 生きている Canvas を消す（番号は残す = alive = false）。無ければ false。removed が
    // あれば、消した設計値をそこへ移す（ステータスの呼び名に使う）。
    bool removeCanvas(int index, ui::CanvasDesc* removed = nullptr);
    // 生きている Canvas の設計値を置き換える。無ければ false（何もしない）。次の要素の
    // id は下がらない（置き換えた中身の最大の次までは上がる）。
    bool replaceCanvas(int index, ui::CanvasDesc desc);
    // 生きている Canvas の設計値を写し、要素とアニメ（elements / anims）は持ち主から
    // 入れる: 自分の物か、<canvas ui> なら使っている UI の中身（宣言が無ければ空）。
    // info に持ち主の様子。消えた物は false（out は触らない）。
    bool copyCanvasContents(int index, ui::CanvasDesc& out, ContentsInfo& info) const;
    // copyCanvasContents で写して書き換えた desc の要素とアニメを持ち主へ戻す（自分の
    // 物なら Canvas ごと置き換え、使っている UI なら UI の中身だけ = Canvas の属性は
    // 触らない）。Canvas が消えた・使う UI が写したときと変わった・UI の宣言が無いなら
    // false（何もしない）。
    // wholesale = 中身を丸ごと入れ替えた（ui.xml の適用）: 同じロックの中で持ち主の
    // contentsGeneration を新しくする（その中身の上のドラッグだけを止める。ほかの Canvas の
    // ドラッグ・描画の読み直しは起こさない。写しにも同じ版で入る）。
    bool replaceCanvasContents(int index, ui::CanvasDesc desc, bool wholesale = false);

    // ---- 宣言した UI（<asset> の <ui>。共用にする・やめる・読み込む = UiOps）-----
    // 名前 name の UI を out へ写す（無ければ false）。
    bool copyUiAsset(const std::string& name, ui::UiAssetDesc& out) const;
    // UI を宣言に足す（同じ名前があれば false で何もしない）。
    bool addUiAsset(ui::UiAssetDesc asset);
    // 名前 name の UI の file（空 = このシーン専用）を変える。無ければ false。読めたかの印は
    // 変えない（syncReadable が変更の目印から写す）。
    bool setUiAssetFile(const std::string& name, const std::string& file);
    // 生きている Canvas の中身（要素とアニメ）を UI asset にして宣言に足し、Canvas は
    // その UI を使う（ui = asset.name、自分の要素とアニメは空）。asset.elements / anims は
    // 呼び手が Canvas の今の中身を写して入れておく（要素の id はそのまま = 選択と映像の
    // 上のドラッグは同じ要素を指し続ける）。Canvas が消えた・既に UI を使っている・同じ
    // 名前の宣言があるなら false（何もしない）。
    bool moveCanvasContentsToAsset(int index, ui::UiAssetDesc asset);

    // ---- 実行時の状態（シミュレート中）-----------------------------------------------
    // カメラ 1 台のポインタ: どの Canvas の、どのボタン（要素の id）の上にいるか・押して
    // いるか（-1 = 無し。押したまま外へ出たら pressed も hover も -1 で、離すまで押下は
    // 続く）。受け付けない（enabled = false の）ボタンも入る（見た目を変えないのは描く側の
    // ui::resolve の判断）。
    struct Pointer {
        int canvas = -1;
        int hover = -1;
        int pressed = -1;
        bool operator==(const Pointer& o) const {
            return canvas == o.canvas && hover == o.hover && pressed == o.pressed;
        }
        bool operator!=(const Pointer& o) const { return !(*this == o); }
        // Canvas index の要素 id へのポインタの様子（描く側が要素ごとに引く）。
        ui::PointerPhase phaseOf(int index, int element) const {
            if (canvas != index || element < 0) return ui::PointerPhase::None;
            if (pressed == element) return ui::PointerPhase::Pressed;
            return hover == element ? ui::PointerPhase::Hover : ui::PointerPhase::None;
        }
    };
    // 実行時の世代の組（generations() が 1 回のロックで読む）。押下とクリックに添え、今と
    // 違えば捨てる。
    struct Generations {
        // resetRuntime（シミュレートの開始・停止・Reset）で進む。Reset をまたいだ押下を捨てる
        // （Reset ではモードの世代が進まないため）。
        std::uint64_t runtime = 0;
        // setPaused で止める・再開するたびに進む。押してから取り出すまでに一時停止をまたいだ
        // クリックを捨てる。
        std::uint64_t pause = 0;
    };
    // 押して離した 1 回（INPUT が積む）。世代は押したときの値（PHYSICS が今と比べる）。
    struct PendingClick {
        std::size_t camera = 0;
        ElementRef ref;                        // 押したボタン（押したときの写しの世代）
        std::uint64_t modeEpoch = 0;           // EditorState::modeEpoch
        Generations generations;               // generations()（押したときの値）
    };
    // 確かめたクリック 1 回（takeClicks の結果 = ノードの OnUiClick が受ける形）。
    struct Click {
        std::size_t camera = 0;
        int canvas = -1;    // Canvas の番号（UiState の番号）
        int element = -1;   // 要素の id
        std::string name;   // 要素の名前（ノードは名前で絞る。空可）
    };
    // 積んだまま取り出されないクリックの上限（エディタ中は取り出さないので、溢れた古い物は
    // 捨てる = どのみち世代が違って捨てられる物）。
    static constexpr std::size_t kMaxPendingClicks = 64;

    // Canvas 1 枚のノードの上書き（SetUiProp。要素の id → 性質）と、最後に変わったときの
    // 上書きの版（描く側がこの Canvas だけを組み直し・写し直す目印。0 = 上書きが無い）。
    struct CanvasOverrides {
        ui::NodeOverrides props;
        std::uint64_t revision = 0;
    };
    // ノードの上書きの写し（描く側とボタンの当たりが、変わった Canvas だけ写し直す）。
    struct NodeOverrideSet {
        std::map<int, CanvasOverrides> canvases;  // Canvas の番号 → ノードの上書き
        // Canvas index のノードの上書き（無ければ nullptr = ui::resolveElements にそのまま渡す）。
        const ui::NodeOverrides* of(int index) const;
        // その上書きが最後に変わった版（無ければ 0）。
        std::uint64_t revision(int index) const;
    };
    // setNodeProp の結果（Ok 以外は書かない）。
    enum class NodePropResult { Ok, Unchanged, NoCanvas, NoElement, KindLacksProp };

    // ポインタの版（変わるたびに進む。クリックの出し入れでは進まない = 描く側が写し直すのは
    // ボタンの見た目が変わったときだけ）。
    std::uint64_t pointersVersion() const;
    // ノードの上書きの版（ポインタとは別 = ボタンをまたいでも上書きを写し直さない）。
    std::uint64_t overridesVersion() const;
    // 今の実行時の世代の組（1 回のロックで両方を読む）。
    Generations generations() const;
    // 一時停止しているか（PHYSICS が毎パス Scene::setPaused から渡す）。変わったときだけロックを
    // 取って一時停止の世代を進め、止まったときは積んだクリックを捨てる（止まっている間は取り
    // 出されず、再開した瞬間にまとめて発火させない）。止まっている間の pushClick も捨てる。
    // ポインタ（hover / pressed の見た目）は触らない。
    void setPaused(bool paused);
    // このカメラのページに押せる UI があるか（そのカメラに出ている overlay の Canvas に、
    // 押下を受け止める要素がある。ブラウザがホバーを送るかの判断 = /scene の uiClickable）。
    bool clickableOnCamera(int camera) const;
    // カメラ camera のポインタを置く（INPUT。変わったときだけ版を進める）。スロットの範囲
    // （kMaxCameraSlots）の外と、runtimeGeneration（置く側が受け付けるかを決める前に読んだ
    // Generations::runtime）が今と違う物は捨てる（比べるのはロックの下 = resetRuntime と競らない。
    // 止めた・Reset した後に古いポインタを残さない）。
    void setPointer(std::size_t camera, const Pointer& pointer, std::uint64_t runtimeGeneration);
    // pointersVersion と known が違えばポインタの一覧を out へ写して known を進め、true。
    bool copyPointersIfChanged(std::uint64_t& known, std::vector<Pointer>& out) const;
    // overridesVersion と known が違えば、ノードの上書きのうち変わった Canvas だけを out へ写し
    // （消えた Canvas の分は out からも消す）、known を進めて true。out は前にこの口で埋めた写し
    // （または空）を渡す。
    bool copyOverridesIfChanged(std::uint64_t& known, NodeOverrideSet& out) const;
    // ノード（SetUiProp）の上書き 1 つを置く（PHYSICS）。要素は名前（element が空でなければ
    // 名前で引く = ui::findElementByName の規則）か id（elementId）で指す。その種類が持たない
    // 性質は書かない（KindLacksProp）。値は性質の表の範囲へ丸め、ノードの欄が持たない成分
    // （色の不透明度）は描く側の値のまま（ui::withNodeValue / effectiveValue）にして置き、今と
    // 同じ値なら版を進めない（毎フレームから同じ値を書き続けても組み直さない = Unchanged）。
    // 設計値は書き換えない（resetRuntime で消える）。
    NodePropResult setNodeProp(int canvas, const std::string& element, int elementId,
                               ui::Prop prop, const ui::PropValue& value);
    // 押して離した 1 回を積む（INPUT。一時停止中は捨てる）。
    void pushClick(const PendingClick& click);
    // 積まれたクリックを全部取り出し、今も有効な物だけを返す（PHYSICS。唯一の確かめ）:
    // 押したときのモードの世代が modeEpoch と同じ・実行時と一時停止の世代が今と同じ・参照が今も同じ
    // 要素（refValid）・そのカメラの映像に重なる Canvas（ui::overlayShownOnCamera）・クリックを
    // 受け付ける（ui::acceptsClick。性質はノードの上書きまで重ねた値 = ui::resolveElements）。
    std::vector<Click> takeClicks(std::uint64_t modeEpoch);
    // 実行時の状態を全部捨てる（シミュレートの開始・停止・Reset。Scene::resetGraphRuntime）。
    void resetRuntime();

private:
    // 版を進めて新しい版を返す（変えた枠・宣言の revision に入れる）。
    std::uint64_t bumpLocked() { return ++version_; }
    // 押して離したクリック 1 回の確かめ（mutex_ を持って呼ぶ。takeClicks の中身）。
    bool clickValidLocked(const PendingClick& c, Click& out) const;
    // refValid の中身（mutex_ を持って呼ぶ）。
    bool refValidLocked(const ElementRef& ref) const;
    // 中身の世代の新しい値（contentsGeneration。mutex_ を持って呼ぶ）。
    std::uint64_t nextGenerationLocked() { return ++generations_; }
    // 全 Canvas を生きていない枠にする（番号は残し、中身は空に）。mutex_ を持って呼ぶ。
    void retireAllCanvasesLocked(std::uint64_t revision);

    mutable std::mutex mutex_;
    // 実行時の状態（ポインタは pointersVersion_、上書きは overridesVersion_ が版）。
    std::vector<Pointer> pointers_;         // カメラのスロット番号 → ポインタ
    std::vector<PendingClick> clicks_;      // 積まれて、まだ取り出していないクリック
    std::map<int, CanvasOverrides> overrides_;  // Canvas の番号 → ノードの上書き
    std::uint64_t pointersVersion_ = 1;
    std::uint64_t overridesVersion_ = 1;
    std::uint64_t runtimeGeneration_ = 1;
    std::uint64_t pauseGeneration_ = 1;
    // setPaused（積み込みを捨てるかの判断）。書くのは setPaused（PHYSICS）だけで、変わったときは
    // mutex_ の下で書く。atomic なのは、毎パスの「変わっていない」をロック無しで見るため。
    std::atomic<bool> paused_{false};
    std::vector<CanvasItem> canvases_;
    std::vector<UiAssetState> uiAssets_;
    std::vector<ui::FontAssetDesc> fonts_;
    std::uint64_t fontsRevision_ = 1;
    std::uint64_t version_ = 1;
    std::uint64_t loadGeneration_ = 1;
    std::uint64_t generations_ = 0;  // 最後に配った中身の世代
};

}  // namespace wizengine
