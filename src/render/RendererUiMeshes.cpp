// Renderer の UI のテクスチャと Canvas（overlay）のレンダラブル。
//
// Canvas の中身はビューごと（UiCanvasPart）: アンカーの解き方はビューの縦横比で
// 変わるので、ビューごとにメッシュとレンダラブルを持ち、そのビューの UI の
// シーンにだけ入れる。レンダラブルは UiMeshData の run（同じテクスチャ・材質が
// 続く範囲）ごとにプリミティブを 1 つ持つ。
// 重なり順は深さではなく blendOrder（全体で比べる = global）で、ビューごとに
// Canvas の order（setUiCanvasOrder の整数。前後の規則は呼び手 = ui::canvasBehind の順位で、
// Canvas ごとに違う値を渡す）→ run の並び（= 文書順）の順に 0, 1, 2, ... を振る。
// 頂点の形は ui/UiMeshFormat.h の UiVertex（属性の割り当ては ui.mat の先頭）。
#include "render/RendererInternal.h"

#include <algorithm>
#include <cstddef>

#include "render/ImageScale.h"
#include "render/UiTextureId.h"

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

// 頂点バッファの末尾に足す、どのインデックスも指さない予備の頂点の数。GPU によっては
// バッファのいちばん最後の頂点を正しく読めず（最後の字の角が飛んだ）、本物の頂点を
// 末尾に置かないための保険（頂点の大きさを 16 バイトの倍数にした UiVertex と組）。
constexpr std::size_t kUiTailVertices = 1;

// 頂点の並び + 予備の頂点を、Filament へ渡す記述子に写す（作るときの vertexCount と組）。
filament::backend::BufferDescriptor uiVertexDescriptor(const std::vector<ui::UiVertex>& vertices) {
    std::vector<ui::UiVertex> padded;
    padded.reserve(vertices.size() + kUiTailVertices);
    padded.assign(vertices.begin(), vertices.end());
    padded.resize(vertices.size() + kUiTailVertices);  // 予備は既定値（インデックスが指さない = 描かれない）
    return copyToDescriptor(padded.data(), padded.size());
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

// UI のテクスチャを作れる大きさか（端末の上限を超えると Filament が止まる = abort
// なので、作る前に断る）。字の置き場と画像の両方がここを通る。だめなら警告する
// （consequence = 代わりにどう描くか、英語）。
bool uiTextureSizeOk(Engine& engine, int width, int height, const char* consequence) {
    const std::size_t maxSize = Texture::getMaxTextureSize(engine, Texture::Sampler::SAMPLER_2D);
    const bool positive = width > 0 && height > 0;
    const bool fits = std::size_t(width) <= maxSize && std::size_t(height) <= maxSize;
    if (positive && fits) return true;
    LOGW("render", "ui texture %dx%d cannot be created (limit %zu) - %s", width, height, maxSize,
         consequence);
    return false;
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
    if (textureId < 0) return nullptr;
    const std::size_t slot = std::size_t(uiTextureSlot(textureId));
    if (slot >= uiTextures_.size()) return nullptr;
    UiTexture& t = uiTextures_[slot];
    // 返した後に枠が使い直されていれば世代が違う = 古い番号は別のテクスチャを指さない。
    const bool current = t.used && t.generation == uiTextureGeneration(textureId);
    return current ? &t : nullptr;
}

Renderer::UiCanvas* Renderer::uiCanvasAt(std::size_t id) {
    if (id >= uiCanvases_.size() || !uiCanvases_[id].used) return nullptr;
    return &uiCanvases_[id];
}

// ---- テクスチャ ----------------------------------------------------------------
Renderer::UiTexture& Renderer::uiTextureOrWhite(int textureId) {
    if (UiTexture* t = uiTextureAt(textureId)) return *t;
    if (textureId != ui::kUiNoTexture && !uiStaleTextureWarned_) {
        uiStaleTextureWarned_ = true;
        LOGW("render", "ui mesh refers to texture %d which no longer exists - drawn white",
             textureId);
    }
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
        // 輪郭の値（ui/UiSdfParams.h の kSdfOnEdge、0..255 → 0..1）。
        mi->setParameter("onEdge", float(ui::kSdfOnEdge) / 255.0f);
    }
    return mi;
}

int Renderer::addUiTextureR8(int width, int height) {
    if (!uiTextureSizeOk(*engine_, width, height, "text is drawn as filled boxes")) {
        return ui::kUiNoTexture;
    }
    Texture* texture = Texture::Builder()
                           .width(uint32_t(width))
                           .height(uint32_t(height))
                           .levels(1)
                           .format(Texture::InternalFormat::R8)
                           .sampler(Texture::Sampler::SAMPLER_2D)
                           .build(*engine_);
    if (!texture) {
        LOGW("render", "ui texture %dx%d could not be created - text is drawn as filled boxes",
             width, height);
        return ui::kUiNoTexture;
    }
    // 中身を 0（= 輪郭の外）で入れておく（未初期化のテクスチャを引かせない）。
    std::vector<uint8_t> zeros(std::size_t(width) * std::size_t(height), 0);
    texture->setImage(*engine_, 0, moveToPixelDescriptor(std::move(zeros), Texture::Format::R));
    return adoptUiTexture(texture, width, height, false);
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
    if (!uiTextureSizeOk(*engine_, w, h, "the image is drawn as a white panel")) {
        return ui::kUiNoTexture;
    }
    premultiplySrgbRGBA(pixels);  // 縁の透明な画素の色がにじまないように
    Texture* texture =
        createTextureRGBA(*engine_, std::move(pixels), w, h, true, MipFilter::PremultipliedSrgb);
    return adoptUiTexture(texture, w, h, true);
}

int Renderer::adoptUiTexture(Texture* texture, int width, int height, bool mipmapped) {
    const int slot = firstFreeSlot(uiTextures_);
    if (slot >= kUiTextureSlots) {  // 番号に入らない（同時に 65536 枚 = 呼び手の誤り）
        uiTextures_.pop_back();
        engine_->destroy(texture);
        LOGW("render", "too many ui textures - this one is not created");
        return ui::kUiNoTexture;
    }
    UiTexture& t = uiTextures_[std::size_t(slot)];
    const int generation = nextUiTextureGeneration(t.generation);
    t = UiTexture{};
    t.texture = texture;
    t.width = width;
    t.height = height;
    t.mipmapped = mipmapped;
    t.generation = generation;
    t.used = true;
    return uiTextureId(slot, generation);
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
    const int generation = t.generation;  // 枠の世代は残す（次に使うときに進める）
    t = UiTexture{};
    t.generation = generation;
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

void Renderer::destroyUiCanvasPart(UiCanvasPart& part) {
    if (!part.entity.isNull()) {
        // 入っているのはそのビューの UI のシーンだけ（Renderer.h の UiCanvasPart の約束）。
        ViewSlot& slot = views_[part.view];
        bool inUiScene = true;
        destroyRenderable(*engine_, *slot.uiScene, part.entity, part.vb, part.ib, inUiScene);
    }
    part.mesh = ui::UiMeshData{};
    part.instances.clear();
}

void Renderer::buildUiCanvasPart(const UiCanvas& canvas, UiCanvasPart& part) {
    const ui::UiMeshData& m = part.mesh;
    part.vb = VertexBuffer::Builder()
                  .vertexCount(uint32_t(m.vertices.size() + kUiTailVertices))
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
    slot.uiScene->addEntity(part.entity);  // 入れ物があれば uiScene もある（Renderer.h）
}

void Renderer::setUiCanvasMesh(std::size_t id, std::size_t viewIndex, ui::UiMeshData mesh) {
    UiCanvas* canvas = uiCanvasAt(id);
    if (!canvas) return;
    UiCanvasPart* part = nullptr;
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
    part->mesh = std::move(mesh);
    if (sameShape) {
        refreshUiPartInstances(*part);  // 作り直す道は buildUiCanvasPart が解く
    } else {
        buildUiCanvasPart(*canvas, *part);
    }
    part->vb->setBufferAt(*engine_, 0, uiVertexDescriptor(part->mesh.vertices));
    part->ib->setBuffer(*engine_, copyToDescriptor(part->mesh.indices.data(),
                                                   part->mesh.indices.size()));
    if (!sameShape) refreshUiBlendOrders(viewIndex);
}

void Renderer::refreshUiPartInstances(UiCanvasPart& part) {
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
        UiCanvasPart part;
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

void Renderer::applyUiCanvasTransform(const UiCanvas& canvas, UiCanvasPart& part) {
    if (part.entity.isNull()) return;
    // 設計上の画素 → ビューの高さ = 1 の単位（applyUiProjection の座標）。
    const float s = 1.0f / canvas.designHeight;
    setEntityTransform(*engine_, part.entity, mat4f::scaling(float3{s, s, 1.0f}));
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
