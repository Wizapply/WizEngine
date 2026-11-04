#include "terrain/TerrainScatter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/MathConstants.h"
#include "terrain/TerrainBrush.h"
#include "terrain/TerrainHash.h"
#include "terrain/TerrainMath.h"

namespace wizengine {
namespace terrain {
namespace {

// 散布の筆が見る配置 1 つ（格子のローカルの位置と原型）。
struct Placed {
    double x, z;
    int prototype;
};

// Unscatter: どれかの円の中（StampDisc::contains）にある配置を消す。消した数を返す。
std::size_t removeInDiscs(const HeightField& field, const BrushStamp& s, TerrainScatter& out) {
    const int discs = stampDiscCount(s);
    const StampBounds box = stampBounds(s, s.radius);
    const std::size_t before = out.instances.size();
    out.instances.erase(
        std::remove_if(out.instances.begin(), out.instances.end(),
                       [&](const ScatterInstance& in) {
                           const double x = scatterLocalX(field, in.u);
                           const double z = scatterLocalZ(field, in.v);
                           if (!box.contains(x, z)) return false;
                           for (int k = 1; k <= discs; ++k) {
                               if (stampDisc(s, discs, k).contains(x, z)) return true;
                           }
                           return false;
                       }),
        out.instances.end());
    return before - out.instances.size();
}

// 円 k の目標の個数: expected（= density × π r²、上限で抑えた値）の端数をくじで
// 丸めたもの（roundByLottery）。くじは seed と円の番号から決まる（試行のくじとは
// 別の散らし方）。
int discTarget(double expected, uint32_t seed, int k) {
    const double lottery = hashUnit(mix32(mix32(seed) ^ (uint32_t(k) * 0x85EBCA6Bu)));
    return int(roundByLottery(expected, lottery));
}

// Scatter: 円ごとに不足分を足す（ヘッダの約束）。stats に足した数と capped を書く。
void addInDiscs(const HeightField& field, const BrushStamp& s, TerrainScatter& out,
                ScatterStampStats& stats) {
    const int discs = stampDiscCount(s);
    const double r = s.radius;
    // 近くの配置を 1 回だけ集める（円の中の数え上げ = ± r、間隔の検査 = ± r +
    // spacing）。足した配置もここへ足す（後の円・試行が見る）。
    std::vector<Placed> nearby;
    const StampBounds box = stampBounds(s, r + s.spacing);
    for (const ScatterInstance& in : out.instances) {
        const double x = scatterLocalX(field, in.u);
        const double z = scatterLocalZ(field, in.v);
        if (box.contains(x, z)) nearby.push_back({x, z, int(in.prototype)});
    }
    const double expected = std::min(s.density * kPi * r * r, double(kScatterMaxPerDisc));
    if (!(expected > 0.0)) return;
    int tries = 0;  // スタンプ全体の試行の通し番号（くじの番号も兼ねる）
    for (int k = 1; k <= discs && tries < kScatterMaxTriesPerStamp; ++k) {
        const StampDisc disc = stampDisc(s, discs, k);
        const int target = discTarget(expected, s.seed, k);
        int have = 0;
        for (const Placed& p : nearby)
            if (p.prototype == s.prototype && disc.contains(p.x, p.z)) ++have;
        const int missing = target - have;
        if (missing <= 0) continue;
        const int limit = std::min(tries + 4 * missing, kScatterMaxTriesPerStamp);
        for (; tries < limit && have < target; ++tries) {
            // くじ 4 本（位置の半径・角度・向き・倍率）。同じ入力なら同じ値。
            const uint32_t base =
                mix32(s.seed ^ uint32_t(k) ^ (uint32_t(tries) * 0x9E3779B9u));
            const double xi1 = hashUnit(base);
            const double xi2 = hashUnit(mix32(base ^ 0x1u));
            const double xi3 = hashUnit(mix32(base ^ 0x2u));
            const double xi4 = hashUnit(mix32(base ^ 0x3u));
            // 円の中の一様な点（半径は平方根で取る = 面積あたり一様）。
            const double rr = r * std::sqrt(xi1);
            const double theta = 2.0 * kPi * xi2;
            const double x = disc.cx + rr * std::cos(theta);
            const double z = disc.cz + rr * std::sin(theta);
            if (!field.solidAt(x, z)) continue;  // 穴・格子の外
            // 間隔の円（中心 = 候補、半径 = spacing）の中に配置があれば置かない。
            const StampDisc room{x, z, s.spacing, s.spacing * s.spacing};
            bool crowded = false;
            for (const Placed& p : nearby) {
                if (room.contains(p.x, p.z)) {
                    crowded = true;
                    break;
                }
            }
            if (crowded) continue;
            double u = 0.0, v = 0.0;
            if (!scatterUvAt(field, x, z, u, v)) continue;
            if (out.instances.size() >= std::size_t(kMaxScatterInstances)) {
                stats.capped = true;
                return;
            }
            ScatterInstance in;
            in.u = float(u);
            in.v = float(v);
            in.yaw = float(2.0 * kPi * xi3);
            in.scale = float(std::max(1.0 + s.scaleJitter * (2.0 * xi4 - 1.0),
                                      double(kScatterScaleMin)));
            in.prototype = uint8_t(s.prototype);
            out.instances.push_back(in);
            // 間隔の検査は保存した (u, v) の位置で（後のスタンプが見る位置と揃える）。
            nearby.push_back({scatterLocalX(field, in.u), scatterLocalZ(field, in.v), s.prototype});
            ++stats.added;
            ++have;
        }
    }
}

}  // namespace

double scatterLocalX(const HeightField& field, double u) {
    return double(field.minX()) + u * (field.maxX() - double(field.minX()));
}

double scatterLocalZ(const HeightField& field, double v) {
    return double(field.minZ()) + v * (field.maxZ() - double(field.minZ()));
}

bool scatterUvAt(const HeightField& field, double x, double z, double& u, double& v) {
    if (!field.contains(x, z)) return false;  // 空の格子・NaN・無限もここで弾く
    const double x0 = double(field.minX());
    const double z0 = double(field.minZ());
    // 境界の丸めで 0〜1 をわずかに外れないよう丸める（範囲の判定は contains）。
    u = std::clamp((x - x0) / (field.maxX() - x0), 0.0, 1.0);
    v = std::clamp((z - z0) / (field.maxZ() - z0), 0.0, 1.0);
    return true;
}

bool scatterTrunk(const HeightField& field, const ScatterPrototype& proto,
                  const ScatterInstance& inst, ScatterTrunk& out) {
    const double x = scatterLocalX(field, inst.u);
    const double z = scatterLocalZ(field, inst.v);
    if (!field.solidAt(x, z)) return false;  // 穴・格子の外・空の格子・NaN
    out.x = x;
    out.z = z;
    out.ground = field.heightAt(x, z);
    out.radius = double(proto.trunkRadius) * double(inst.scale);
    out.height = double(proto.trunkHeight) * double(inst.scale);
    return true;
}

double scatterModelScale(const ScatterPrototype& proto, const ScatterInstance& inst) {
    return double(proto.scale) * double(inst.scale);
}

const ScatterPrototype* scatterCollidingPrototype(const TerrainScatter& s,
                                                  const ScatterInstance& inst) {
    // 番号を検査する前の値でも原型の配列を読み越さない。
    if (int(inst.prototype) >= kMaxScatterPrototypes) return nullptr;
    const ScatterPrototype& proto = s.prototypes[inst.prototype];
    if (!proto.collide || !(proto.trunkRadius > 0.0f) || !(proto.trunkHeight > 0.0f)) return nullptr;
    return &proto;
}

bool scatterTouches(const TerrainScatter& s, const HeightField& field, const GridRect& cells) {
    if (cells.empty()) return false;
    // 余白は足さない: stampRect の矩形は書き換えたサンプルを角に持つセルを全部含む。
    for (const ScatterInstance& inst : s.instances) {
        if (!scatterCollidingPrototype(s, inst)) continue;
        int i = 0, j = 0;
        if (!field.cellAt(scatterLocalX(field, inst.u), scatterLocalZ(field, inst.v), i, j)) continue;
        if (i >= cells.i0 && i < cells.i1 && j >= cells.j0 && j < cells.j1) return true;
    }
    return false;
}

ScatterPrototype clampScatterPrototype(ScatterPrototype p) {
    const ScatterPrototype d;
    p.scale = float(clampOr(p.scale, kScatterScaleMin, kScatterScaleMax, d.scale));
    p.trunkRadius = float(clampOr(p.trunkRadius, 0.0, kScatterTrunkMax, d.trunkRadius));
    p.trunkHeight = float(clampOr(p.trunkHeight, 0.0, kScatterTrunkMax, d.trunkHeight));
    return p;
}

std::size_t pruneScatterInstances(TerrainScatter& s) {
    const std::size_t before = s.instances.size();
    auto unit = [](float t) { return std::isfinite(t) && t >= 0.0f && t <= 1.0f; };
    s.instances.erase(
        std::remove_if(s.instances.begin(), s.instances.end(),
                       [&](const ScatterInstance& i) {
                           const bool usable =
                               unit(i.u) && unit(i.v) && std::isfinite(i.scale) &&
                               i.scale > 0.0f && std::isfinite(i.yaw) &&
                               int(i.prototype) < kMaxScatterPrototypes;
                           return !usable;
                       }),
        s.instances.end());
    if (s.instances.size() > std::size_t(kMaxScatterInstances)) {
        s.instances.resize(std::size_t(kMaxScatterInstances));
    }
    return before - s.instances.size();
}

bool scatterStamp(const HeightField& field, const BrushStamp& stamp, TerrainScatter& s,
                  ScatterStampStats* stats) {
    ScatterStampStats local;
    ScatterStampStats& st = stats ? *stats : local;
    st = ScatterStampStats{};
    if (!field.valid() || !brushStampUsable(stamp) || !brushScatters(stamp.tool)) return false;
    if (stamp.tool == BrushTool::Unscatter) {
        st.removed = removeInDiscs(field, stamp, s);
    } else {
        addInDiscs(field, stamp, s, st);
    }
    return st.added > 0 || st.removed > 0;
}

}  // namespace terrain
}  // namespace wizengine
