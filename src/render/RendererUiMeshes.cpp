// Renderer の UI のテクスチャと Canvas（overlay）のレンダラブル。
//
// Canvas の中身はビューごと（UiCanvasView）: アンカーの解き方はビューの縦横比で
// 変わるので、ビューごとにメッシュとレンダラブルを持ち、そのビューの UI の
// シーンにだけ入れる。レンダラブルは UiMeshData の run（同じテクスチャ・材質が
// 続く範囲）ごとにプリミティブを 1 つ持つ。
// 重なり順は深さではなく blendOrder（全体で比べる = global）で、ビューごとに
// Canvas の order → Canvas の番号 → run の並び（= 文書順）の順に 0, 1, 2, ... を振る。
// 頂点の形は ui/UiMeshFormat.h の UiVertex（属性の割り当ては ui.mat の先頭）。
#include "render/RendererInternal.h"

#include <cstddef>

using namespace filament;
using namespace filament::math;
using utils::EntityManager;
using namespace render_detail;

namespace wizengine {
namespace {

// UI のビューは後処理（sRGB への符号化を含む）を切ってあり、主のビューは
// 符号化した値を同じ描画先に書く。UI の色はリニア値（文書の約束）なので、
// sRGB へ直して書けば主のビューと同じ明るさになる（ui.mat の srgbOut）。
// 実機で色が明るすぎる・暗すぎるときはここを false にして見比べる。
constexpr bool kUiSrgbOut = true;

// blendOrder に使える範囲（15 ビット）。
constexpr uint32_t kMaxBlendOrder = 0x7FFF;

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
    }
    return nullptr;
}

template <class T>
int firstFreeSlot(std::vector<T>& slots) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].used) return int(i);
    }
    slots.emplace_back();
    return int(slots.size()) - 1;
}

}  // namespace

void Renderer::loadUiMaterial() {
    uiMaterial_ = loadMaterialPackage(*engine_, "ui.filamat", "ui material");
}

// ---- 番号 → 実体 ---------------------------------------------------------------
Renderer::UiTexture* Renderer::uiTextureAt(int textureId) {
    const bool inRange = textureId >= 0 && std::size_t(textureId) < uiTextures_.size();
    if (!inRange || !uiTextures_[std::size_t(textureId)].used) return nullptr;
    return &uiTextures_[std::size_t(textureId)];
}

Renderer::UiCanvas* Renderer::uiCanvasAt(std::size_t id) {
    if (id >= uiCanvases_.size() || !uiCanvases_[id].used) return nullptr;
    return &uiCanvases_[id];
}

// ---- テクスチャ ----------------------------------------------------------------
Renderer::UiTexture& Renderer::uiTextureOrWhite(int textureId) {
    if (UiTexture* t = uiTextureAt(textureId)) return *t;
    if (!uiWhite_.texture) {
        uiWhite_.texture = createTextureRGBA(*engine_, {255, 255, 255, 255}, 1, 1, false);
        uiWhite_.width = uiWhite_.height = 1;
        uiWhite_.mipmapped = true;  // 1 段だけ = ミップ付きの引き方でも同じ
        uiWhite_.used = true;
    }
    return uiWhite_;
}

MaterialInstance* Renderer::uiInstance(int textureId, int materialId) {
    UiTexture& t = uiTextureOrWhite(textureId);
    const int slot = materialId == ui::kUiMaterialSdf ? 1 : 0;
    MaterialInstance*& mi = t.instances[slot];
    if (!mi) {
        mi = uiMaterial_->createInstance();
        const TextureSampler sampler(t.mipmapped ? TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR
                                                 : TextureSampler::MinFilter::LINEAR,
                                     TextureSampler::MagFilter::LINEAR);
        mi->setParameter("tex", t.texture, sampler);
        mi->setParameter("sdf", slot == 1);
        mi->setParameter("srgbOut", kUiSrgbOut);
        // 輪郭の値（UiMeshFormat.h の kSdfOnEdge、0..255 → 0..1）。
        mi->setParameter("onEdge", float(ui::kSdfOnEdge) / 255.0f);
    }
    return mi;
}

int Renderer::addUiTextureR8(int width, int height) {
    if (width <= 0 || height <= 0) return ui::kUiNoTexture;
    Texture* texture = Texture::Builder()
                           .width(uint32_t(width))
                           .height(uint32_t(height))
                           .levels(1)
                           .format(Texture::InternalFormat::R8)
                           .sampler(Texture::Sampler::SAMPLER_2D)
                           .build(*engine_);
    if (!texture) return ui::kUiNoTexture;
    const int id = firstFreeSlot(uiTextures_);
    UiTexture& t = uiTextures_[std::size_t(id)];
    t = UiTexture{};
    t.texture = texture;
    t.width = width;
    t.height = height;
    t.used = true;
    // 中身を 0（= 輪郭の外）で入れておく（未初期化のテクスチャを引かせない）。
    std::vector<uint8_t> zeros(std::size_t(width) * std::size_t(height), 0);
    texture->setImage(*engine_, 0, moveToPixelDescriptor(std::move(zeros), Texture::Format::R));
    return id;
}

void Renderer::updateUiTexture(int textureId, int x, int y, int w, int h,
                               const uint8_t* pixels, int rowStride) {
    const UiTexture* t = uiTextureAt(textureId);
    if (!t || !pixels) return;
    const bool inside = x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= t->width &&
                        y + h <= t->height && rowStride >= x + w;
    if (!inside) return;
    // 矩形の行だけを詰めて写す（Filament は非同期に読むので、呼び手の配列は渡せない）。
    std::vector<uint8_t> rect(std::size_t(w) * std::size_t(h));
    for (int row = 0; row < h; ++row) {
        std::memcpy(rect.data() + std::size_t(row) * std::size_t(w),
                    pixels + std::size_t(y + row) * std::size_t(rowStride) + std::size_t(x),
                    std::size_t(w));
    }
    // 行 y はデータの y 行目（ui.mat の flipUV : false と同じ向き = v = 行 / 高さ）。
    t->texture->setImage(*engine_, 0, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h),
                         moveToPixelDescriptor(std::move(rect), Texture::Format::R));
}

int Renderer::loadUiImage(const std::string& path) {
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    // 読めなければ decodeTextureRGBA が警告する。代わりの絵は作らない（白で描く）。
    if (!decodeTextureRGBA(path, "ui image", TextureFallback::None, pixels, w, h)) {
        return ui::kUiNoTexture;
    }
    const int id = firstFreeSlot(uiTextures_);
    UiTexture& t = uiTextures_[std::size_t(id)];
    t = UiTexture{};
    t.texture = createTextureRGBA(*engine_, std::move(pixels), w, h, true);
    t.width = w;
    t.height = h;
    t.mipmapped = true;
    t.used = true;
    return id;
}

void Renderer::releaseUiTexture(int textureId) {
    UiTexture* t = uiTextureAt(textureId);
    if (!t) return;
    // 使っている Canvas × ビューは描くのをやめる（次の setUiCanvasMesh で作り直す）。
    for (auto& canvas : uiCanvases_) {
        if (!canvas.used) continue;
        for (auto& part : canvas.parts) {
            bool uses = false;
            for (const auto& r : part.mesh.runs) {
                if (r.textureId == textureId) uses = true;
            }
            if (uses) destroyUiCanvasPart(part);
        }
    }
    destroyUiTexture(*t);
}

void Renderer::destroyUiTexture(UiTexture& t) {
    // 材質インスタンス（テクスチャを参照している）→ テクスチャ の順に壊す。
    for (auto*& mi : t.instances) {
        if (mi) engine_->destroy(mi);
    }
    if (t.texture) engine_->destroy(t.texture);
    t = UiTexture{};
}

// ---- Canvas --------------------------------------------------------------------
std::size_t Renderer::addUiCanvas() {
    const int id = firstFreeSlot(uiCanvases_);
    uiCanvases_[std::size_t(id)] = UiCanvas{};
    uiCanvases_[std::size_t(id)].used = true;
    return std::size_t(id);
}

void Renderer::removeUiCanvas(std::size_t id) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas) return;
    for (auto& part : canvas->parts) destroyUiCanvasPart(part);
    *canvas = UiCanvas{};
}

void Renderer::destroyUiCanvasPart(UiCanvasView& part) {
    if (!part.entity.isNull()) {
        ViewSlot& slot = views_[part.view];
        if (slot.uiScene) slot.uiScene->remove(part.entity);
        // UI のシーンからは上で外した（主のシーンには入っていない）。
        bool inMainScene = false;
        destroyRenderable(*engine_, *scene_, part.entity, part.vb, part.ib, inMainScene);
    }
    part.mesh = ui::UiMeshData{};
    part.instances.clear();
}

void Renderer::buildUiCanvasPart(const UiCanvas& canvas, UiCanvasView& part) {
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
    RenderableManager::Builder builder(m.runs.size());
    builder.boundingBox({{-1.0e6f, -1.0e6f, -1.0f}, {1.0e6f, 1.0e6f, 1.0f}})
        .culling(false)
        .castShadows(false)
        .receiveShadows(false);
    for (std::size_t i = 0; i < m.runs.size(); ++i) {
        const ui::UiDrawRun& r = m.runs[i];
        part.instances.push_back(uiInstance(r.textureId, r.materialId));
        builder.geometry(i, RenderableManager::PrimitiveType::TRIANGLES, part.vb, part.ib,
                         r.firstIndex, r.indexCount)
            .material(i, part.instances.back())
            .globalBlendOrderEnabled(i, true);
    }
    builder.build(*engine_, part.entity);
    applyUiCanvasTransform(canvas, part);
    ViewSlot& slot = views_[part.view];
    if (slot.uiScene) slot.uiScene->addEntity(part.entity);
}

void Renderer::setUiCanvasMesh(std::size_t id, std::size_t viewIndex,
                               const ui::UiMeshData& mesh) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas) return;
    UiCanvasView* part = nullptr;
    for (auto& p : canvas->parts) {
        if (p.view == viewIndex) part = &p;
    }
    if (!part) return;  // このビューには出していない

    const bool hasRenderable = !part->entity.isNull();
    if (hasRenderable && sameMesh(part->mesh, mesh)) {
        refreshUiPartInstances(*part);  // 同じ中身は送らない（材質だけ見直す）
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
    part->mesh = mesh;
    if (sameShape) {
        refreshUiPartInstances(*part);  // 作り直す道は buildUiCanvasPart が解く
    } else {
        buildUiCanvasPart(*canvas, *part);
    }
    part->vb->setBufferAt(*engine_, 0,
                          copyToDescriptor(part->mesh.vertices.data(),
                                           part->mesh.vertices.size()));
    part->ib->setBuffer(*engine_, copyToDescriptor(part->mesh.indices.data(),
                                                   part->mesh.indices.size()));
    if (!sameShape) refreshUiBlendOrders(viewIndex);
}

void Renderer::refreshUiPartInstances(UiCanvasView& part) {
    // テクスチャが後から用意された（白で解決していた番号が有効になった）ら、
    // その run の材質インスタンスを解決し直して差し替える（作り直さない）。
    auto& rm = engine_->getRenderableManager();
    const auto inst = rm.getInstance(part.entity);
    for (std::size_t i = 0; i < part.mesh.runs.size(); ++i) {
        const ui::UiDrawRun& r = part.mesh.runs[i];
        MaterialInstance* mi = uiInstance(r.textureId, r.materialId);
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
        UiCanvasView part;
        part.view = v;
        parts.push_back(std::move(part));
    }
}

void Renderer::setUiCanvasOrder(std::size_t id, int order) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas || canvas->order == order) return;
    canvas->order = order;
    for (const auto& part : canvas->parts) refreshUiBlendOrders(part.view);
}

void Renderer::setUiCanvasDesignHeight(std::size_t id, float designHeight) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas || !(designHeight > 0.0f)) return;
    canvas->designHeight = designHeight;
    for (auto& part : canvas->parts) applyUiCanvasTransform(*canvas, part);
}

void Renderer::applyUiCanvasTransform(const UiCanvas& canvas, UiCanvasView& part) {
    if (part.entity.isNull()) return;
    // 設計上の画素 → ビューの高さ = 1 の単位（applyUiProjection の座標）。
    const float s = 1.0f / canvas.designHeight;
    setEntityTransform(*engine_, part.entity, mat4f::scaling(float3{s, s, 1.0f}));
}

void Renderer::refreshUiBlendOrders(std::size_t viewIndex) {
    // このビューに出している Canvas を order → 番号 の順に並べ、run の並び
    // （文書順）どおりに通し番号を振る（UI のシーンはビューごとに別）。
    struct Entry {
        std::size_t canvas;
        const UiCanvasView* part;
    };
    std::vector<Entry> order;
    for (std::size_t i = 0; i < uiCanvases_.size(); ++i) {
        if (!uiCanvases_[i].used) continue;
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
            rm.setBlendOrderAt(inst, p, uint16_t(std::min(next, kMaxBlendOrder)));
            ++next;
        }
    }
    if (next > kMaxBlendOrder + 1 && !uiBlendWarned_) {
        uiBlendWarned_ = true;
        LOGW("render", "ui view %zu: %u draw runs exceed the blend order range - the "
             "overlap order of the last ones is not kept", viewIndex, next);
    }
}

}  // namespace wizengine
