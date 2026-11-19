#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "ui/UiMeshFormat.h"

// UI のメッシュを、設計の座標の半平面 a x + b y + c >= 0 の側だけに切る（純粋な計算）。
// 板の上の選択枠の一部がカメラの後ろに回ったとき、見えている側だけを映像へ写すため
// （Scene::plateMeshToOverlay が「カメラの前」を設計の座標の半平面に直して渡す）。
// Chrono / Filament / Scene を知らない（tests/ui で検査する）。
namespace wizengine {
namespace ui {

namespace clipdetail {
// 頂点は float だけの並び（属性を足しても補間の手順を直さなくてよい）。
inline constexpr std::size_t kVertexFloats = sizeof(UiVertex) / sizeof(float);
static_assert(sizeof(UiVertex) == kVertexFloats * sizeof(float),
              "UiVertex must consist of floats only");
static_assert(std::is_trivially_copyable<UiVertex>::value, "UiVertex must be trivially copyable");

// a と b の間を t（0..1）で線形に補間した頂点（全部の欄）。
inline UiVertex lerpVertex(const UiVertex& a, const UiVertex& b, double t) {
    float fa[kVertexFloats], fb[kVertexFloats];
    std::memcpy(fa, &a, sizeof(UiVertex));
    std::memcpy(fb, &b, sizeof(UiVertex));
    for (std::size_t k = 0; k < kVertexFloats; ++k) fa[k] = float(fa[k] + (fb[k] - fa[k]) * t);
    UiVertex out;
    std::memcpy(&out, fa, sizeof(UiVertex));
    return out;
}
}  // namespace clipdetail

// mesh の三角形のうち半平面の外の部分を捨てる（またがる三角形は切り口で分ける。全部の頂点が
// 表側なら何もしない = 頂点もインデックスもそのまま）。三角形の
// 描く順と run の区切り（テクスチャ・材質）は保ち、run の範囲は切った後の並びで振り直す
// （三角形が無くなった run は消す）。頂点は三角形ごとに持ち直す（共有しない）。
inline void clipMeshToHalfPlane(UiMeshData& mesh, double a, double b, double c) {
    using clipdetail::lerpVertex;
    auto side = [a, b, c](const UiVertex& v) { return a * v.x + b * v.y + c; };
    // 全部の頂点が表側（ふつうの場合）なら、作り直さずにそのまま。
    bool allInside = true;
    for (const UiVertex& v : mesh.vertices) allInside = allInside && side(v) >= 0.0;
    if (allInside) return;
    UiMeshData out;
    for (const UiDrawRun& run : mesh.runs) {
        UiDrawRun next = run;
        next.firstIndex = std::uint32_t(out.indices.size());
        // 範囲は並びの中に収める（壊れた run で並びの外を読まない）。
        const std::size_t end =
            std::min<std::size_t>(std::size_t(run.firstIndex) + run.indexCount, mesh.indices.size());
        for (std::size_t i = run.firstIndex; i + 2 < end; i += 3) {
            const UiVertex tri[3] = {mesh.vertices[mesh.indices[i]],
                                     mesh.vertices[mesh.indices[i + 1]],
                                     mesh.vertices[mesh.indices[i + 2]]};
            // 三角形を半平面で切った多角形（最大 4 頂点）。
            UiVertex poly[4];
            std::size_t n = 0;
            for (int k = 0; k < 3; ++k) {
                const UiVertex& p = tri[k];
                const UiVertex& q = tri[(k + 1) % 3];
                const double dp = side(p), dq = side(q);
                if (dp >= 0.0) poly[n++] = p;
                const bool crosses = (dp >= 0.0) != (dq >= 0.0);
                if (crosses) poly[n++] = lerpVertex(p, q, dp / (dp - dq));
            }
            if (n < 3) continue;
            const std::uint32_t base = std::uint32_t(out.vertices.size());
            out.vertices.insert(out.vertices.end(), poly, poly + n);
            for (std::size_t k = 1; k + 1 < n; ++k) {
                out.indices.push_back(base);
                out.indices.push_back(base + std::uint32_t(k));
                out.indices.push_back(base + std::uint32_t(k + 1));
            }
        }
        next.indexCount = std::uint32_t(out.indices.size()) - next.firstIndex;
        if (next.indexCount > 0) out.runs.push_back(next);
    }
    mesh = std::move(out);
}

}  // namespace ui
}  // namespace wizengine
