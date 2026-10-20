#pragma once

#include <vector>

#include <Eigen/Core>

#include "terrain/HeightField.h"

// テレイン（高さ格子）と物の接触を見つける純粋な計算。
//
// HeightField と同じ位置付けで、Chrono も Filament もシーン文書も知らない
// （tests/terrain で単体で回せる）。境界（PhysicsWorld）がボディの形を
// BodyProxy に記録し、毎ステップ姿勢と速度を渡して Contact の列を受け取り、
// Chrono の接触（ChCollisionInfo）へ詰め替える。力・摩擦・積分は Chrono の
// まま。見つけるのだけがここ。
//
// 方式は「点の集合 vs 高さ格子」:
//   * 形ごとにローカルの点の列を作る（箱 = 8 角 + 任意で面サンプル、
//     球 = 中心、円柱 = 両端のリム 12 点ずつ + 両端の中心、Points = そのまま）。
//   * 点 p が穴でないセルの上（solidAt）なら h = heightAt、n = normalAt
//     （面法線、+Y 側）、gap = (p.y − h)·n.y（球は − 半径）。
//   * 閾値 = envelope + approachSpeedFactor·max(0, −v_n)·dt（v_n はその点の
//     速度の法線成分）。gap < 閾値なら接触。
//   * 地面の下は詰まっている扱い: どれだけ深く沈んでも法線は地表の上向き
//     = 三角メッシュ（厚み 0）のように法線が反転してすり抜けることがない。
// 式は B1 のベンチ（chrono_bench の mode=hf のインライン検出）と同じ。
// 違いは 2 つ: 早期棄却の上限を回転も含めて漏れなく取ること（下の
// collectContacts）と、出力を常に深い順に並べること（ベンチは上限を超えた
// ときだけ並べ替えていた）。
namespace wizengine {
namespace terrain {

// 回転（ローカル → 親の向き）。Eigen の 3x3 は整列の制約が無いので、構造体の
// メンバや std::vector にそのまま入れてよい。
using Mat3 = Eigen::Matrix3d;

// 物（ボディ）のワールドの姿勢。position はボディの原点（Chrono の GetPos）。
struct Pose {
    Vec3 position = Vec3::Zero();
    Mat3 rotation = Mat3::Identity();
};

// ボディの速度（ワールド）。linear は Pose::position の点の速度、angular は
// 角速度 (rad/s)。点 p の速度 = linear + angular × (p − position)。
struct BodyVelocity {
    Vec3 linear = Vec3::Zero();
    Vec3 angular = Vec3::Zero();
};

// 形 1 個の記録（ボディローカル）。position / rotation はボディ原点から見た
// 形の置き場所（複合形状の部品・凸包の重心ずれの補正に使う）。
struct ShapeProxy {
    enum class Kind {
        Box,       // halfExtents（各辺の半分）
        Sphere,    // radius。1 点接触の丸い形（Contact::round）
        Cylinder,  // radius と halfLength。軸は形のローカル X（Renderer の円柱・ExtraShape と同じ）
        Points,    // points（形のローカル）。凸包の間引き点・三角メッシュの頂点など
    };
    Kind kind = Kind::Box;
    Vec3 position = Vec3::Zero();
    Mat3 rotation = Mat3::Identity();
    Vec3 halfExtents = Vec3::Zero();  // Box
    double radius = 0.0;              // Sphere / Cylinder
    double halfLength = 0.0;          // Cylinder
    std::vector<Vec3> points;         // Points
};

// ボディ 1 個ぶんの形の集合。boundRadius はボディ原点を中心に全部の形が
// 収まる球の半径（早期棄却に使う。computeBoundRadius で出せる）。
// **shapes を変えたら出し直す**こと（小さすぎると接触を取りこぼす）。
// 負・有限でない値は不正な入力 = 無接触。
//
// pointId（Contact）は shapes の並び・各形の点数と ContactParams::faceSamples
// で決まる。反力キャッシュ（H2）は proxy ごとに params を固定し、
// proxyPointCount の大きさで確保する（どちらかを変えたら作り直す）。
struct BodyProxy {
    std::vector<ShapeProxy> shapes;
    double boundRadius = 0.0;
};

// 接触の判定の設定。
struct ContactParams {
    double envelope = 0.006;           // 静止時の閾値 (m)。B1 の推薦値
    double approachSpeedFactor = 1.5;  // 近づく速度の項の倍率（B1 の推薦値）。0 = 速度項なし
    double dt = 0.0;                   // 1 ステップの時間 (s)。0 = 速度項なし
    int maxPerBody = 16;               // 1 ボディあたりの上限（深い順に残す）。0 以下 = 上限なし
    int faceSamples = 0;               // 箱の各面に k x k の内点を足す（0 = 8 角のみ。上限 16）
};

// 接触 1 個。Chrono へは A = 物（vpA = onBody）、B = テレイン
// （vpB = onTerrain）、vN = −normal（A → B）、distance = gap で渡す。
struct Contact {
    Vec3 onBody = Vec3::Zero();     // 物の側の点（ワールド）。球は中心 − normal·半径
    Vec3 onTerrain = Vec3::Zero();  // テレインの側の点（ワールド）= onBody − normal·gap
    Vec3 normal = Vec3::UnitY();    // テレインの上向き法線（面法線、単位ベクトル）
    double gap = 0.0;               // 負 = めり込み (m)
    int shape = 0;                  // BodyProxy::shapes の番号
    int pointId = 0;  // ボディ内で安定な点番号（0〜proxyPointCount − 1。反力キャッシュの添字）
    bool round = false;  // 1 点接触の丸い形（球）由来か（テレインの材質を分けるため）
};

// テレインの早期棄却に使う値を覚えた参照。どちらも格子全体を走査するので、
// 毎ステップ出さないようここで 1 回だけ出しておく:
//   topY       … 地表の最高点のワールド Y（baseY + maxSample）
//   minNormalY … 全三角形の面法線の y の最小値（一番急な斜面。平らなら 1）
// **格子の高さを書き換えたら作り直す**こと（古い値だと接触を取りこぼす）。
// field は呼び出し側が生かしておく（一時オブジェクトからは作れない）。
struct ContactTerrain {
    const HeightField* field = nullptr;
    double topY = 0.0;
    double minNormalY = 1.0;

    ContactTerrain() = default;
    explicit ContactTerrain(const HeightField& f);
    ContactTerrain(const HeightField&&) = delete;
};

// 形 1 個 / ボディ全体の点の数（pointId の範囲。faceSamples で変わる）。
int shapePointCount(const ShapeProxy& shape, const ContactParams& params);
int proxyPointCount(const BodyProxy& proxy, const ContactParams& params);

// 形の全点（球は中心から半径ぶん、円柱の縁を含む）がボディ原点からどれだけ
// 離れうるかの上限。BodyProxy::boundRadius に入れる値。箱の面サンプルは箱の
// 中なので faceSamples に依らない。
double computeBoundRadius(const BodyProxy& proxy);

// 接触を out に書いて（先に clear する）数を返す。並びは深い順（gap の小さい
// 順、同じなら pointId の小さい順）で、maxPerBody を超えたぶんは浅い方を捨てる。
// 次の場合は無接触（0）:
//   * テレインが空 / 未設定、proxy が空
//   * 姿勢・速度・boundRadius・params・topY / minNormalY に有限でない値がある
//   * 早期棄却: D = position.y − boundRadius − topY として、D > 0 かつ
//     D·minNormalY − boundRadius·(1 − minNormalY) > 閾値の上限
//     （閾値の上限 = envelope + factor·(|linear| + |angular|·boundRadius)·dt）。
//     左辺は斜面での平面距離 gap の下限、右辺は回転を含めた点ごとの閾値の
//     上限なので、点ごとの判定で接触になる物を捨てることはない（導出は .cpp）。
//     前提は boundRadius と ContactTerrain が今の形・今の格子に合っていること。
// 点ごとには、穴のセル・格子の外（solidAt が false）の点と、有限でない点を
// 飛ばす（形の値が壊れていても他の点は見る）。半径が負・有限でない球は飛ばす。
int collectContacts(const ContactTerrain& terrain, const BodyProxy& proxy, const Pose& pose,
                    const BodyVelocity& velocity, const ContactParams& params,
                    std::vector<Contact>& out);

}  // namespace terrain
}  // namespace wizengine
