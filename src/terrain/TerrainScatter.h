#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <Eigen/Geometry>

#include "core/MathConstants.h"
#include "terrain/HeightField.h"

// テレインの散布物（木や小物）の値型と、置き場所の座標の換算・値の検査、
// 散布の筆の計算（scatterStamp: 円の中へ配置を足す / 円の中の配置を消す）。
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

struct BrushStamp;  // TerrainBrush.h（scatterStamp が参照で受けるだけ）

// 原型の数（固定の席。配置は番号で原型を指す）。
constexpr int kMaxScatterPrototypes = 8;
// 配置の数の上限（描画と当たり判定の重さの安全弁）。
constexpr int kMaxScatterInstances = 4096;
// 原型の倍率の範囲（clampScatterPrototype が丸める。配置の倍率は丸めず、
// pruneScatterInstances が 0 以下・有限でないものを落とすだけ）。下限は散布の
// 筆（scatterStamp）が作る配置の倍率の下限にも使う。
constexpr float kScatterScaleMin = 0.01f;
constexpr float kScatterScaleMax = 100.0f;
// 幹の半径・高さの上限 (m)。下限は 0。
constexpr float kScatterTrunkMax = 100.0f;

// 原型 1 つ（モデルと幹の当たり判定の寸法）。
struct ScatterPrototype {
    std::string file;           // モデル（assets/ 相対のパス）。空 = 代役で描く
    // モデルの見た目の倍率（モデルの単位 → m の換算にも使う）。幹の寸法には
    // 掛からない（scatterTrunk）。
    float scale = 1.0f;
    float trunkRadius = 0.25f;  // 幹の半径 (m。配置の倍率だけが掛かる)
    float trunkHeight = 2.0f;   // 幹の高さ (m。配置の倍率だけが掛かる)
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

// 配置 1 つの幹（立てた円柱）。座標は格子のローカル（field と同じ）。
// 幹は鉛直（ローカルの Y 向き）に立て、足元 ground から高さ height ぶん伸びる。
// 軸が X の円柱（描画の単位円柱・物理の円柱部品）で描く・当てる側は、Z まわりに
// 90° 回して X を Y へ向ける（代役の描画も幹の当たり判定もこれに従う）。
struct ScatterTrunk {
    double x = 0.0, z = 0.0;  // 配置の位置（scatterLocalX / Z）
    double ground = 0.0;      // その点の地表の高さ（heightAt）= 配置の足元
    double radius = 0.0;      // 幹の半径 (m) = 原型の trunkRadius × 配置の倍率
    double height = 0.0;      // 幹の高さ (m) = 原型の trunkHeight × 配置の倍率
    // 幹の中心の高さ。
    double centerY() const { return ground + 0.5 * height; }
};
// 軸が X の円柱を Y へ立てる回転（上の Z まわり 90°）の唯一の定義。
inline Eigen::Quaterniond scatterTrunkUpright() {
    return Eigen::Quaterniond(Eigen::AngleAxisd(0.5 * kPi, Eigen::Vector3d::UnitZ()));
}
// 配置 inst（原型 proto）の幹を out に書く。足元が穴・格子の外（solidAt が false）
// なら false で out は触らない。幹の寸法の規則はここ 1 か所（描画の代役と幹の
// 当たり判定が同じ値を使う）。原型の倍率（モデルの見た目）は幹に掛けない。
bool scatterTrunk(const HeightField& field, const ScatterPrototype& proto,
                  const ScatterInstance& inst, ScatterTrunk& out);
// 配置のモデルの見た目の倍率（原型の倍率 × 配置の倍率）。
double scatterModelScale(const ScatterPrototype& proto, const ScatterInstance& inst);
// 配置 inst が幹の当たり判定を持つ原型を指していれば、その原型（s の中を指す）。
// 原型の番号が範囲外・collide でない・幹の半径か高さが 0 以下なら nullptr。
// 「幹の当たり判定を持つ配置」の規則はここ 1 か所。
const ScatterPrototype* scatterCollidingPrototype(const TerrainScatter& s,
                                                  const ScatterInstance& inst);
// 幹の当たり判定を持つ配置（scatterCollidingPrototype）のうち、足元のセル
// （HeightField::cellAt。穴でも数える）が cells（セル単位）の中にあるものが
// 1 つでもあるか。cells が空なら false。
bool scatterTouches(const TerrainScatter& s, const HeightField& field, const GridRect& cells);

// 原型の数値を範囲へ丸める（有限でない値は既定値、倍率は kScatterScaleMin〜
// kScatterScaleMax、幹は 0〜kScatterTrunkMax）。パスは触らない。
ScatterPrototype clampScatterPrototype(ScatterPrototype p);
// 使えない配置を落とす: u / v が有限でないか [0, 1] の外・倍率が有限でないか
// 0 以下・向きが有限でない・原型の番号が範囲外。そのあと kMaxScatterInstances を
// 超えるぶんを後ろから切る。戻り値は落とした数（切ったぶんを含む）。
std::size_t pruneScatterInstances(TerrainScatter& s);

// 散布の筆の安全弁: 円 1 個の目標の個数の上限と、スタンプ 1 個で試す場所の数の
// 上限（どちらも半径・密度を最大にしたときの重さを抑える）。
constexpr int kScatterMaxPerDisc = 256;
constexpr int kScatterMaxTriesPerStamp = 1024;

// scatterStamp の結果の内訳（呼び手のステータス用）。
struct ScatterStampStats {
    std::size_t added = 0;    // 足した配置の数
    std::size_t removed = 0;  // 消した配置の数
    bool capped = false;      // kMaxScatterInstances に当たって置けなかった場所があった
};

// 散布の筆のスタンプ 1 個を scatter の配置に当てる（格子の座標 = field と同じ
// ローカル。円・円の中・届く範囲は TerrainBrush の stampDiscCount / stampDisc /
// stampBounds）。
//   Scatter   … 円ごとの目標の個数は density × π r²（kScatterMaxPerDisc まで）の
//                端数をくじで丸めた数（roundByLottery。くじは seed と円の番号から
//                決まる）。平均が式どおりなのは、何も無い所へのスタンプ 1 回ぶん
//                だけ: 同じ場所で押し続ける・線分の円が重なると、スタンプごとに
//                種が変わるので、円の中の数は ceil(density × π r²) まで増える
//                （spacing がそれより先に詰まればそこまで）。小さな筆でも当て
//                続ければ置ける。この目標に対して、
//                円の中にある同じ原型の配置の不足分を、不足分の 4 倍まで
//                （スタンプ全体で kScatterMaxTriesPerStamp まで）場所を試して足す。
//                場所は円の中の一様なくじ（seed と円・試行の番号から決まる =
//                同じ入力なら同じ結果）。穴・格子の外（solidAt が false）と、
//                どれかの配置（原型によらない）から spacing 未満の場所には
//                置かない。向きは 0〜2π、倍率は 1 + scaleJitter × [-1, 1)
//                （kScatterScaleMin を下限にする = 0 以下の倍率を作らない）。
//                kMaxScatterInstances に達したら止めて capped。
//   Unscatter … どれかの円の中にある配置を原型によらず全部消す。
// 使えないスタンプ（brushStampUsable）・散布の道具でない・空の格子は何もしない。
// 配置が 1 つでも増減したら true。stats は省略可（あれば上書きする）。
bool scatterStamp(const HeightField& field, const BrushStamp& stamp, TerrainScatter& scatter,
                  ScatterStampStats* stats = nullptr);

}  // namespace terrain
}  // namespace wizengine
