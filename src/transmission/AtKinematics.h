#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "transmission/AtTypes.h"

// AT の歯車列の構造（名前の解決・段の変速比・要素の分担）。純粋な計算。
//
// 動力学（Chrono のシャフト）を組む側（components/AtDriveline）と、変速の判断
// （transmission/AtControl）と、文書の検査（transmission/AtXml）が同じ答えを使う。
//
//   * 軸は desc.shafts の並び（0..）、ケースは -1
//   * 遊星は Willis の式 ωs + k ωr − (1 + k) ωc = 0（k = zr / zs）
//   * 段の変速比 = 遊星の式 + 締結した要素（軸の相対回転 0）+ 入力 = 1 を解いた 1 / ω_out
//   * 要素の分担 = 入力に 1 N·m、出力を止めたときの要素の反トルク（静的な釣り合い）。
//     変速の油圧（分担 × 入力トルク × 余裕）と、容量 0 の要素の自動決定に使う
namespace wizengine {
namespace transmission {

class AtKinematics {
public:
    struct Gearset {
        int s = -1, c = -1, r = -1;  // 軸の番号
        double k = 2.0, eff = 0.985;
    };
    struct Element {
        int a = -1, b = -1;  // b = -1 はケース（ブレーキ）
        double cap = 0.0;    // 満圧の容量 N·m（自動のものは決めた値）
    };
    struct Gear {
        std::vector<int> elements;
        double ratio = 0.0;  // ω_in / ω_out（R は負。拘束が足りない / 余る段は 0）
        int number = 0;      // 1..、R = -1
    };

    explicit AtKinematics(const AtDesc& desc);

    bool ok() const { return errors_.empty(); }
    const std::vector<std::string>& errors() const { return errors_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

    std::size_t shaftCount() const { return shaftCount_; }
    int inputShaft() const { return in_; }
    int outputShaft() const { return out_; }
    const std::vector<Gearset>& gearsets() const { return sets_; }
    const std::vector<Element>& elements() const { return elements_; }
    const std::vector<Gear>& gears() const { return gears_; }
    const std::vector<int>& neutral() const { return neutral_; }
    // 前進段（番号順の段の添字）と後退段（-1 = 無い）。
    const std::vector<int>& forward() const { return forward_; }
    int reverse() const { return gearR_; }
    // 段 g の要素 e の分担（入力 1 N·m あたりの要素のトルク。締結していない要素は 0）。
    double factor(std::size_t g, std::size_t e) const;
    // 段 from → to が要素 1 つの掛け替えか。
    bool singleSwap(int from, int to) const;

private:
    bool staticFactors(std::size_t g, std::vector<double>& out) const;

    std::size_t shaftCount_ = 0;
    int in_ = -1, out_ = -1;
    std::vector<Gearset> sets_;
    std::vector<Element> elements_;
    std::vector<Gear> gears_;
    std::vector<int> neutral_, forward_;
    int gearR_ = -1;
    std::vector<std::vector<double>> factors_;
    std::vector<std::string> errors_, warnings_;
};

}  // namespace transmission
}  // namespace wizengine
