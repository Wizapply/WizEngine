#include "terrain/TerrainContact.h"

#include <algorithm>
#include <cmath>

#include <Eigen/Geometry>

namespace wizengine {
namespace terrain {

namespace {

constexpr double kPi = 3.14159265358979323846;
// 円柱の 1 つの端のリムの点数。
constexpr int kCylinderRimPoints = 12;
// 箱の面サンプルの上限（各面 k x k）。
constexpr int kMaxFaceSamples = 16;

int faceSampleCount(const ContactParams& params) {
    return std::clamp(params.faceSamples, 0, kMaxFaceSamples);
}

// 形のローカルの点を順に fn(番号, 点) へ渡す。番号は形の中で 0 から連番で、
// 並びは固定（= pointId が姿勢に依らず同じ点を指す）。
//   箱:   8 角（x, y, z の符号 −/+、x が外側のループ = ベンチと同じ並び）→
//         面サンプル（各面 k x k の内点。軸 0..2 × 符号 −/+ × i × j。ベンチの
//         boxgrid と同じ並び）
//   球:   中心 1 点
//   円柱: 端 −X のリム 12 点 → 端 +X のリム 12 点 → 端 −X の中心 → 端 +X の中心
//   Points: そのままの順
template <typename Fn>
void forEachLocalPoint(const ShapeProxy& s, const ContactParams& params, Fn&& fn) {
    switch (s.kind) {
    case ShapeProxy::Kind::Box: {
        const Vec3& h = s.halfExtents;
        int id = 0;
        for (int a = -1; a <= 1; a += 2)
            for (int c = -1; c <= 1; c += 2)
                for (int e = -1; e <= 1; e += 2) fn(id++, Vec3(a * h.x(), c * h.y(), e * h.z()));
        const int k = faceSampleCount(params);
        for (int ax = 0; ax < 3; ++ax)
            for (int sgn = -1; sgn <= 1; sgn += 2)
                for (int i = 0; i < k; ++i)
                    for (int j = 0; j < k; ++j) {
                        Vec3 v;
                        v[ax] = sgn;
                        v[(ax + 1) % 3] = (i + 1.0) / (k + 1) * 2 - 1;
                        v[(ax + 2) % 3] = (j + 1.0) / (k + 1) * 2 - 1;
                        fn(id++, Vec3(v.cwiseProduct(h)));
                    }
        break;
    }
    case ShapeProxy::Kind::Sphere:
        fn(0, Vec3::Zero());
        break;
    case ShapeProxy::Kind::Cylinder: {
        int id = 0;
        for (int end = -1; end <= 1; end += 2)
            for (int k = 0; k < kCylinderRimPoints; ++k) {
                const double a = 2.0 * kPi * k / kCylinderRimPoints;
                fn(id++, Vec3(end * s.halfLength, s.radius * std::cos(a), s.radius * std::sin(a)));
            }
        fn(id++, Vec3(-s.halfLength, 0.0, 0.0));
        fn(id++, Vec3(s.halfLength, 0.0, 0.0));
        break;
    }
    case ShapeProxy::Kind::Points:
        for (int i = 0; i < int(s.points.size()); ++i) fn(i, s.points[size_t(i)]);
        break;
    }
}

}  // namespace

ContactTerrain::ContactTerrain(const HeightField& f)
    : field(&f), topY(double(f.baseY()) + double(f.maxSample())) {
    if (!f.valid()) return;
    // 全セルの 2 三角形の面法線の y の最小値。normalAt と同じ勾配の式
    // （slopeNormal(dhdu / cell, dhdw / cell) の y = 1 / sqrt(1 + gx² + gz²)）。
    // 穴のセルも含める（保守側 = 小さめになるだけ）。
    const double cell = double(f.cellSize());
    const int res = f.resolution();
    double ny = 1.0;
    for (int j = 0; j < res; ++j) {
        for (int i = 0; i < res; ++i) {
            const double h00 = f.sample(i, j), h10 = f.sample(i + 1, j);
            const double h01 = f.sample(i, j + 1), h11 = f.sample(i + 1, j + 1);
            const double grads[2][2] = {{h11 - h01, h01 - h00},   // 三角形 A
                                        {h10 - h00, h11 - h10}};  // 三角形 B
            for (const auto& g : grads) {
                const double gx = g[0] / cell, gz = g[1] / cell;
                ny = std::min(ny, 1.0 / std::sqrt(1.0 + gx * gx + gz * gz));
            }
        }
    }
    // normalAt は正規化の丸めで最後の桁が違いうるので、ほんの少し下げておく。
    minNormalY = ny * (1.0 - 1e-9);
}

int shapePointCount(const ShapeProxy& shape, const ContactParams& params) {
    switch (shape.kind) {
    case ShapeProxy::Kind::Box: {
        const int k = faceSampleCount(params);
        return 8 + 6 * k * k;
    }
    case ShapeProxy::Kind::Sphere: return 1;
    case ShapeProxy::Kind::Cylinder: return 2 * kCylinderRimPoints + 2;
    case ShapeProxy::Kind::Points: return int(shape.points.size());
    }
    return 0;
}

int proxyPointCount(const BodyProxy& proxy, const ContactParams& params) {
    int n = 0;
    for (const auto& s : proxy.shapes) n += shapePointCount(s, params);
    return n;
}

double computeBoundRadius(const BodyProxy& proxy) {
    double r = 0.0;
    for (const auto& s : proxy.shapes) {
        double extent = 0.0;
        switch (s.kind) {
        case ShapeProxy::Kind::Box: extent = s.halfExtents.norm(); break;
        case ShapeProxy::Kind::Sphere: extent = std::fabs(s.radius); break;
        case ShapeProxy::Kind::Cylinder:
            extent = std::sqrt(s.halfLength * s.halfLength + s.radius * s.radius);
            break;
        case ShapeProxy::Kind::Points:
            for (const auto& p : s.points) extent = std::max(extent, p.norm());
            break;
        }
        // 回転は長さを変えないので、原点からのずれ + 形の広がりが上限。
        r = std::max(r, s.position.norm() + extent);
    }
    return r;
}

int collectContacts(const ContactTerrain& terrain, const BodyProxy& proxy, const Pose& pose,
                    const BodyVelocity& velocity, const ContactParams& params,
                    std::vector<Contact>& out) {
    out.clear();
    const HeightField* f = terrain.field;
    if (f == nullptr || !f->valid() || proxy.shapes.empty()) return 0;
    if (!pose.position.allFinite() || !pose.rotation.allFinite() || !velocity.linear.allFinite() ||
        !velocity.angular.allFinite() || !std::isfinite(terrain.topY) ||
        !std::isfinite(terrain.minNormalY))
        return 0;
    if (!std::isfinite(proxy.boundRadius) || proxy.boundRadius < 0.0) return 0;
    if (!std::isfinite(params.envelope) || !std::isfinite(params.approachSpeedFactor) ||
        !std::isfinite(params.dt))
        return 0;

    const double env = params.envelope;
    // 速度項は倍率と dt が両方正のときだけ（ベンチの adapt > 0 と同じ）。
    const bool speedTerm = params.approachSpeedFactor > 0.0 && params.dt > 0.0;
    const double factorDt = speedTerm ? params.approachSpeedFactor * params.dt : 0.0;

    // 早期棄却。「どの点の gap も、どの点の閾値より大きい」と言えるときだけ捨てる。
    //   閾値の側: どの点の速度も |linear| + |angular|·R（R = boundRadius）を超えない
    //     ので、点ごとの閾値 th ≤ thMax = env + factor·dt·(|linear| + |angular|·R)。
    //   gap の側: 点（球なら中心）p、半径 r（球以外は 0）とする。形は原点から R
    //     の球に収まるので p.y ≥ pos.y − R + r。地表は h ≤ topY、面法線は
    //     n.y ≥ ny（= minNormalY > 0）。D = pos.y − R − topY > 0 なら
    //     p.y − h ≥ D + r > 0 なので
    //       gap = (p.y − h)·n.y − r ≥ (D + r)·ny − r = D·ny − r·(1 − ny)
    //           ≥ D·ny − R·(1 − ny)        （r ≤ R、1 − ny ≥ 0）。
    //   よって D > 0 かつ D·ny − R·(1 − ny) > thMax なら全点で gap > th = 無接触。
    // 鉛直の隙間 D だけで比べると、斜面（n.y < 1）では平面距離の gap の方が
    // 小さいので、丘の頂上付近で接触を取りこぼす（ガタつく）。
    const double bound = proxy.boundRadius;
    const double vMax = velocity.linear.norm() + velocity.angular.norm() * bound;
    const double thMax = env + factorDt * vMax;
    const double ny = std::clamp(terrain.minNormalY, 0.0, 1.0);
    const double D = pose.position.y() - bound - terrain.topY;
    if (D > 0.0 && D * ny - bound * (1.0 - ny) > thMax) return 0;

    int base = 0;
    for (int si = 0; si < int(proxy.shapes.size()); ++si) {
        const ShapeProxy& s = proxy.shapes[size_t(si)];
        const bool round = s.kind == ShapeProxy::Kind::Sphere;
        const double radius = round ? s.radius : 0.0;
        const Mat3 rot = pose.rotation * s.rotation;
        const Vec3 origin = pose.position + pose.rotation * s.position;
        if (!round || (std::isfinite(radius) && radius >= 0.0)) {
            forEachLocalPoint(s, params, [&](int localId, const Vec3& lp) {
                const Vec3 p = origin + rot * lp;
                if (!p.allFinite()) return;
                // solidAt は格子の外・穴で false（contains を含む）。
                if (!f->solidAt(p.x(), p.z())) return;
                const double h = f->heightAt(p.x(), p.z());
                const Vec3 n = f->normalAt(p.x(), p.z());
                const double gap = (p.y() - h) * n.y() - radius;
                double th = env;
                if (speedTerm) {
                    // 点の速度 = 並進 + 角速度 × 原点からの腕（球は中心の速度）。
                    const Vec3 vp = velocity.linear + velocity.angular.cross(p - pose.position);
                    th += factorDt * std::max(0.0, -vp.dot(n));
                }
                if (!(gap < th)) return;
                Contact c;
                c.onBody = p - n * radius;
                c.onTerrain = c.onBody - n * gap;
                c.normal = n;
                c.gap = gap;
                c.shape = si;
                c.pointId = base + localId;
                c.round = round;
                out.push_back(c);
            });
        }
        base += shapePointCount(s, params);
    }

    std::sort(out.begin(), out.end(), [](const Contact& a, const Contact& b) {
        if (a.gap != b.gap) return a.gap < b.gap;
        return a.pointId < b.pointId;
    });
    if (params.maxPerBody > 0 && int(out.size()) > params.maxPerBody)
        out.resize(size_t(params.maxPerBody));
    return int(out.size());
}

}  // namespace terrain
}  // namespace wizengine
