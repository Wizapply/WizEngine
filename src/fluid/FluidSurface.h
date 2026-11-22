#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// 流体粒子から「つながった水の表面」の三角メッシュを組む（純粋な計算。Chrono も
// Filament も知らない）。粒子を 1 個ずつ玉で描くと、隣どうしが離れた粒の集まりに
// 見える。そこで粒子ごとに滑らかな山（メタボール）を足し合わせた場
//   f(x) = Σ_i w(|x − p_i| / R)、w(q) = (1 − q²)³（q < 1）
// を格子に溜め、その等値面 f = iso を **Surface Nets**（等値面をまたぐ格子のセルごとに
// 頂点を 1 個置き、等値面をまたぐ格子の辺ごとに周りの 4 セルの頂点で四角形を張る）で
// 三角形にする。近い粒子の山は重なって 1 つの面になり、離れた粒子は小さな水滴になる。
// 法線は場の勾配（滑らかな陰影）。
//
// 大きさの決め方は粒子の間隔 d を単位にする: 山の半径 R = radius × d、格子の間隔
// h = cell × d、等値面 iso は「粒子が詰まった塊の表面が、外側の粒子の中心から
// d/2 ほど外に来る」値（FluidSurface.cpp の kIso。単体テストで確かめてある）。
// 孤立した粒子（しぶき）も小さな水滴として残る。
// 格子は粒子の外接箱 + R の範囲だけを持つ（流体のいない所の計算をしない）。セルの
// 数が maxCells を超えるときは h を広げる（細かさより時間を守る）。
namespace wizengine {
namespace fluid {

struct SurfaceParams {
    double spacing = 0.04;     // 粒子の間隔 d (m)
    double radius = 1.4;       // 山の半径 R（d の何倍。kIso はこの値で較正してある）
    double cell = 0.5;         // 格子の間隔 h（d の何倍）
    std::size_t maxCells = 1500000;  // 格子の節点の上限（超えたら h を広げる）
    int smooth = 1;            // 頂点の位置をならす回数（0 = しない）
};

struct SurfaceMesh {
    std::vector<float> positions;  // 3 個で 1 頂点（ワールド）
    std::vector<float> normals;    // 同じ並び（外向き、単位ベクトル）
    std::vector<uint32_t> indices; // 3 個で 1 三角形（外から見て反時計回り）
    double cellSize = 0.0;         // 実際に使った格子の間隔 (m)

    // 中身を入れ替える（スレッド間の受け渡しで写さないため）。
    void swap(SurfaceMesh& o) noexcept {
        positions.swap(o.positions);
        normals.swap(o.normals);
        indices.swap(o.indices);
        std::swap(cellSize, o.cellSize);
    }
};

// 作業領域（毎フレームの確保を避けるため、使い手が持ち回る）。
struct SurfaceScratch {
    std::vector<float> field;
    std::vector<int32_t> cellVertex;
};

// points は 3 個で 1 粒子（ワールド）。out は上書きする。
void buildFluidSurface(const std::vector<float>& points, const SurfaceParams& params,
                       SurfaceMesh& out, SurfaceScratch& scratch);

// 等値面の値（場の正規化は w(0) = 1）。テストと説明のために公開する。
double fluidSurfaceIso();

}  // namespace fluid
}  // namespace wizengine
