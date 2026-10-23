#pragma once

#include <limits>
#include <string>

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
// 1 個だけ）。dt は円の数で等分する = 線の長さで効き目の総量が変わらない。
namespace wizengine {
namespace terrain {

enum class BrushTool { Raise, Lower, Flatten, Smooth, Hole, Unhole };

// 名前（"raise" "lower" "flatten" "smooth" "hole" "unhole"）→ ツール。
// 知らない名前は fallback。
BrushTool brushToolFromName(const std::string& name, BrushTool fallback);

// 筆の値の範囲（丸めは clampBrush の 1 か所。ブラウザの min / max は表示用）。
constexpr double kBrushMinRadius = 0.1;    // m
constexpr double kBrushMaxRadius = 500.0;  // m
constexpr double kBrushMaxStrength = 100.0;
constexpr double kBrushMaxDt = 0.1;        // s

// スタンプ 1 個（格子の座標の線分 1 本）。単位:
//   Raise / Lower   … strength = m/s。高さを strength × dt × 減衰 だけ上下する。
//   Flatten / Smooth … strength = 1/s。目標へ min(1, strength × dt) × 減衰 だけ寄せる
//                      （Flatten の目標 = target、Smooth の目標 = 周り 3x3 の平均）。
//   Hole / Unhole   … 即時（中心が半径内のセルを穴にする / 埋める。strength と
//                      dt は見ない）。
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
};

// 筆の減衰: 中心からの距離 d の重み（中心 1 → 半径 r 以上で 0、smoothstep で
// 単調に減る）。applyStamp の効き目と、カーソルの円盤の濃さ（TerrainComponent）
// が同じ形になるよう公開している。r は正であること。
double brushFalloff(double d, double r);

// 半径 kBrushMinRadius〜kBrushMaxRadius、強さ 0〜kBrushMaxStrength、
// dt 0〜kBrushMaxDt、maxHeight 0 以上に丸める。NaN は丸めずに残す
// （stampRect / applyStamp が何もしない = 壊れた入力で格子を書き換えない）。
BrushStamp clampBrush(BrushStamp s);

// スタンプが書き換えうるセルの矩形（セル単位、0〜resolution に切り詰め済み。
// TerrainEdit の rect と同じ約束）。線分の外接矩形 ± 半径に入るサンプル k を
// 角に持つセル（k − 1 と k）と、中心が入るセルを包む。空の格子・有限でない
// 値・格子の外なら空。
GridRect stampRect(const HeightField& field, const BrushStamp& stamp);

// スタンプを当てる。格子の値が実際に 1 つでも変われば true（強さか dt が 0 の
// 高さの道具は何も書かずに false）。範囲は stampRect の中に収まる。
// 共有中の格子（shared_ptr<const>）には呼べない = コピーに対して呼ぶ。
bool applyStamp(HeightField& field, const BrushStamp& stamp);

}  // namespace terrain
}  // namespace wizengine
