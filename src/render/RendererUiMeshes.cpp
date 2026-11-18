// Renderer の UI の Canvas（overlay / 板）のレンダラブル。テクスチャは RendererUiTextures.cpp。
//
// Canvas の中身はビューごと（UiCanvasPart）: アンカーの解き方はビューの縦横比で
// 変わるので、ビューごとにメッシュとレンダラブルを持ち、そのビューの UI の
// シーンにだけ入れる。レンダラブルは UiMeshData の run（同じテクスチャ・材質が
// 続く範囲）ごとにプリミティブを 1 つ持つ。
// 重なり順は深さではなく blendOrder（全体で比べる = global）で、ビューごとに
// Canvas の order（setUiCanvasOrder の整数。前後の規則は呼び手 = ui::canvasBehind の順位で、
// Canvas ごとに違う値を渡す）→ run の並び（= 文書順）の順に 0, 1, 2, ... を振る。
// 頂点の形は ui/UiMeshFormat.h の UiVertex（属性の割り当ては ui.mat の先頭）。
//
// 板の Canvas（UiCanvasPlacement::Plate = camera / world）は中身 1 つを主のシーンに
// 置く（板の大きさで配置が決まるので全ビューで同じ中身）。材質は同じ ui.mat の
// 別のインスタンスで、深さで比べる（手前の物に隠れる。書きはしない = 板の中の
// run どうしは重なり順で決まる）・sRGB へ直すのは主のビューの後処理が切れている
// ときだけ（後処理が符号化するため。uiPlateSrgbOut）。
// 重なり順は板の中だけ（ローカルの blendOrder = run の並び）で、板どうし・他の
// 半透明の物とはカメラからの距離で並ぶ。姿勢と可視はビューごと（RendererUiViews.cpp）。
#include "render/RendererInternal.h"

#include <algorithm>
#include <cstddef>

using namespace filament;
using namespace filament::math;
using utils::EntityManager;
using namespace render_detail;

namespace wizengine {
namespace {

// blendOrder に使える範囲（15 ビット）。
constexpr uint32_t kMaxBlendOrder = 0x7FFF;

// 通し番号 index の重なり順（範囲の外は最後の番号に丸める）。
uint16_t clampedBlendOrder(std::size_t index) {
    return uint16_t(std::min<std::size_t>(index, kMaxBlendOrder));
}

// 描く回数 runCount が重なり順の範囲に入らなければ 1 回だけ警告する（overlay の
// ビューと板の共通。what / which は "view" / "canvas" とその番号）。
void warnBlendOrderRange(bool& warned, std::size_t runCount, const char* what,
                         std::size_t which) {
    if (runCount <= std::size_t(kMaxBlendOrder) + 1 || warned) return;
    warned = true;
    LOGW("render", "ui %s %zu: %zu draw runs exceed the blend order range - the "
         "overlap order of the last ones is not kept", what, which, runCount);
}

bool sameRuns(const std::vector<ui::UiDrawRun>& a, const std::vector<ui::UiDrawRun>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const bool same = a[i].textureId == b[i].textureId &&
                          a[i].materialId == b[i].materialId &&
                          a[i].firstIndex == b[i].firstIndex &&
                          a[i].indexCount == b[i].indexCount;
        if (!same) return false;
    }
    return true;
}

bool sameMesh(const ui::UiMeshData& a, const ui::UiMeshData& b) {
    const bool sameSizes =
        a.vertices.size() == b.vertices.size() && a.indices.size() == b.indices.size();
    if (!sameSizes || !sameRuns(a.runs, b.runs)) return false;
    const bool sameVertices =
        a.vertices.empty() || std::memcmp(a.vertices.data(), b.vertices.data(),
                                          a.vertices.size() * sizeof(ui::UiVertex)) == 0;
    const bool sameIndices =
        a.indices.empty() || std::memcmp(a.indices.data(), b.indices.data(),
                                         a.indices.size() * sizeof(uint32_t)) == 0;
    return sameVertices && sameIndices;
}

// 描ける形か。だめなら理由（ログ用）。三角形の並びは共通の検査、run の範囲は UI 固有。
const char* uiMeshProblem(const ui::UiMeshData& m) {
    if (const char* p = triangleIndexProblem(m.indices, m.vertices.size())) return p;
    for (const auto& r : m.runs) {
        const bool inside = std::size_t(r.firstIndex) + r.indexCount <= m.indices.size();
        if (!inside || r.indexCount == 0) return "a draw run is outside the indices";
        const bool wholeTriangles = r.firstIndex % 3 == 0 && r.indexCount % 3 == 0;
        if (!wholeTriangles) return "a draw run starts or ends inside a triangle";
    }
    return nullptr;
}

}  // namespace

Renderer::UiCanvas* Renderer::uiCanvasAt(std::size_t id) {
    if (id >= uiCanvases_.size() || !uiCanvases_[id].used) return nullptr;
    return &uiCanvases_[id];
}

// ---- Canvas --------------------------------------------------------------------
std::size_t Renderer::addUiCanvas(UiCanvasPlacement placement) {
    const int id = firstFreeSlot(uiCanvases_);
    UiCanvas& canvas = uiCanvases_[std::size_t(id)];
    canvas = UiCanvas{};
    canvas.placement = placement;
    canvas.used = true;
    if (placement == UiCanvasPlacement::Plate) {
        // 板の中身は全ビュー共通の 1 つ（主のシーンに入る）。中身は setUiCanvasMesh。
        UiCanvasPart part;
        part.scene = scene_;
        canvas.parts.push_back(std::move(part));
    }
    return std::size_t(id);
}

void Renderer::removeUiCanvas(std::size_t id) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas) return;
    for (auto& part : canvas->parts) destroyUiCanvasPart(part);
    *canvas = UiCanvas{};
}

void Renderer::destroyUiCanvasPart(UiCanvasPart& part) {
    if (!part.entity.isNull()) {
        // 入っているのは part.scene だけ（Renderer.h の UiCanvasPart の約束）。
        bool inScene = true;
        destroyRenderable(*engine_, *part.scene, part.entity, part.vb, part.ib, inScene);
    }
    part.mesh = ui::UiMeshData{};
    part.instances.clear();
}

void Renderer::buildUiCanvasPart(const UiCanvas& canvas, UiCanvasPart& part) {
    const ui::UiMeshData& m = part.mesh;
    part.vb = VertexBuffer::Builder()
                  .vertexCount(uint32_t(m.vertices.size()))
                  .bufferCount(1)
                  .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT2,
                             offsetof(ui::UiVertex, x), sizeof(ui::UiVertex))
                  .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2,
                             offsetof(ui::UiVertex, u0), sizeof(ui::UiVertex))
                  .attribute(VertexAttribute::UV1, 0, VertexBuffer::AttributeType::FLOAT2,
                             offsetof(ui::UiVertex, u1), sizeof(ui::UiVertex))
                  .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::FLOAT4,
                             offsetof(ui::UiVertex, r), sizeof(ui::UiVertex))
                  .attribute(VertexAttribute::CUSTOM0, 0, VertexBuffer::AttributeType::FLOAT,
                             offsetof(ui::UiVertex, outline), sizeof(ui::UiVertex))
                  .attribute(VertexAttribute::CUSTOM1, 0, VertexBuffer::AttributeType::FLOAT4,
                             offsetof(ui::UiVertex, outlineR), sizeof(ui::UiVertex))
                  .build(*engine_);
    part.ib = IndexBuffer::Builder()
                  .indexCount(uint32_t(m.indices.size()))
                  .bufferType(IndexBuffer::IndexType::UINT)
                  .build(*engine_);
    part.entity = EntityManager::get().create();
    part.instances.clear();
    const bool plate = canvas.placement == UiCanvasPlacement::Plate;
    RenderableManager::Builder builder(m.runs.size());
    // overlay は境界の箱を見ない（UI のビューは全部を描く）。板の箱は中身から
    // 当て直す（refreshUiPlateBounds）。
    builder.boundingBox({{-1.0e6f, -1.0e6f, -1.0f}, {1.0e6f, 1.0e6f, 1.0f}})
        .culling(false)
        .castShadows(false)
        .receiveShadows(false);
    for (std::size_t i = 0; i < m.runs.size(); ++i) {
        const ui::UiDrawRun& r = m.runs[i];
        part.instances.push_back(uiInstance(r.textureId, r.materialId, canvas.placement));
        builder.geometry(i, RenderableManager::PrimitiveType::TRIANGLES, part.vb, part.ib,
                         r.firstIndex, r.indexCount)
            .material(i, part.instances.back());
        if (plate) {
            // 板の中だけの重なり順（run の並び = 文書順）。板どうしは距離で並ぶ。
            builder.blendOrder(i, clampedBlendOrder(i));
        } else {
            builder.globalBlendOrderEnabled(i, true);  // 番号は refreshUiBlendOrders
        }
    }
    builder.build(*engine_, part.entity);
    if (plate) {
        // 板は姿勢を当てるまで映さない（applyUiVisibility が描くビューごとに当てる）。
        setUiPlateShown(part.entity, false);
        warnBlendOrderRange(uiBlendWarned_, m.runs.size(), "canvas",
                            std::size_t(&canvas - uiCanvases_.data()));
    }
    applyUiCanvasTransform(canvas, part);
    // overlay の入れ物があれば uiScene もある、板は主のシーン（Renderer.h）。
    part.scene->addEntity(part.entity);
}

void Renderer::setUiCanvasMesh(std::size_t id, std::size_t viewIndex, ui::UiMeshData mesh) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas) return;
    const bool plate = canvas->placement == UiCanvasPlacement::Plate;
    UiCanvasPart* part = nullptr;
    if (plate) {
        part = &canvas->parts.front();  // 板はビューに依らない 1 つ（addUiCanvas）
    } else {
        for (auto& p : canvas->parts) {
            if (p.view == viewIndex) part = &p;
        }
    }
    if (!part) return;  // このビューには出していない

    const bool hasRenderable = !part->entity.isNull();
    if (hasRenderable && sameMesh(part->mesh, mesh)) {
        refreshUiPartInstances(*canvas, *part);  // 同じ中身は送らない（材質だけ見直す）
        return;
    }

    const char* problem = mesh.runs.empty() ? nullptr : uiMeshProblem(mesh);
    if (problem && !uiWarned_) {
        LOGW("render", "ui canvas %zu hidden: %s", id, problem);
        uiWarned_ = true;
    }
    if (mesh.runs.empty() || problem) {
        destroyUiCanvasPart(*part);
        return;
    }
    // 頂点数・インデックス数・run が変わったらレンダラブルごと作り直す（プリミ
    // ティブの数と範囲を実行中に変える API は版によって名前が違うので使わない =
    // RendererOverlays.cpp と同じ流儀）。同じなら中身だけ送る。
    const bool sameShape = hasRenderable &&
                           part->mesh.vertices.size() == mesh.vertices.size() &&
                           part->mesh.indices.size() == mesh.indices.size() &&
                           sameRuns(part->mesh.runs, mesh.runs);
    if (!sameShape) destroyUiCanvasPart(*part);
    part->mesh = std::move(mesh);
    if (sameShape) {
        refreshUiPartInstances(*canvas, *part);  // 作り直す道は buildUiCanvasPart が解く
    } else {
        buildUiCanvasPart(*canvas, *part);
    }
    part->vb->setBufferAt(*engine_, 0,
                          copyToDescriptor(part->mesh.vertices.data(),
                                           part->mesh.vertices.size()));
    part->ib->setBuffer(*engine_, copyToDescriptor(part->mesh.indices.data(),
                                                   part->mesh.indices.size()));
    if (plate) {
        refreshUiPlateBounds(*part);  // 板の重なり順は作るときに決まる（ローカル）
    } else if (!sameShape) {
        refreshUiBlendOrders(viewIndex);
    }
}

void Renderer::refreshUiPlateBounds(UiCanvasPart& part) {
    // 中身の頂点（設計上の画素、z = 0）を囲む箱。変換は描くビューごとに当たる。
    float3 lo{0.0f}, hi{0.0f};
    for (std::size_t i = 0; i < part.mesh.vertices.size(); ++i) {
        const ui::UiVertex& v = part.mesh.vertices[i];
        const float3 p{v.x, v.y, 0.0f};
        lo = i == 0 ? p : min(lo, p);
        hi = i == 0 ? p : max(hi, p);
    }
    auto& rm = engine_->getRenderableManager();
    rm.setAxisAlignedBoundingBox(rm.getInstance(part.entity), boxFromBounds(lo, hi));
}

void Renderer::refreshUiPartInstances(const UiCanvas& canvas, UiCanvasPart& part) {
    // テクスチャが後から用意された（白で解決していた番号が有効になった）ら、
    // その run の材質インスタンスを解決し直して差し替える（作り直さない）。
    auto& rm = engine_->getRenderableManager();
    const auto inst = rm.getInstance(part.entity);
    for (std::size_t i = 0; i < part.mesh.runs.size(); ++i) {
        const ui::UiDrawRun& r = part.mesh.runs[i];
        MaterialInstance* mi = uiInstance(r.textureId, r.materialId, canvas.placement);
        if (mi == part.instances[i]) continue;
        rm.setMaterialInstanceAt(inst, i, mi);
        part.instances[i] = mi;
    }
}

void Renderer::setUiCanvasViews(std::size_t id, const std::vector<std::size_t>& viewIndices) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas) return;
    std::vector<std::size_t> next;
    for (const std::size_t v : viewIndices) {
        const bool exists = v < views_.size();
        const bool duplicate = std::find(next.begin(), next.end(), v) != next.end();
        if (exists && !duplicate) next.push_back(v);
    }
    if (canvas->placement == UiCanvasPlacement::Plate) {
        // 板は中身が 1 つなので、出すビューの一覧だけを持ち直す（残るビューの姿勢は保つ）。
        std::vector<UiPlateView> views;
        for (const std::size_t v : next) {
            UiPlateView entry;
            entry.view = v;
            for (const auto& old : canvas->plateViews) {
                if (old.view == v) entry = old;
            }
            views.push_back(entry);
        }
        canvas->plateViews = std::move(views);
        return;
    }
    // 外れたビューの中身は壊す。
    auto& parts = canvas->parts;
    for (auto it = parts.begin(); it != parts.end();) {
        const bool stays = std::find(next.begin(), next.end(), it->view) != next.end();
        if (stays) {
            ++it;
            continue;
        }
        destroyUiCanvasPart(*it);
        it = parts.erase(it);
    }
    // 増えたビューは入れ物だけ作る（中身は次の setUiCanvasMesh）。
    for (const std::size_t v : next) {
        bool already = false;
        for (const auto& p : parts) {
            if (p.view == v) already = true;
        }
        if (already) continue;
        ensureUiView(views_[v]);  // 最初に Canvas を出すときに UI のビューを作る
        UiCanvasPart part;
        part.view = v;
        part.scene = views_[v].uiScene;
        parts.push_back(std::move(part));
    }
}

void Renderer::setUiCanvasTransform(std::size_t id, std::size_t viewIndex,
                                    const mat4f& transform) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas) return;
    for (auto& pv : canvas->plateViews) {  // overlay の一覧は空 = 何もしない
        if (pv.view != viewIndex) continue;
        pv.transform = transform;
        pv.posed = true;
    }
}

void Renderer::setUiCanvasOrder(std::size_t id, int order) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas || canvas->order == order) return;
    canvas->order = order;
    if (canvas->placement == UiCanvasPlacement::Plate) return;  // 板は距離で並ぶ
    for (const auto& part : canvas->parts) refreshUiBlendOrders(part.view);
}

void Renderer::setUiCanvasDesignHeight(std::size_t id, float designHeight) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas || !(designHeight > 0.0f)) return;
    canvas->designHeight = designHeight;
    for (auto& part : canvas->parts) applyUiCanvasTransform(*canvas, part);
}

void Renderer::applyUiCanvasTransform(const UiCanvas& canvas, UiCanvasPart& part) {
    // 板の変換は描くビューごとに当てる（applyUiVisibility）。
    const bool plate = canvas.placement == UiCanvasPlacement::Plate;
    if (part.entity.isNull() || plate) return;
    // 設計上の画素 → ビューの高さ = 1 の単位（applyUiProjection の座標）。
    setEntityTransform(*engine_, part.entity, uiDesignScale(canvas.designHeight));
}

void Renderer::refreshUiBlendOrders(std::size_t viewIndex) {
    // このビューに出している Canvas を order の順に並べ、run の並び（文書順）どおりに
    // 通し番号を振る（UI のシーンはビューごとに別）。Renderer の番号は並びに使わない（呼び手が
    // Canvas ごとに違う order を渡す。同じ値が来たときだけ番号の順のまま）。
    struct Entry {
        std::size_t canvas;
        const UiCanvasPart* part;
    };
    std::vector<Entry> order;
    for (std::size_t i = 0; i < uiCanvases_.size(); ++i) {
        const bool overlay = uiCanvases_[i].placement == UiCanvasPlacement::Overlay;
        if (!uiCanvases_[i].used || !overlay) continue;
        for (const auto& part : uiCanvases_[i].parts) {
            if (part.view == viewIndex && !part.entity.isNull()) order.push_back({i, &part});
        }
    }
    std::stable_sort(order.begin(), order.end(), [this](const Entry& a, const Entry& b) {
        return uiCanvases_[a.canvas].order < uiCanvases_[b.canvas].order;
    });
    auto& rm = engine_->getRenderableManager();
    uint32_t next = 0;
    for (const Entry& e : order) {
        const auto inst = rm.getInstance(e.part->entity);
        for (std::size_t p = 0; p < e.part->mesh.runs.size(); ++p) {
            rm.setBlendOrderAt(inst, p, clampedBlendOrder(next));
            ++next;
        }
    }
    warnBlendOrderRange(uiBlendWarned_, next, "view", viewIndex);
}

}  // namespace wizengine
