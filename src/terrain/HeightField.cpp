#include "terrain/HeightField.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Eigen/Geometry>  // cross

namespace wizengine {
namespace terrain {
namespace {

// 長さがほぼ 0 なら fallback を返す正規化（Eigen の normalized() は 0 を
// そのまま返すので、しきい値と代わりの値はここで持つ）。
Vec3 normalizedOr(const Vec3& v, const Vec3& fallback) {
    const double l = v.norm();
    if (!(l > 1e-12)) return fallback;
    return v / l;
}

// 勾配 (dh/dx, dh/dz) の面の法線。
Vec3 slopeNormal(double dhdx, double dhdz) {
    return normalizedOr(Vec3(-dhdx, 1.0, -dhdz), Vec3::UnitY());
}

// 三角形の重心座標の許容差。セルの境界・対角線ちょうどを通るレイが両側の
// 三角形の「すき間」に落ちないよう、わずかに外側も当たりとみなす。
constexpr double kBaryTolerance = 1e-7;
// DDA でセルの出入りの t に足す余裕（m）。境界ちょうどの交点を両側で拾う。
constexpr double kCellSlack = 1e-6;

// 片面の Möller–Trumbore。(a, b, c) は上から見て反時計回り = 法線が上。
// 上から入るレイ（dir と法線が逆向き）だけを当たりにする。
bool hitTriangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& o,
                 const Vec3& d, double tMin, double tMax, double& tOut,
                 Vec3& normalOut) {
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 n = e1.cross(e2);
    if (!(n.dot(d) < 0.0)) return false;  // 裏から・面と平行
    const Vec3 p = d.cross(e2);
    const double det = e1.dot(p);
    if (std::fabs(det) < 1e-18) return false;
    const double inv = 1.0 / det;
    const Vec3 s = o - a;
    const double u = s.dot(p) * inv;
    if (u < -kBaryTolerance || u > 1.0 + kBaryTolerance) return false;
    const Vec3 q = s.cross(e1);
    const double v = d.dot(q) * inv;
    if (v < -kBaryTolerance || u + v > 1.0 + kBaryTolerance) return false;
    const double t = e2.dot(q) * inv;
    if (t < tMin || t > tMax) return false;
    tOut = t;
    normalOut = normalizedOr(n, Vec3::UnitY());
    return true;
}

}  // namespace

HeightField::HeightField(int resolution, float size, float centerX, float baseY,
                         float centerZ) {
    if (resolution < 1 || resolution > kMaxResolution || !(size > 0.0f) ||
        !std::isfinite(size) ||
        !std::isfinite(centerX) || !std::isfinite(baseY) || !std::isfinite(centerZ)) {
        return;  // 空のまま
    }
    resolution_ = resolution;
    size_ = size;
    cell_ = size / float(resolution);
    minX_ = centerX - size * 0.5f;
    minZ_ = centerZ - size * 0.5f;
    baseY_ = baseY;
    const std::size_t side = std::size_t(resolution) + 1;
    heights_.assign(side * side, 0.0f);
}

float HeightField::sample(int i, int j) const {
    if (!valid()) return 0.0f;
    i = std::clamp(i, 0, resolution_);
    j = std::clamp(j, 0, resolution_);
    return heights_[std::size_t(j) * std::size_t(resolution_ + 1) + std::size_t(i)];
}

void HeightField::setSample(int i, int j, float h) {
    if (!valid() || i < 0 || j < 0 || i > resolution_ || j > resolution_) return;
    heights_[std::size_t(j) * std::size_t(resolution_ + 1) + std::size_t(i)] = h;
}

float HeightField::minSample() const {
    if (heights_.empty()) return 0.0f;
    return *std::min_element(heights_.begin(), heights_.end());
}

float HeightField::maxSample() const {
    if (heights_.empty()) return 0.0f;
    return *std::max_element(heights_.begin(), heights_.end());
}

bool HeightField::contains(double x, double z) const {
    // NaN はどの比較も偽 = 範囲外。
    return valid() && x >= minX_ && x <= maxX() && z >= minZ_ && z <= maxZ();
}

void HeightField::setHole(int i, int j, bool hole) {
    if (i < 0 || j < 0 || i >= resolution_ || j >= resolution_) return;
    if (holes_.empty()) {
        if (!hole) return;  // 穴が無いまま = 確保しない
        holes_.assign(std::size_t(resolution_) * std::size_t(resolution_), uint8_t(0));
    }
    holes_[std::size_t(j) * std::size_t(resolution_) + std::size_t(i)] = hole ? 1 : 0;
}

bool HeightField::holeAt(int i, int j) const {
    if (holes_.empty() || i < 0 || j < 0 || i >= resolution_ || j >= resolution_) return false;
    return holes_[std::size_t(j) * std::size_t(resolution_) + std::size_t(i)] != 0;
}

bool HeightField::anyHoles() const {
    return std::any_of(holes_.begin(), holes_.end(), [](uint8_t h) { return h != 0; });
}

void HeightField::clearHoles() { holes_.clear(); }

namespace {

constexpr std::size_t kSplatTexelBytes = std::size_t(kSplatChannels);

}  // namespace

std::size_t HeightField::splatOffset(int i, int j) const {
    return (std::size_t(j) * std::size_t(resolution_) + std::size_t(i)) * kSplatTexelBytes;
}

void HeightField::allocateSplat() {
    const std::size_t cells = std::size_t(resolution_) * std::size_t(resolution_);
    splat_.resize(cells * kSplatTexelBytes);
    for (std::size_t k = 0; k < cells; ++k)
        std::copy(kDefaultSplatTexel.begin(), kDefaultSplatTexel.end(),
                  splat_.begin() + std::ptrdiff_t(k * kSplatTexelBytes));
}

void HeightField::splatAt(int i, int j, uint8_t out[kSplatChannels]) const {
    if (splat_.empty() || i < 0 || j < 0 || i >= resolution_ || j >= resolution_) {
        std::copy(kDefaultSplatTexel.begin(), kDefaultSplatTexel.end(), out);
        return;
    }
    const std::size_t at = splatOffset(i, j);
    std::copy_n(splat_.begin() + std::ptrdiff_t(at), kSplatChannels, out);
}

bool HeightField::setSplatWeights(int i, int j, const float w[kSplatChannels]) {
    if (i < 0 || j < 0 || i >= resolution_ || j >= resolution_) return false;
    if (holeAt(i, j)) return false;  // 穴のセルは塗らない（ヘッダの約束）
    // 負・NaN・無限大は 0（無限大を残すと合計が無限大になり比が出せない）。
    double v[kSplatChannels];
    double sum = 0.0;
    for (int k = 0; k < kSplatChannels; ++k) {
        v[k] = (std::isfinite(w[k]) && w[k] > 0.0f) ? double(w[k]) : 0.0;
        sum += v[k];
    }
    std::array<uint8_t, kSplatChannels> q = kDefaultSplatTexel;
    if (sum > 0.0) {
        // 合計 255 への最大剰余法: 切り捨ててから、足りない分を端数の大きい層へ。
        double frac[kSplatChannels];
        int total = 0;
        for (int k = 0; k < kSplatChannels; ++k) {
            const double s = std::min(v[k] * 255.0 / sum, 255.0);
            const double f = std::floor(s);
            q[std::size_t(k)] = uint8_t(f);
            frac[k] = s - f;
            total += int(f);
        }
        for (int left = 255 - total; left > 0; --left) {
            int best = 0;
            for (int k = 1; k < kSplatChannels; ++k)
                if (frac[k] > frac[best]) best = k;  // 同じ端数なら番号の小さい層
            ++q[std::size_t(best)];
            frac[best] = -1.0;
        }
    }
    uint8_t cur[kSplatChannels];
    splatAt(i, j, cur);
    if (std::equal(q.begin(), q.end(), cur)) return false;
    if (splat_.empty()) allocateSplat();
    const std::size_t at = splatOffset(i, j);
    std::copy(q.begin(), q.end(), splat_.begin() + std::ptrdiff_t(at));
    return true;
}

bool HeightField::copySplat(const GridRect& cells, uint8_t* out) const {
    if (!valid() || !out || !cells.inside(resolution_)) return false;
    const std::size_t row = std::size_t(cells.i1 - cells.i0) * kSplatTexelBytes;
    for (int j = cells.j0; j < cells.j1; ++j) {
        uint8_t* dst = out + std::size_t(j - cells.j0) * row;
        if (splat_.empty()) {
            for (std::size_t k = 0; k < row; k += kSplatTexelBytes)
                std::copy(kDefaultSplatTexel.begin(), kDefaultSplatTexel.end(), dst + k);
            continue;
        }
        const std::size_t at = splatOffset(cells.i0, j);
        std::copy_n(splat_.begin() + std::ptrdiff_t(at), row, dst);
    }
    return true;
}

bool HeightField::pasteSplat(const GridRect& cells, const uint8_t* rgba, std::size_t count) {
    if (!valid() || !rgba || !cells.inside(resolution_)) return false;
    const std::size_t row = std::size_t(cells.i1 - cells.i0) * kSplatTexelBytes;
    if (count != row * std::size_t(cells.j1 - cells.j0)) return false;
    if (splat_.empty()) {
        bool allDefault = true;
        for (std::size_t k = 0; k < count && allDefault; ++k)
            allDefault = rgba[k] == kDefaultSplatTexel[k % kSplatTexelBytes];
        if (allDefault) return true;  // 塗っていない = 配列を持たないまま
        allocateSplat();
    }
    for (int j = cells.j0; j < cells.j1; ++j) {
        const std::size_t at = splatOffset(cells.i0, j);
        std::copy_n(rgba + std::size_t(j - cells.j0) * row, row,
                    splat_.begin() + std::ptrdiff_t(at));
    }
    return true;
}

bool HeightField::isSolidCell(int i, int j) const {
    return i >= 0 && j >= 0 && i < resolution_ && j < resolution_ && !holeAt(i, j);
}

bool HeightField::solidAt(double x, double z) const {
    // contains は NaN / 無限大・空の格子で false（locate に有限でない値を渡さない）。
    if (!contains(x, z)) return false;
    int i = 0, j = 0;
    double u = 0.0, w = 0.0;
    locate(x, z, i, j, u, w);
    return isSolidCell(i, j);
}

Vec3 HeightField::vertexWorld(int i, int j) const {
    return {double(minX_) + double(i) * double(cell_),
            double(baseY_) + double(sample(i, j)),
            double(minZ_) + double(j) * double(cell_)};
}

void HeightField::cellLocal(double x, double z, int i, int j, double& u,
                            double& w) const {
    const double fx = std::clamp((x - minX_) / cell_, 0.0, double(resolution_));
    const double fz = std::clamp((z - minZ_) / cell_, 0.0, double(resolution_));
    u = fx - i;
    w = fz - j;
}

void HeightField::locate(double x, double z, int& i, int& j, double& u,
                         double& w) const {
    // 範囲外は端へ丸める（縁の高さが外へ続いているとみなす）。
    const double fx = std::clamp((x - minX_) / cell_, 0.0, double(resolution_));
    const double fz = std::clamp((z - minZ_) / cell_, 0.0, double(resolution_));
    i = std::min(int(std::floor(fx)), resolution_ - 1);
    j = std::min(int(std::floor(fz)), resolution_ - 1);
    cellLocal(x, z, i, j, u, w);
}

double HeightField::heightAt(double x, double z) const {
    if (!valid()) return 0.0;
    // 有限でない座標は baseY（floor(NaN) を int にするのは未定義動作）。
    if (!std::isfinite(x) || !std::isfinite(z)) return double(baseY_);
    int i = 0, j = 0;
    double u = 0.0, w = 0.0;
    locate(x, z, i, j, u, w);
    const double h00 = sample(i, j), h10 = sample(i + 1, j);
    const double h01 = sample(i, j + 1), h11 = sample(i + 1, j + 1);
    // 対角線 v00-v11 で割った 2 三角形の平面（描画・衝突と同じ）。
    const double h = (w >= u) ? h00 + u * (h11 - h01) + w * (h01 - h00)   // 三角形 A
                              : h00 + u * (h10 - h00) + w * (h11 - h10);  // 三角形 B
    return double(baseY_) + h;
}

Vec3 HeightField::normalAt(double x, double z) const {
    if (!valid() || !std::isfinite(x) || !std::isfinite(z)) return {0.0, 1.0, 0.0};
    int i = 0, j = 0;
    double u = 0.0, w = 0.0;
    locate(x, z, i, j, u, w);
    const double h00 = sample(i, j), h10 = sample(i + 1, j);
    const double h01 = sample(i, j + 1), h11 = sample(i + 1, j + 1);
    double dhdu = 0.0, dhdw = 0.0;
    if (w >= u) {  // 三角形 A
        dhdu = h11 - h01;
        dhdw = h01 - h00;
    } else {       // 三角形 B
        dhdu = h10 - h00;
        dhdw = h11 - h10;
    }
    return slopeNormal(dhdu / cell_, dhdw / cell_);
}

Vec3 HeightField::vertexNormal(int i, int j) const {
    if (!valid()) return {0.0, 1.0, 0.0};
    i = std::clamp(i, 0, resolution_);
    j = std::clamp(j, 0, resolution_);
    const int il = std::max(i - 1, 0), ir = std::min(i + 1, resolution_);
    const int jd = std::max(j - 1, 0), ju = std::min(j + 1, resolution_);
    const double dhdx =
        (double(sample(ir, j)) - double(sample(il, j))) / (double(ir - il) * cell_);
    const double dhdz =
        (double(sample(i, ju)) - double(sample(i, jd))) / (double(ju - jd) * cell_);
    return slopeNormal(dhdx, dhdz);
}

int HeightField::chunksPerSide(int quadsPerChunk) const {
    if (!valid() || quadsPerChunk < 1) return 0;
    return (resolution_ + quadsPerChunk - 1) / quadsPerChunk;
}

bool HeightField::buildChunkMesh(int cx, int cz, int quadsPerChunk,
                                 ChunkMesh& out) const {
    if (quadsPerChunk > kMaxChunkQuads) return false;
    const int chunks = chunksPerSide(quadsPerChunk);
    if (chunks < 1 || cx < 0 || cz < 0 || cx >= chunks || cz >= chunks) return false;

    const int q = quadsPerChunk;
    const std::size_t side = std::size_t(q) + 1;
    const std::size_t n = side * side;
    out.positions.assign(n * 3, 0.0f);
    out.normals.assign(n * 3, 0.0f);
    out.uvs.assign(n * 2, 0.0f);
    out.indices.assign(std::size_t(q) * std::size_t(q) * 6, 0u);

    // 頂点（行優先: r が +Z、c が +X）。格子の外にはみ出す分は端へ丸める
    // （同じ位置の頂点が重なるだけで、そのクアッドは下で縮退させる）。
    for (int r = 0; r <= q; ++r) {
        const int gj = std::min(cz * q + r, resolution_);
        for (int c = 0; c <= q; ++c) {
            const int gi = std::min(cx * q + c, resolution_);
            const std::size_t v = std::size_t(r) * side + std::size_t(c);
            const Vec3 p = vertexWorld(gi, gj);
            const Vec3 nn = vertexNormal(gi, gj);
            out.positions[v * 3 + 0] = float(p.x());
            out.positions[v * 3 + 1] = float(p.y());
            out.positions[v * 3 + 2] = float(p.z());
            out.normals[v * 3 + 0] = float(nn.x());
            out.normals[v * 3 + 1] = float(nn.y());
            out.normals[v * 3 + 2] = float(nn.z());
            // UV はメートル単位（ワールドの x, z）。タイルは材質が決める。
            out.uvs[v * 2 + 0] = float(p.x());
            out.uvs[v * 2 + 1] = float(p.z());
        }
    }

    // インデックス。クアッドごとに 6 個の席を固定で持ち、中身の無いクアッド
    // （格子の外・穴）は同じ頂点 3 回 × 2 = 面積 0 にする。
    std::size_t k = 0;
    for (int r = 0; r < q; ++r) {
        for (int c = 0; c < q; ++c) {
            const uint32_t v00 = uint32_t(std::size_t(r) * side + std::size_t(c));
            const uint32_t v10 = v00 + 1u;
            const uint32_t v01 = v00 + uint32_t(side);
            const uint32_t v11 = v01 + 1u;
            if (isSolidCell(cx * q + c, cz * q + r)) {
                // 三角形 A = (v00, v01, v11)、B = (v00, v11, v10)。上から見て反時計回り。
                out.indices[k++] = v00;
                out.indices[k++] = v01;
                out.indices[k++] = v11;
                out.indices[k++] = v00;
                out.indices[k++] = v11;
                out.indices[k++] = v10;
            } else {
                for (int m = 0; m < 6; ++m) out.indices[k++] = v00;
            }
        }
    }
    return true;
}

bool HeightField::hitCell(int i, int j, const Vec3& o, const Vec3& d, double tMin,
                          double tMax, double& tOut, Vec3& normalOut) const {
    if (!isSolidCell(i, j)) return false;
    const Vec3 v00 = vertexWorld(i, j);
    const Vec3 v10 = vertexWorld(i + 1, j);
    const Vec3 v01 = vertexWorld(i, j + 1);
    const Vec3 v11 = vertexWorld(i + 1, j + 1);
    // 戻り値を変数に受けてから出力を使う（引数の評価順に頼らない）。
    double tA = 0.0, tB = 0.0;
    Vec3 nA = Vec3::Zero(), nB = Vec3::Zero();
    const bool hitA = hitTriangle(v00, v01, v11, o, d, tMin, tMax, tA, nA);
    const bool hitB = hitTriangle(v00, v11, v10, o, d, tMin, tMax, tB, nB);
    if (!hitA && !hitB) return false;
    bool useA = hitA;
    if (hitA && hitB) {
        const double t = std::min(tA, tB);
        if (std::fabs(tA - tB) > 1e-9 * (1.0 + std::fabs(t))) {
            // 別々の点で当たった（尾根をかすめる斜めのレイ）= 近い方。
            useA = tA < tB;
        } else {
            // 同じ点 = 対角線のごく近く（重心座標の許容差の中）。t の大小は
            // 丸め誤差で決まってしまうので、normalAt と同じ規則・同じ式
            // （cellLocal の (u, w) で、w >= u なら A）で選ぶ。
            double u = 0.0, w = 0.0;
            cellLocal(o.x() + d.x() * t, o.z() + d.z() * t, i, j, u, w);
            useA = (w >= u);
        }
    }
    tOut = useA ? tA : tB;
    normalOut = useA ? nA : nB;
    return true;
}

bool HeightField::raycast(const Vec3& origin, const Vec3& dir, double maxDist,
                          double& tOut, Vec3& normalOut) const {
    if (!valid() || !(maxDist > 0.0)) return false;  // NaN もここで弾く
    if (!std::isfinite(origin.x()) || !std::isfinite(origin.y()) ||
        !std::isfinite(origin.z()) || !std::isfinite(dir.x()) || !std::isfinite(dir.y()) ||
        !std::isfinite(dir.z())) {
        return false;  // NaN / Inf は min / max を素通りして t = NaN の当たりになる
    }
    const double len = dir.norm();
    if (!(len > 1e-12) || !std::isfinite(len)) return false;
    const Vec3 d = dir / len;
    const Vec3& o = origin;
    constexpr double kInf = std::numeric_limits<double>::infinity();

    // 格子の水平な範囲（X-Z の長方形）で t を切る（スラブ法）。
    double tEnter = 0.0, tExit = maxDist;
    const double lo[2] = {double(minX_), double(minZ_)};
    const double hi[2] = {double(maxX()), double(maxZ())};
    const double oa[2] = {o.x(), o.z()};
    const double da[2] = {d.x(), d.z()};
    for (int a = 0; a < 2; ++a) {
        if (std::fabs(da[a]) < 1e-15) {
            if (oa[a] < lo[a] || oa[a] > hi[a]) return false;
            continue;
        }
        double t0 = (lo[a] - oa[a]) / da[a];
        double t1 = (hi[a] - oa[a]) / da[a];
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
    }
    if (tEnter > tExit) return false;

    // 入った点のセルから DDA（Amanatides & Woo）で辿る。
    const double cell = cell_;
    const double px = o.x() + d.x() * tEnter;
    const double pz = o.z() + d.z() * tEnter;
    int i = std::clamp(int(std::floor((px - minX_) / cell)), 0, resolution_ - 1);
    int j = std::clamp(int(std::floor((pz - minZ_) / cell)), 0, resolution_ - 1);

    const int stepI = d.x() > 1e-15 ? 1 : (d.x() < -1e-15 ? -1 : 0);
    const int stepJ = d.z() > 1e-15 ? 1 : (d.z() < -1e-15 ? -1 : 0);
    double tMaxX = kInf, tMaxZ = kInf, tDeltaX = kInf, tDeltaZ = kInf;
    if (stepI != 0) {
        const double bx = minX_ + (stepI > 0 ? (i + 1) : i) * cell;
        tMaxX = (bx - o.x()) / d.x();
        tDeltaX = cell / std::fabs(d.x());
    }
    if (stepJ != 0) {
        const double bz = minZ_ + (stepJ > 0 ? (j + 1) : j) * cell;
        tMaxZ = (bz - o.z()) / d.z();
        tDeltaZ = cell / std::fabs(d.z());
    }

    double tCellEnter = tEnter;
    // 辿るセル数の上限（対角に横切っても 2 * resolution + 2 で足りる）。
    const int maxSteps = 2 * resolution_ + 4;
    for (int step = 0; step < maxSteps; ++step) {
        const double tCellExit = std::min({tMaxX, tMaxZ, tExit});
        const double tMin = std::max(0.0, tCellEnter - kCellSlack);
        const double tMax = std::min(maxDist, tCellExit + kCellSlack);
        double t = 0.0;
        Vec3 nrm = Vec3::Zero();
        const bool hit = hitCell(i, j, o, d, tMin, tMax, t, nrm);
        if (hit) {
            tOut = t;
            normalOut = nrm;
            return true;
        }
        if (tCellExit >= tExit) break;
        if (tMaxX < tMaxZ) {
            i += stepI;
            tCellEnter = tMaxX;
            tMaxX += tDeltaX;
        } else {
            j += stepJ;
            tCellEnter = tMaxZ;
            tMaxZ += tDeltaZ;
        }
        if (i < 0 || j < 0 || i >= resolution_ || j >= resolution_) break;
    }
    return false;
}

}  // namespace terrain
}  // namespace wizengine
