#pragma once

#include <cstdint>
#include <vector>

// 字の SDF の決まり（輪郭の値・にじみ幅。頂点の縁取りの幅の意味が依る）。
#include "ui/UiSdf.h"

// UI の頂点の形（型だけ。処理は持たない）。UI 側（UiMesh）が作り、描画側
// （render/）がそのまま頂点バッファに写す。並び順の知識をここ 1 か所に
// 置くため、render はこのヘッダだけを読む。Chrono / Filament / Scene は知らない。
namespace wizengine {
namespace ui {

// 字の SDF の作り方（基準の字の大きさ・にじみ幅・輪郭の値）は ui/UiSdf.h。

// 頂点 1 個。座標は Canvas の設計上の画素（左上が原点、y 下向き）。
struct UiVertex {
    float x = 0.0f, y = 0.0f;    // 位置
    float u0 = 0.0f, v0 = 0.0f;  // テクスチャの座標（0..1）
    float u1 = 0.0f, v1 = 0.0f;  // 要素の中の位置（左上 0,0 〜 右下 1,1）
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;  // 色（リニア値、不透明度込み）
    // 縁取りの幅（にじみ幅に対する割合 0..1、SDF の字だけが使う）。輪郭の外の
    // 距離 d の画素の値は kSdfOnEdge × (1 − d / にじみ幅) なので、材質は
    // 「値 ≥ kSdfOnEdge × (1 − outline)」を縁取りの内側と読めばよい
    // （字の大きさ・基準の大きさに関係なく同じ式）。
    float outline = 0.0f;
    // 縁取りの色（リニア値、不透明度込み）。outline が 0 の頂点では使わない。
    float outlineR = 0.0f, outlineG = 0.0f, outlineB = 0.0f, outlineA = 0.0f;
};

// 材質の番号（描画側がこの番号で材質を選ぶ）。
enum UiMaterialId : int {
    kUiMaterialImage = 0,  // RGBA の画像（テクスチャ無しは白）
    kUiMaterialSdf = 1,    // 字の置き場（R8 の SDF）
};

// テクスチャの番号のうち「テクスチャ無し（白）」。
inline constexpr int kUiNoTexture = -1;

// 同じテクスチャ・材質で続く三角形の範囲（描画の 1 回ぶん）。
struct UiDrawRun {
    int textureId = kUiNoTexture;
    int materialId = kUiMaterialImage;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

// Canvas 1 枚ぶん。三角形はインデックス 3 個で 1 枚、描く順 = 並び順。
struct UiMeshData {
    std::vector<UiVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<UiDrawRun> runs;
};

}  // namespace ui
}  // namespace wizengine
