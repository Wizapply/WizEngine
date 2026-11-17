#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
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
    // シーンが宣言する UI（<asset> の <ui>。file があれば共用 = 中身は読込でファイルから
    // 入れる）。ファイルへ書く変更の目印は持たない（保存は読んだとき・書いたときの中身と
    // 比べる = Scene の libBaseline_）。
    struct UiAssetState {
        ui::UiAssetDesc desc;
        // 共用 UI のファイルが読めなかった（中身は空）。この中身は編集しない（保存で
        // ファイルへ書かれず、編集が黙って消えるため）。
        bool unreadable = false;
        // 次にエディタで足す要素の id（CanvasItem::nextElementId と同じ約束。この UI を
        // 使う Canvas が何枚あっても 1 つ）。
        int nextElementId = 1;
    };
    // Canvas の要素の持ち主の様子（編集してよいかの規則は uiops::elementsEditable）。
    struct ContentsInfo {
        std::string asset;        // 空 = Canvas 自身。あり = 使っている UI（<canvas ui>）の名前
        bool declared = true;     // その名前の UI が宣言されているか
        bool unreadable = false;  // その UI のファイルが読めなかった
        int nextElementId = 1;    // 次にエディタで足す要素の id
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
    };

    enum class LoadMode { Replace, Append };
    // fromDocument の結果。
    struct LoadResult {
        // 文書の Canvas 0 番が入った番号（読込のノードの番号をずらすため = lightBase）。
        std::size_t base = 0;
        // 名前が今のシーンと重なったので入れなかった UI とフォントの名前（取込）。
        std::vector<std::string> skippedUi, skippedFonts;
    };

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
    // 生きている Canvas index の設計値を 1 枚だけ out へ写す（範囲外・消えた物は false で
    // out は触らない）。全体の写し（snapshot）より軽い = 映像の上のドラッグのように、
    // 1 枚を何度も読み書きする口。
    bool copyCanvas(int index, ui::CanvasDesc& out) const;

    // 文書へ書く（生きている Canvas だけを詰め、共用 UI とフォントの宣言はそのまま）。
    // 戻り値 = Canvas の番号 → 文書の中の番号（消えた物は -1。保存で Canvas を
    // 番号で指すノードを付け替えるため。ライトの lightRemap と同じ形）。
    std::vector<int> toDocument(editor::SceneDocument& doc) const;
    // 文書から入れる。Replace（読込・XML の適用）は置き換え、Append（取込）は
    // 後ろへ足す（共用 UI とフォントは名前が重ならないものだけ。重なった名前は
    // LoadResult に返す）。共用 UI の中身は呼び手が先にファイルから入れておく
    // （UiFiles）。unreadable = そのとき読めなかった共用 UI の名前（編集しない印）。
    LoadResult fromDocument(const std::vector<ui::CanvasDesc>& canvases,
                            const std::vector<ui::UiAssetDesc>& uiAssets,
                            const std::vector<ui::FontAssetDesc>& fonts, LoadMode mode,
                            const std::vector<std::string>& unreadable = {});
    // 全部消す（シーンの全消し）。
    void clear();

    // ---- エディタの編集（PHYSICS の UiOps と、映像の上のドラッグ = ----------
    // ---- UiInputComponent::onEditorStep が呼ぶ）-------------------------------
    // 後ろへ足して番号を返す。
    int addCanvas(ui::CanvasDesc desc);
    // 生きている Canvas を消す（番号は残す = alive = false）。無ければ false。
    bool removeCanvas(int index);
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
    bool replaceCanvasContents(int index, ui::CanvasDesc desc);

    // ---- 宣言した UI（<asset> の <ui>。共用にする・やめる・読み込む = UiOps）-----
    // 名前 name の UI を out へ写す（無ければ false）。
    bool copyUiAsset(const std::string& name, ui::UiAssetDesc& out) const;
    // UI を宣言に足す（同じ名前があれば false で何もしない）。
    bool addUiAsset(ui::UiAssetDesc asset);
    // 名前 name の UI の file（空 = このシーン専用）を変える。今の中身がそのまま正になる
    // （読めなかった印を下ろす）。無ければ false。
    bool setUiAssetFile(const std::string& name, const std::string& file);
    // 生きている Canvas の中身（要素とアニメ）を UI asset にして宣言に足し、Canvas は
    // その UI を使う（ui = asset.name、自分の要素とアニメは空）。asset.elements / anims は
    // 呼び手が Canvas の今の中身を写して入れておく（要素の id はそのまま = 選択と映像の
    // 上のドラッグは同じ要素を指し続ける）。Canvas が消えた・既に UI を使っている・同じ
    // 名前の宣言があるなら false（何もしない）。
    bool moveCanvasContentsToAsset(int index, ui::UiAssetDesc asset);

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
