#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "terrain/HeightField.h"

// テレインの筆: 1 スタンプ（線分）ぶんの格子の書き換えと、その影響セル矩形。
//
// HeightField と同じ純粋な層（Chrono も Filament もシーン文書も知らない。
// tests/terrain が単体で回す）。座標は**格子の座標**（HeightField の minX /
// minZ と同じ系 = テレインのローカル）で受ける。ワールドとの変換は呼び手が
// TerrainFrame で行う。
//
// 1 スタンプは「前回の点 → 今回の点」の線分 1 本で、線分を半径の 1/4 刻みに
// 割って円（ディスク）を順に当てる（ポインタが速く動いても筋が途切れない）。
// 始点の円は前回のスタンプが当て済みなので当てない（begin は始点 = 終点で
// 1 個だけ）。高さ・穴の道具は dt を円の数で等分する = 線の長さで効き目の
// 総量が変わらない。Paint は円の重みをセルごとの最大でまとめて 1 スタンプ
// 1 回だけ塗る（動かす速さで塗れる量が変わらない）。散布の道具（Scatter /
// Unscatter）は格子を書き換えない（stampRect は空・applyStamp は false）。配置を
// 書き換えるのは TerrainScatter の scatterStamp で、円の並べ方は同じ
// stampDiscCount / stampDisc を使う。
namespace wizengine {
namespace terrain {

enum class BrushTool {
    Raise, Lower, Flatten, Smooth, Hole, Unhole, Paint, Erase, Scatter, Unscatter
};

// 名前（"raise" "lower" "flatten" "smooth" "hole" "unhole" "paint" "erase"
// "scatter" "unscatter"）→ ツール。知らない名前は fallback。
BrushTool brushToolFromName(const std::string& name, BrushTool fallback);

// 塗り分けの重みだけを書き換える道具か（Paint / Erase。高さ・穴は変えない）。
// 呼び手が描画・当たり判定の作り直しを決めるのに使う。
inline bool brushPaintsWeights(BrushTool tool) {
    return tool == BrushTool::Paint || tool == BrushTool::Erase;
}

// 散布物の配置だけを書き換える道具か（Scatter / Unscatter。格子は変えない）。
inline bool brushScatters(BrushTool tool) {
    return tool == BrushTool::Scatter || tool == BrushTool::Unscatter;
}

// 筆の値の範囲（丸めは clampBrush の 1 か所。ブラウザは /scene の limits で
// 受けたこの範囲で筆の欄を丸めて書き戻す）。
constexpr double kBrushMinRadius = 0.1;    // m
constexpr double kBrushMaxRadius = 500.0;  // m
constexpr double kBrushMinStrength = 0.0;
constexpr double kBrushMaxStrength = 100.0;
constexpr double kBrushMaxDt = 0.1;        // s
constexpr double kBrushMinDensity = 0.0;      // 個/m²（散布）
constexpr double kBrushMaxDensity = 10.0;     // 個/m²（散布）
constexpr double kBrushMinSpacing = 0.0;      // m（散布の配置どうしの間隔）
constexpr double kBrushMaxSpacing = 500.0;    // m（散布の配置どうしの間隔）
constexpr double kBrushMinScaleJitter = 0.0;  // 倍率のばらつき（散布）
constexpr double kBrushMaxScaleJitter = 1.0;  // 倍率のばらつき（散布）

// スタンプ 1 個（格子の座標の線分 1 本）。単位:
//   Raise / Lower   … strength = m/s。高さを strength × dt × 減衰 だけ上下する。
//   Flatten / Smooth … strength = 1/s。目標へ min(1, strength × dt) × 減衰 だけ寄せる
//                      （Flatten の目標 = target、Smooth の目標 = 周り 3x3 の平均）。
//   Hole / Unhole   … 即時（中心が半径内のセルを穴にする / 埋める。strength と
//                      dt は見ない）。
//   Paint           … strength = 1/s。中心が半径内のセルの層 layer の重みを、
//                      満杯（重みの合計）との差の min(1, strength × dt) × 減衰
//                      だけ増やし、他の層は同じ割合で減らす（高さ・穴は変えない。
//                      穴のセルは塗らない）。増分の目盛りの端数は seed で決まる
//                      くじで丸める（平均が式どおり = 押し続ければ満杯に届く）。
//                      下地の層（kBaseSplatLayer）と範囲外の層は塗れない
//                      （スタンプごと弾く）。
//   Erase           … 下地の層を塗るのと同じ計算（layer は見ない）。
//   Scatter         … 即時。円の中に原型 prototype の配置を、密度 density
//                      （個/m²）になるまで足す。配置どうしは spacing (m) より
//                      近づけない。倍率は 1 ± scaleJitter の一様（strength と
//                      dt は見ない。計算は TerrainScatter の scatterStamp）。
//   Unscatter       … 即時。円の中の配置を原型によらず全部消す。
// 減衰は中心 1 → 半径 0 の smoothstep。高さは [0, maxHeight] に丸める
// （どちらも格子値 = baseY からの相対高さ。元から範囲の外にある値は、その
// 向きへは押し戻さない）。target は heightAt と同じ値 = **ローカル**の高さ
// （格子は原点基準で、置き場所は TerrainFrame が持つ。呼び手が begin の
// ローカル点の heightAt で決める）。
struct BrushStamp {
    double x0 = 0.0;  // 線分の始点（前回の点。begin では x1 と同じ）
    double z0 = 0.0;
    double x1 = 0.0;  // 線分の終点（今回の点）
    double z1 = 0.0;
    double radius = 5.0;      // m
    double strength = 1.0;    // 単位は上の表
    double dt = 0.0;          // この線分ぶんの時間 (s)
    // 高さの上限（相対）。既定は NaN = 呼び手が必ず渡す（渡し忘れたスタンプは
    // stampRect / applyStamp が何もしない。0 を既定にすると Raise でも 0 へ潰れる）。
    double maxHeight = std::numeric_limits<double>::quiet_NaN();
    // Flatten の目標（heightAt と同じローカルの高さ）。Flatten のときだけ使う
    // （他の道具では NaN でもよい）。
    double target = 0.0;
    BrushTool tool = BrushTool::Raise;
    // Paint で塗る層（0〜kSplatChannels - 1。範囲外と下地の層 = kBaseSplatLayer は
    // Paint ではスタンプごと弾く = 丸めない）。他の道具では見ない。
    int layer = 0;
    // Paint の端数のくじの種。呼び手がスタンプごとに変える（同じ種のままだと、
    // 止めて押し続けたときに毎回同じくじになり、端数ぶんが進まないセルが出る）。
    uint32_t seed = 0;
    // 散布の欄（散布の道具のときだけ見る）。prototype は原型の番号
    // （0〜kMaxScatterPrototypes - 1。範囲外は丸めずにスタンプごと弾く）。
    int prototype = 0;
    double density = 0.05;     // 個/m²（0〜kBrushMaxDensity）
    double spacing = 2.0;      // 配置どうしの最小の間隔 (m、0〜kBrushMaxSpacing)
    double scaleJitter = 0.0;  // 倍率のばらつき（0〜kBrushMaxScaleJitter）
};

// 筆の減衰: 中心からの距離 d の重み（中心 1 → 半径 r 以上で 0、smoothstep で
// 単調に減る）。applyStamp の効き目と、カーソルの円盤の濃さ（TerrainComponent）
// が同じ形になるよう公開している。r は正であること。
double brushFalloff(double d, double r);

// 半径 kBrushMinRadius〜kBrushMaxRadius、強さ 0〜kBrushMaxStrength、
// dt 0〜kBrushMaxDt、maxHeight 0 以上、density 0〜kBrushMaxDensity、spacing 0〜
// kBrushMaxSpacing、scaleJitter 0〜kBrushMaxScaleJitter に丸める。NaN は丸めずに
// 残す（brushStampUsable が弾く = 壊れた入力で格子も配置も書き換えない）。
// layer と prototype は丸めない（範囲外は brushStampUsable が弾く）。
BrushStamp clampBrush(BrushStamp s);

// 当ててよいスタンプか（使う値が全部有限、半径が正、maxHeight が負でない =
// std::clamp の lo <= hi を守る）。maxHeight を渡し忘れた（既定の NaN）
// スタンプもここで弾く。target は Flatten のときだけ、layer は Paint のときだけ、
// prototype と散布の 3 欄は散布の道具のときだけ見る（Erase は下地の層に
// 決まっているので layer を見ない）。範囲外の layer / prototype は丸めずに弾く
// （丸めると別の層を塗る・別の原型を置く）。Paint は下地の層
// （kBaseSplatLayer）を塗れない: 下地を塗る = 消しゴムの役目で、layer を送り
// 忘れた（既定 0 の）塗りが黙って消しゴムになるのを防ぐ。
// stampRect / applyStamp / scatterStamp が使う。
bool brushStampUsable(const BrushStamp& s);

// スタンプの線分に並べる円の数（1 以上）。円の間隔は半径の 1/4 で、上限
// （256 個）を超える長さでは間隔が広がる。長さ 0（begin）なら 1 個。
int stampDiscCount(const BrushStamp& s);

// スタンプの円 1 個（格子の座標）。「円の中」の規則はここ 1 か所（筆の格子の
// 道具も散布も使う）: 中心からの距離の二乗が r² 未満。平方根は重みを出す
// ときだけ。
struct StampDisc {
    double cx, cz, r;
    double r2;  // r * r
    double distanceSq(double x, double z) const {
        const double dx = x - cx, dz = z - cz;
        return dx * dx + dz * dz;
    }
    bool contains(double x, double z) const { return distanceSq(x, z) < r2; }
    // 減衰の重み（brushFalloff）。半径の外は 0。
    double weight(double x, double z) const {
        const double d2 = distanceSq(x, z);
        return d2 < r2 ? brushFalloff(std::sqrt(d2), r) : 0.0;
    }
};

// k 番目（1〜discs。discs = stampDiscCount の値）の円。始点は前回のスタンプが
// 当て済みなので k = 0 には当てず、k = discs が終点。半径はスタンプの半径。
StampDisc stampDisc(const BrushStamp& s, int discs, int k);

// スタンプの線分の外接矩形 ± margin（格子の座標、境界を含む）。
struct StampBounds {
    double x0, z0, x1, z1;
    bool contains(double x, double z) const { return x >= x0 && x <= x1 && z >= z0 && z <= z1; }
};
// 「スタンプの届く範囲」の定義はここ 1 か所（stampRect は margin = 半径、
// 散布の近くの配置集めは半径 + 間隔）。
StampBounds stampBounds(const BrushStamp& s, double margin);

// スタンプが書き換えうるセルの矩形（セル単位、0〜resolution に切り詰め済み。
// TerrainEdit の rect と同じ約束）。線分の外接矩形 ± 半径に入るサンプル k を
// 角に持つセル（k − 1 と k）と、中心が入るセルを包む。空の格子・有限でない
// 値・格子の外・散布の道具（格子を書き換えない）なら空。
GridRect stampRect(const HeightField& field, const BrushStamp& stamp);

// スタンプを当てる。格子の値が実際に 1 つでも変われば true（強さか dt が 0 の
// 高さの道具は何も書かずに false。散布の道具も false）。範囲は stampRect の
// 中に収まる。
// 共有中の格子（shared_ptr<const>）には呼べない = コピーに対して呼ぶ。
bool applyStamp(HeightField& field, const BrushStamp& stamp);
// 同じ。scratch は作業用の領域（Smooth の写し・Paint の重みの 1 枚）で、続けて
// 当てる呼び手が使い回せば、スタンプごとに確保しない。中身は呼ぶたびに作り直す。
bool applyStamp(HeightField& field, const BrushStamp& stamp, std::vector<float>& scratch);

}  // namespace terrain
}  // namespace wizengine
