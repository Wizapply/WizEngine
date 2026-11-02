#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "terrain/HeightField.h"

// テレインの散布物（木や小物）の値型と、置き場所の座標の換算・値の検査。
//
// 散布物は「原型」（モデルのパスと幹の寸法）と「配置」（どの原型をどこに・
// どの向きと倍率で）の 2 つ。配置の位置はテレインの正規化座標 (u, v)
// （0〜1、格子の minX〜maxX / minZ〜maxZ）で持ち、高さは持たない（地表に
// 載せるのは使う側。形を彫っても木が地表に付いていく）。
//
// .terrain の TREE 節との往復は TerrainFile の担当。ここは Chrono も Filament も
// シーン文書も知らない。パスの関所（assets/ の外を指していないか）は呼び手
// （Scene）の担当で、ここは素の値を運ぶだけ。
namespace wizengine {
namespace terrain {

// 原型の数（固定の席。配置は番号で原型を指す）。
constexpr int kMaxScatterPrototypes = 8;
// 配置の数の上限（描画と当たり判定の重さの安全弁）。
constexpr int kMaxScatterInstances = 4096;
// 原型の倍率の範囲（clampScatterPrototype が丸める。配置の倍率は丸めず、
// pruneScatterInstances が 0 以下・有限でないものを落とすだけ）。
constexpr float kScatterScaleMin = 0.01f;
constexpr float kScatterScaleMax = 100.0f;
// 幹の半径・高さの上限 (m)。下限は 0。
constexpr float kScatterTrunkMax = 100.0f;

// 原型 1 つ（モデルと幹の当たり判定の寸法）。
struct ScatterPrototype {
    std::string file;           // モデル（assets/ 相対のパス）。空 = 代役で描く
    float scale = 1.0f;         // モデルの倍率
    float trunkRadius = 0.25f;  // 幹の半径 (m、倍率 1 のとき)
    float trunkHeight = 2.0f;   // 幹の高さ (m、倍率 1 のとき)
    bool collide = false;       // 幹に当たり判定を付けるか

    bool operator==(const ScatterPrototype& o) const {
        return file == o.file && scale == o.scale && trunkRadius == o.trunkRadius &&
               trunkHeight == o.trunkHeight && collide == o.collide;
    }
    bool operator!=(const ScatterPrototype& o) const { return !(*this == o); }
};

// 配置 1 つ。
struct ScatterInstance {
    float u = 0.0f;         // 正規化座標（0〜1、格子の -X 端 → +X 端）
    float v = 0.0f;         // 正規化座標（0〜1、格子の -Z 端 → +Z 端）
    float yaw = 0.0f;       // Y 軸まわりの向き（ラジアン）
    float scale = 1.0f;     // 原型の倍率に掛ける倍率
    uint8_t prototype = 0;  // 原型の番号（0〜kMaxScatterPrototypes-1）

    bool operator==(const ScatterInstance& o) const {
        return u == o.u && v == o.v && yaw == o.yaw && scale == o.scale &&
               prototype == o.prototype;
    }
    bool operator!=(const ScatterInstance& o) const { return !(*this == o); }
};

// テレイン 1 枚ぶんの散布物。
struct TerrainScatter {
    std::array<ScatterPrototype, kMaxScatterPrototypes> prototypes;
    std::vector<ScatterInstance> instances;

    // 原型が全部既定で配置も無いか（= .terrain に TREE 節を書かない）。
    // 原型だけ変えた（配置 0 個）でも空ではない。
    bool empty() const {
        if (!instances.empty()) return false;
        for (const ScatterPrototype& p : prototypes) {
            if (p != ScatterPrototype{}) return false;
        }
        return true;
    }
    bool operator==(const TerrainScatter& o) const {
        return prototypes == o.prototypes && instances == o.instances;
    }
    bool operator!=(const TerrainScatter& o) const { return !(*this == o); }
};

// 正規化座標 → 格子のローカルの X / Z（minX + u × (maxX − minX)）。
double scatterLocalX(const HeightField& field, double u);
double scatterLocalZ(const HeightField& field, double v);
// 格子のローカルの (x, z) → 正規化座標。空の格子・有限でない座標・格子の
// 水平な範囲（境界を含む）の外は false で u / v は触らない。
bool scatterUvAt(const HeightField& field, double x, double z, double& u, double& v);

// 原型の数値を範囲へ丸める（有限でない値は既定値、倍率は kScatterScaleMin〜
// kScatterScaleMax、幹は 0〜kScatterTrunkMax）。パスは触らない。
ScatterPrototype clampScatterPrototype(ScatterPrototype p);
// 使えない配置を落とす: u / v が有限でないか [0, 1] の外・倍率が有限でないか
// 0 以下・向きが有限でない・原型の番号が範囲外。そのあと kMaxScatterInstances を
// 超えるぶんを後ろから切る。戻り値は落とした数（切ったぶんを含む）。
std::size_t pruneScatterInstances(TerrainScatter& s);

}  // namespace terrain
}  // namespace wizengine
