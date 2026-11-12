#pragma once

#include <string>
#include <vector>

#include "core/Xml.h"
#include "document/XmlValues.h"
#include "ui/UiTypes.h"

// UI の文書の読み書き（シーン文書の <font> / <ui> / <canvas> と、共用の UI の
// ファイル assets/ui/<名前>.xml）。属性の名前・型・既定値は性質の表（UiProps）・
// Canvas の属性の表（UiTypes の kCanvasAttrs）・種類の表（UiKinds）から引くので、
// 性質や種類を足してもここは変えない。
//
// 書式:
//   <asset>
//     <font name="jp" file="fonts/NotoSansJP-Regular.otf"/>
//     <ui name="hud" file="ui/hud.xml"/>          ... 共用の UI（中身はファイル）
//     <ui name="menu"> <image .../> ... </ui>     ... このシーン専用の UI
//   </asset>
//   <worldbody>
//     <canvas name="hud" mode="overlay" cameras="0 1" height="1080" ui="hud"/>
//     <canvas name="title">                       ... ui を書かなければ中身を直接
//       <image id="1" name="panel" anchor="0 0 1 1" size="-40 -40" color="0 0 0 0.5"/>
//       <text id="2" parent="1" name="score" text="スコア" font="jp" fontSize="48"
//             align="center" outline="0.05" outlineColor="0 0 0 1"/>
//       <anim name="fade" loop="false" autoplay="true">
//         <key element="score" t="0" alpha="0"/>
//         <key element="score" t="0.5" alpha="1"/>
//       </anim>
//     </canvas>
//   </worldbody>
//
//   * 要素は平らに並べ、親は parent="親の id"（文書順で親が前）。id は Canvas
//     （または UI アセット）の中で一意で、省略すると空き番号を振る。name は
//     アニメのキーやノードが要素を指す名前。
//   * 性質の属性名は性質の表の名前（anchor pivot pos size color alpha visible
//     text font fontSize align outline outlineColor wrap file hover pressed
//     enabled）。種類が持たない性質・型に合わない値・知らない属性は警告して無視。
//   * 既定値と同じ値は書き出さない（キーの性質は除く: キーは値そのものが意味）。
//   * <canvas ui="..."> と子要素を両方書いたら、子要素は警告して無視
//     （中身は共用の UI が持つ）。
// 読み取りは失敗しない（文書層の約束）。
namespace wizengine {
namespace ui {

// 要素名（シーン文書の「知っている節の一覧」にもこの名前を使う）。
inline constexpr const char* kFontTag = "font";
inline constexpr const char* kUiAssetTag = "ui";
inline constexpr const char* kCanvasTag = "canvas";
inline constexpr const char* kAnimTag = "anim";
inline constexpr const char* kKeyTag = "key";

// ---- 書き出し ----------------------------------------------------------------
xml::Element fontElement(const FontAssetDesc& font);
// withContents = true は共用ファイルそのもの（uiAssetToXmlText）を書くとき。
// false で file があれば参照だけを書く。
xml::Element uiAssetElement(const UiAssetDesc& asset, bool withContents = false);
xml::Element canvasElement(const CanvasDesc& canvas);

// ---- 読み込み ----------------------------------------------------------------
// <asset> の <font> と <ui> を全部（フォントを先に読む = UI の中の font の参照を確かめる）。
void readUiAssets(const xml::Element& asset, std::vector<FontAssetDesc>& fonts,
                  std::vector<UiAssetDesc>& uiAssets, editor::Warn& warn);
// <worldbody> の <canvas> を全部。fonts / uiAssets は宣言の確かめに使う。
void readCanvases(const xml::Element& world, const std::vector<FontAssetDesc>& fonts,
                  const std::vector<UiAssetDesc>& uiAssets, std::vector<CanvasDesc>& out,
                  editor::Warn& warn);

// ---- 共用の UI のファイル（assets/ui/<名前>.xml）----------------------------
// 中身はシーンの <asset> の <ui> と同じ書式で、ルートが <ui name="..."> になった
// だけ（イベントライブラリの <event name> と同じ流儀）。フォントの宣言はどの
// シーンで使うかで決まるので、読み込みでは確かめない（読むシーンが確かめる）。
std::string uiAssetToXmlText(const UiAssetDesc& asset);
// 名前はルートの name 属性（正規化済み。空もありうる = 呼び手がファイル名で補う）。
// file は空で返す。XML として壊れている場合だけ false。
bool parseUiAssetXml(const std::string& text, UiAssetDesc& out, std::string& error,
                     std::vector<std::string>* warnings = nullptr);

}  // namespace ui
}  // namespace wizengine
