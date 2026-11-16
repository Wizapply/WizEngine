#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "document/SceneDocument.h"
#include "ui/UiTypes.h"

// UI の状態の唯一の持ち主（Scene は ui_ として 1 つ持つだけ）。
//
// 今持つのは設計値（文書の <canvas> / <ui> / <font>）。Canvas は削除しても番号を
// 詰めない（alive = false。ライトの LightItem と同じ流儀 = ノードやブラウザの
// 選択が別の物を指さない）。保存（toDocument）で生きている物だけを詰める。
// エディタの編集（Canvas 1 枚の追加・削除・置き換え）の口は持つが、ブラウザの
// 操作の読み方（JSON・部分更新・丸め）は UiOps が持つ。
// 実行時の上書き（ノード・アニメ・ポインタ）とクリックの受け渡しは後の PR で
// ここへ足す（設計値の版 version と別の版を持つ予定）。
//
// ファイル・JSON・XML は知らない（共用 UI のファイルは UiFiles、XML は ui/UiXml）。
// スレッド: 書くのは PHYSICS（読込・編集の Op）。RENDER / INPUT / HTTP は版を見て
// 変わったときだけ写す（copyIfChanged）。mutex_ は末端のロック（中で他のロックを
// 取らない）なので、objects → editor → poses のどこからでも取ってよい。
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
    };
    // 共用の UI（file あり）は読込で中身をファイルから入れる。dirty はファイルへ
    // 書いていない変更の目印（書く側は後の PR。今は常に false）。
    struct UiAssetState {
        ui::UiAssetDesc desc;
        bool dirty = false;
    };
    // 設計値の写し（描画・入力・ブラウザ API が版ごとに 1 回だけ取る）。
    struct Snapshot {
        std::uint64_t version = 0;
        // 読込の世代（loadGeneration()。写した時点の値）。
        std::uint64_t loadGeneration = 0;
        std::vector<CanvasItem> canvases;
        std::vector<UiAssetState> uiAssets;
        std::vector<ui::FontAssetDesc> fonts;

        // 番号 index の Canvas が生きていればその設計値（範囲外・消えた物は nullptr）。
        const ui::CanvasDesc* liveCanvas(int index) const;
        // 番号 index の Canvas に次に足す要素の id（生きていなければ 1）。
        int nextElementIdOf(int index) const;
        // Canvas が使う要素の並び（唯一の定義）: ui を書いていれば共用 UI の中身、
        // 無ければ自分の elements。宣言の無い共用 UI は空。
        const std::vector<ui::ElementDesc>& elementsOf(const ui::CanvasDesc& desc) const;
        // エディタの選択（Canvas の番号、要素の id。-1 = Canvas そのもの）が今もある物を
        // 指すか（選ぶときと、/scene が選択を返すときの確かめの唯一の定義）。
        bool selectionExists(int canvas, int element) const;
    };

    enum class LoadMode { Replace, Append };

    // 設計値の版（変わるたびに進む。0 は「まだ一度も写していない」に使える）。
    std::uint64_t version() const;
    // 読込の世代: 中身を丸ごと入れ替えたとき（fromDocument = 読込・XML の適用・取込と、
    // clear）だけ進む。エディタの編集では進まない。読めなかったフォント・画像を
    // 読み直す区切りに使う（編集のたびに読み直すと、ドラッグの間ずっと無いファイルを
    // 読みに行き、警告も毎回出る）。
    std::uint64_t loadGeneration() const;
    // version と known が違えば out へ写して known を進め、true。同じなら何もしない。
    bool copyIfChanged(std::uint64_t& known, Snapshot& out) const;
    Snapshot snapshot() const;

    // 文書へ書く（生きている Canvas だけを詰め、共用 UI とフォントの宣言はそのまま）。
    // 戻り値 = Canvas の番号 → 文書の中の番号（消えた物は -1。保存で Canvas を
    // 番号で指すノードを付け替えるため。ライトの lightRemap と同じ形）。
    std::vector<int> toDocument(editor::SceneDocument& doc) const;
    // 文書から入れる。Replace（読込・XML の適用）は置き換え、Append（取込）は
    // 後ろへ足す（共用 UI とフォントは名前が重ならないものだけ）。戻り値 =
    // 文書の Canvas 0 番が入った番号（読込のノードの番号をずらすため = lightBase）。
    // 共用 UI の中身は呼び手が先にファイルから入れておく（UiFiles）。
    std::size_t fromDocument(const std::vector<ui::CanvasDesc>& canvases,
                             const std::vector<ui::UiAssetDesc>& uiAssets,
                             const std::vector<ui::FontAssetDesc>& fonts, LoadMode mode);
    // 全部消す（シーンの全消し）。
    void clear();

    // ---- エディタの編集（PHYSICS の UiOps だけが呼ぶ）----------------------
    // 後ろへ足して番号を返す。
    int addCanvas(ui::CanvasDesc desc);
    // 生きている Canvas を消す（番号は残す = alive = false）。無ければ false。
    bool removeCanvas(int index);
    // 生きている Canvas の設計値を置き換える。無ければ false（何もしない）。次の要素の
    // id は下がらない（置き換えた中身の最大の次までは上がる）。
    bool replaceCanvas(int index, ui::CanvasDesc desc);

private:
    void bumpLocked() { ++version_; }
    // 全 Canvas を生きていない枠にする（番号は残し、中身は空に）。mutex_ を持って呼ぶ。
    void retireAllCanvasesLocked();

    mutable std::mutex mutex_;
    std::vector<CanvasItem> canvases_;
    std::vector<UiAssetState> uiAssets_;
    std::vector<ui::FontAssetDesc> fonts_;
    std::uint64_t version_ = 1;
    std::uint64_t loadGeneration_ = 1;
};

}  // namespace wizengine
