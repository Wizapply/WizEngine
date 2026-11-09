#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <chrono/core/ChVector3.h>

namespace wizengine {

// 当たり形状のための CPU 側の形状の読み込み。描画側の glTF の経路（gltfio）は
// 頂点を GPU へ直接流すので、物理は cgltf（gltfio が内部で使うのと同じ
// 単一ヘッダのパーサ）の上に自前の小さな読み込みを持つ。`name` は他の
// 読み込みと同じく assetPath() で解決し、各ノードのワールド変換と指定の
// 一様な倍率を掛け、作ったものをログに出す。投げずに空を返す: 凸包の当たり
// 判定は既に描画できるシーンに足すおまけなので、壊れたファイルは実行を
// 止めずに（警告付きで）質を落とす。

// モデルの頂点を maxPoints 以下に間引いたもの（格子への量子化）。
// ChBodyEasyConvexHull にそのまま渡せる。凸包には数百点で十分で、
// 密なモデルの頂点を全部渡しても凸包の構築が遅くなるだけ。
// Scene の <asset><mesh/>（凸包の当たり判定）がこれを使う。
std::vector<chrono::ChVector3d> loadCollisionPoints(
    const std::string& name, double scale, std::size_t maxPoints = 512);

// 点を格子に寄せて 1 セル 1 点に間引く。格子を粗くしながら maxPoints 以下に
// なるまで。外形は 1 セル以内に保たれる。点の順は元の順のまま（pointId を
// 安定させるため）。格子 2 でも多い（同じ点が大量など）ときは等間隔に抜く。
// loadCollisionPoints（凸包の入力）とテレインの接触の形の記録が使う。
std::vector<chrono::ChVector3d> decimatePoints(std::vector<chrono::ChVector3d> pts,
                                               std::size_t maxPoints);

// 三角形をそのまま（頂点と 3 つ組の添字）。凹形状の当たり判定
// （<geom collision="trimesh">、PhysicsWorld::addTriangleMesh）用。
// 読めなければ両方空。
struct CollisionTriangles {
    std::vector<chrono::ChVector3d> vertices;
    std::vector<std::array<int, 3>> triangles;
};
CollisionTriangles loadCollisionTriangles(const std::string& name, double scale);

}  // namespace wizengine