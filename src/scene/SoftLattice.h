#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "document/EditorTypes.h"

// ソフトボディの「格子」: 質点ばね方式の設計図。
//
// ソフトボディは Chrono の小さな剛体（粒子）を格子状に並べ、隣どうしを
// ばねで結んだもの（質点ばね方式。FEA モジュールは使わない）。ここは
// **設計値（BodyDesc）から格子を作る純粋な計算**だけで、Chrono も Filament
// も出てこない:
//
//   * 粒子の静止位置（ボディローカル）と当たり半径
//   * ばねの一覧（両端の粒子番号・自然長・ばね定数・減衰係数）
//   * 描画用の表面（表面粒子 → 頂点、三角形のインデックス）
//
// 物理側（PhysicsWorld::addSoftBody）は粒子とばねを、描画側
// （Scene::applyToRenderer）は表面を使う。どちらも同じ Lattice を見るので
// 「当たっている場所」と「見えている場所」がずれない。
//
// 格子は 1 軸あたり n 個（2〜8）の粒子で、箱なら各辺を n 等分したセルの
// 中心、球なら同じ箱格子を球へ写像（spherified cube）したもの。どちらも
// 表面の位相は「立方体の 6 面」なので、表面メッシュの作り方は共通。
namespace wizengine {
namespace softlattice {

using Vec3 = std::array<double, 3>;

// ばねの種類。構造（隣）・せん断（面と立方体の対角線）・曲げ（1 個おき）。
enum class SpringKind { Structural, Shear, Bend };

struct Spring {
    int a = 0, b = 0;      // 粒子番号
    SpringKind kind = SpringKind::Structural;
    double rest = 0.0;     // 自然長 (m)
    double k = 0.0;        // ばね定数 (N/m)
    double c = 0.0;        // 減衰係数 (N·s/m)
};

struct Lattice {
    int n = 0;                       // 1 軸あたりの粒子数
    std::vector<Vec3> rest;          // 粒子の静止位置（ボディローカル, m）
    double radius = 0.0;             // 粒子の当たり半径 (m)
    double particleMass = 0.0;       // 粒子 1 個の質量 (kg)
    std::vector<Spring> springs;
    // 描画用の表面。surface[v] = 頂点 v に対応する粒子番号。indices は
    // 3 個で 1 三角形（頂点番号）、外から見て反時計回り。
    std::vector<int> surface;
    std::vector<std::uint32_t> indices;

    std::size_t particleCount() const { return rest.size(); }
};

// 設計値から格子を作る。desc.hasSoft を見ない（呼ぶ側が判断する）。
// 形は Box か Sphere（Model は Box として扱う）。
Lattice build(const editor::BodyDesc& desc);

// 粒子の現在位置（ワールド、3 個で 1 粒子・particleCount ぶん）から描画用の
// 頂点位置と法線を作る。位置は「粒子の中心 + 法線 × 当たり半径」で、
// 粒子の球の外側に表面を張る（当たっている面と見えている面を揃える）。
// positions / normals は 3 個で 1 頂点、surface.size() ぶんに詰め直される。
void buildSurface(const Lattice& lattice, const float* particles,
                  std::vector<float>& positions, std::vector<float>& normals);

// 同じことを任意の位相で（ソフトタイヤなど、Lattice 以外の粒子メッシュ用）。
// surface[v] = 頂点 v の粒子番号（nullptr なら頂点 v = 粒子 v）、indices は
// 3 個で 1 三角形（頂点番号・外向きの巻き）。法線は隣接三角形の面積重み平均。
void buildSurfaceMesh(const int* surface, std::size_t vertexCount,
                      const std::uint32_t* indices, std::size_t indexCount,
                      double radius, const float* particles,
                      std::vector<float>& positions, std::vector<float>& normals);

}  // namespace softlattice
}  // namespace wizengine
