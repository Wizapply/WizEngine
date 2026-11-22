#pragma once

#include <cstddef>
#include <vector>

#include "fluid/FluidMath.h"

// 流体粒子（<worldbody> の <fluid>）の初期配置と、粒子を描くメッシュ。純粋な関数
// だけ（Chrono も Filament も知らない）。PhysicsWorld が粒子を Chrono::Multicore の
// 流体コンテナへ入れるときと、FluidComponent がエディタで設計どおりの粒子を描く
// ときの両方がここを使う = 見た目と物理の初期状態が必ず一致する。
namespace wizengine {
namespace fluid {

// 水を置く箱（中心・半寸法・初速）。
struct FluidBlock {
    Vec3 center;
    Vec3 half{0.2, 0.2, 0.2};
    Vec3 velocity;
};

struct FluidSeedResult {
    std::vector<float> positions;   // 3 個で 1 粒子（ワールド）
    std::vector<float> velocities;  // 同じ並び
    std::size_t requested = 0;      // 上限が無ければ作った数
    bool truncated = false;         // 上限（maxParticles）で打ち切ったか
};

// 粒子の間隔 spacing (m) の六方最密（HCP）格子で箱を満たす（Chrono の
// demo_MCORE_fluidNSC と同じ詰め方）。粒子の中心は箱の内側に spacing / 2 の
// 余白を取る（粒子の広がりが箱からはみ出さない）。maxParticles を超えるぶんは
// 作らない（箱の順に詰めて打ち切る）。
FluidSeedResult seedFluid(const std::vector<FluidBlock>& blocks, double spacing,
                          std::size_t maxParticles);

// 粒子 1 個の体積（HCP の 1 粒子あたり = spacing³ / √2）。質量 = 密度 × これ。
double particleVolume(double spacing);
// Chrono の流体コンテナのカーネル半径（デモと同じ: 粒子の間隔 = カーネル半径 × 0.9）。
double kernelRadius(double spacing);

// ---- 描画用のメッシュ ----------------------------------------------------------
// 粒子 1 個 = 正八面体（6 頂点・8 三角形）。法線は頂点の向き（中心から外）なので
// 陰影は丸く見える。
constexpr std::size_t kParticleVerts = 6;
constexpr std::size_t kParticleTris = 8;
// count 個ぶんのインデックス（外から見て反時計回り）。
std::vector<unsigned> particleMeshIndices(std::size_t count);
// 中心 points（3 個で 1 粒子）・半径 radius の頂点と法線を pos / nrm に書く
// （ここで resize する）。
void particleMeshVertices(const std::vector<float>& points, double radius,
                          std::vector<float>& pos, std::vector<float>& nrm);

}  // namespace fluid
}  // namespace wizengine
