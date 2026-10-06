#include "render/RendererInternal.h"

#include "render/GltfLoader.h"

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using namespace render_detail;

namespace {

// 24 vertices: 4 per face, so each face can carry its own normal (needed for
// lit shading). Faces are wound CCW when viewed from outside.
const float3 kCubePos[24] = {
    // front (+Z)
    {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
    // back (-Z)
    {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
    // left (-X)
    {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f},
    // right (+X)
    {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f},
    // top (+Y)
    {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
    // bottom (-Y)
    {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f},
};

// Per-face normals, in the same order as kCubePos (front, back, left, right,
// top, bottom). Used to build the tangent frames a lit material needs.
const float3 kCubeNrm[24] = {
    {0, 0, 1},  {0, 0, 1},  {0, 0, 1},  {0, 0, 1},   // front (+Z)
    {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},  // back (-Z)
    {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0},  // left (-X)
    {1, 0, 0},  {1, 0, 0},  {1, 0, 0},  {1, 0, 0},   // right (+X)
    {0, 1, 0},  {0, 1, 0},  {0, 1, 0},  {0, 1, 0},   // top (+Y)
    {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0},  // bottom (-Y)
};

// Two triangles per face: (0,1,2) (2,3,0), offset by 4*face.
const uint16_t kCubeIdx[36] = {
    0, 1, 2, 2, 3, 0,        4, 5, 6, 6, 7, 4,
    8, 9, 10, 10, 11, 8,     12, 13, 14, 14, 15, 12,
    16, 17, 18, 18, 19, 16,  20, 21, 22, 22, 23, 20,
};

}  // namespace

namespace wizengine {

Renderer::Renderer(int width, int height, const std::string& materialPath)
    : width_(width), height_(height) {
    // The builder picks a default backend. For headless servers where the
    // default GL context fails, force Vulkan with the env var
    // FILAMENT_BACKEND=vulkan or .backend(Engine::Backend::VULKAN) here.
    //
    // driverHandleArenaSizeMB: the handle arena holds every backend object
    // handle (buffers, textures, render targets, ...). The platform default
    // is not sized for this scene - 512 renderables, three views with their
    // swap chains, and the IBL prefilter's per-mip render targets - so
    // Filament warns ("HandleAllocator arena is full") and falls back to
    // slower heap allocations. Empirically 32 was still not enough on the
    // Vulkan backend (its handle structs are large); 128 MiB is where the
    // warning stops for this scene. The cost is just that much reserved
    // memory - trim it if the scene ever shrinks.
    Engine::Config engineConfig = {};
    engineConfig.driverHandleArenaSizeMB = 128;
    engine_ = Engine::Builder().config(&engineConfig).build();

    renderer_ = engine_->createRenderer();
    scene_ = engine_->createScene();
    addView();  // view 0 - always present

    // Solid clear colour (linear RGBA).
    filament::Renderer::ClearOptions clear;
    clear.clear = true;
    clear.clearColor = {0.10f, 0.12f, 0.15f, 1.0f};
    renderer_->setClearOptions(clear);

    // Box material (lit), shared by all box renderables. Scene sets the colour.
    wizengine::requireFile(materialPath, "object material");
    const auto pkg = readFile(assetPath(materialPath));
    if (pkg.empty()) throw AssetError(assetPath(materialPath), "is empty");
    material_ = Material::Builder().package(pkg.data(), pkg.size()).build(*engine_);
    if (!material_) {
        throw AssetError(assetPath(materialPath), "is not a valid .filamat");
    }
    matInstance_ = material_->createInstance();
    matInstance_->setParameter("baseColor", RgbType::LINEAR,
                               float3{0.80f, 0.36f, 0.18f});
    // 材質のパラメータは .mat に既定値が書けないので、インスタンスを作った
    // ところで必ず入れる（入れ忘れると 0 = 鏡のような金属になる）。
    applyMaterialParams(matInstance_, ShapeMaterial{});

    // Shared cube mesh for boxes: positions + tangent frames (for lit shading).
    auto* cubeOrient = filament::geometry::SurfaceOrientation::Builder()
                           .vertexCount(24)
                           .normals(kCubeNrm)
                           .build();
    cubeOrient->getQuats(cubeTangents_, 24);
    delete cubeOrient;

    vb_ = VertexBuffer::Builder()
              .vertexCount(24)
              .bufferCount(2)
              .attribute(VertexAttribute::POSITION, 0,
                         VertexBuffer::AttributeType::FLOAT3)
              .attribute(VertexAttribute::TANGENTS, 1,
                         VertexBuffer::AttributeType::FLOAT4)
              .build(*engine_);
    vb_->setBufferAt(*engine_, 0,
                     VertexBuffer::BufferDescriptor(kCubePos, sizeof(kCubePos)));
    vb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(cubeTangents_, sizeof(cubeTangents_)));

    ib_ = IndexBuffer::Builder()
              .indexCount(36)
              .bufferType(IndexBuffer::IndexType::USHORT)
              .build(*engine_);
    ib_->setBuffer(*engine_,
                   IndexBuffer::BufferDescriptor(kCubeIdx, sizeof(kCubeIdx)));

    // Lit material for the ground (loaded now; the ground geometry is created
    // later in addGround). Boxes and ground are added by the Scene.
    wizengine::requireFile("ground_lit.filamat", "ground material");
    const auto gpkg = readFile(assetPath("ground_lit.filamat"));
    if (gpkg.empty()) {
        throw AssetError(assetPath("ground_lit.filamat"), "is empty");
    }
    groundMaterial_ =
        Material::Builder().package(gpkg.data(), gpkg.size()).build(*engine_);
    if (!groundMaterial_) {
        throw AssetError(assetPath("ground_lit.filamat"),
                         "is not a valid .filamat");
    }
    groundMatInstance_ = groundMaterial_->createInstance();

    // Direct lights are no longer created here: the scene owns them as
    // editable light descriptors (Scene::LightItem, saved in the scene
    // document) and adds them through addLight() via syncLights. Only the
    // ambient below is built in, so a scene with no lights configured still
    // isn't pitch black.

    // Uniform ambient (constant SH, no environment map) so shadowed areas of
    // the lit ground are a soft gray instead of pure black. Replaced by
    // loadEnvironment() when the scene names an HDR; clearEnvironment() puts
    // it back.
    installFlatAmbient();

    // 描画設定（既定値 = これまでの絵）。ここで一度通しておくと、以後は
    // シーン文書の <visual> が来たときに同じ道を通るだけになる。
    setRenderSettings(settings_);
}

// ---- 描画設定（フォトリアル）------------------------------------------------
// シーン文書の <visual> が変わるたびに Scene が呼ぶ。Filament 側は
// 「ビューの後処理」「カメラの露出」「色作り（LUT）」「ライトの影」の
// 4 か所に分かれているので、ここで配り直す。
void Renderer::setRenderSettings(const RenderSettings& settings) {
    const RenderSettings before = settings_;
    settings_ = settings;
    // 色作りは LUT を焼く（32^3 のテクスチャ）ので、関係する値が変わった
    // ときだけ作り直す。毎フレーム呼ばれても安いままにしておきたい。
    const bool gradingChanged =
        !colorGrading_ || before.tonemap != settings_.tonemap ||
        before.contrast != settings_.contrast ||
        before.saturation != settings_.saturation ||
        before.temperature != settings_.temperature ||
        before.tint != settings_.tint;
    if (gradingChanged) rebuildColorGrading();
    for (auto& slot : views_) applyViewSettings(slot);
    applyShadowSettings();
}

// トーンマップ（HDR → 表示）と色調整を 1 個の ColorGrading に焼く。
// フォトリアルの見え方をいちばん左右するのがここ: 物理的に正しい HDR を
// どうフィルムに落とすか、という段。
void Renderer::rebuildColorGrading() {
    ColorGrading::Builder builder;
    // ToneMapper は build() が同期なので、この関数を抜けるときに壊れてよい
    // （Filament のヘッダにもそう書いてある）。
    filament::LinearToneMapper linear;
    filament::ACESToneMapper aces;
    filament::ACESLegacyToneMapper acesLegacy;
    filament::FilmicToneMapper filmic;
    filament::PBRNeutralToneMapper pbrNeutral;
    filament::AgxToneMapper agx(filament::AgxToneMapper::AgxLook::NONE);
    const ToneMapper* tm = &acesLegacy;
    switch (settings_.tonemap) {
        case RenderSettings::Tonemap::Aces: tm = &aces; break;
        case RenderSettings::Tonemap::Filmic: tm = &filmic; break;
        case RenderSettings::Tonemap::Agx: tm = &agx; break;
        case RenderSettings::Tonemap::PbrNeutral: tm = &pbrNeutral; break;
        case RenderSettings::Tonemap::Linear: tm = &linear; break;
        case RenderSettings::Tonemap::AcesLegacy: break;
    }
    ColorGrading* next = builder.toneMapper(tm)
                             .quality(ColorGrading::QualityLevel::HIGH)
                             .contrast(settings_.contrast)
                             .saturation(settings_.saturation)
                             .whiteBalance(settings_.temperature, settings_.tint)
                             .build(*engine_);
    if (!next) {
        // 焼けなかったときは前の設定のまま（既定のトーンマップに戻すより、
        // 直前まで見えていた絵を保つ方が驚きが少ない）。
        LOGW("render", "color grading could not be built - keeping the previous one");
        return;
    }
    // 先に全ビューへ差し替えてから古い方を壊す（使用中の LUT を消さない）。
    for (auto& slot : views_) slot.view->setColorGrading(next);
    if (colorGrading_) engine_->destroy(colorGrading_);
    colorGrading_ = next;
}

// ビュー 1 つぶん。addView（カメラの遅延生成）からも呼ぶので、あとから
// 開いたページでも設定が揃う。
void Renderer::applyViewSettings(ViewSlot& slot) {
    View* v = slot.view;
    const RenderSettings& s = settings_;

    v->setPostProcessingEnabled(s.postProcess);
    v->setColorGrading(colorGrading_);
    v->setDithering(Dithering::TEMPORAL);  // 暗部のバンディング対策
    v->setAntiAliasing(s.fxaa ? AntiAliasing::FXAA : AntiAliasing::NONE);

    MultiSampleAntiAliasingOptions msaa;
    msaa.enabled = s.msaa > 1;
    msaa.sampleCount = s.msaa;
    v->setMultiSampleAntiAliasingOptions(msaa);

    TemporalAntiAliasingOptions taa;
    taa.enabled = s.taa;
    v->setTemporalAntiAliasingOptions(taa);

    // TAA と被写界深度は画面の外の情報を使うので、縁に余白（ガードバンド）が
    // 要る。無いと画面端に尾を引く。
    GuardBandOptions guard;
    guard.enabled = s.taa || s.dofFocus > 0.0f;
    v->setGuardBandOptions(guard);

    AmbientOcclusionOptions ao;
    ao.enabled = s.ssao;
    ao.intensity = s.ssaoIntensity;
    ao.quality = QualityLevel::HIGH;
    ao.upsampling = QualityLevel::HIGH;
    ao.lowPassFilter = QualityLevel::MEDIUM;
    v->setAmbientOcclusionOptions(ao);

    BloomOptions bloom;
    bloom.enabled = s.bloom > 0.0f;
    bloom.strength = s.bloom;
    bloom.quality = QualityLevel::HIGH;
    v->setBloomOptions(bloom);

    ScreenSpaceReflectionsOptions ssr;
    ssr.enabled = s.ssr;
    v->setScreenSpaceReflectionsOptions(ssr);

    DepthOfFieldOptions dof;
    dof.enabled = s.dofFocus > 0.0f;
    dof.cocScale = s.dofBlur;
    v->setDepthOfFieldOptions(dof);

    VignetteOptions vignette;
    vignette.enabled = s.vignette > 0.0f;
    // 強さ 1 つのつまみを Filament の 3 つの値に配る。midPoint が小さいほど
    // 中心近くまで暗くなるので、強さをそこへ写す。
    vignette.midPoint = 1.0f - 0.5f * s.vignette;
    vignette.roundness = 0.5f;
    vignette.feather = 0.5f;
    v->setVignetteOptions(vignette);

    switch (s.shadow) {
        case RenderSettings::Shadow::Dpcf:
            v->setShadowType(ShadowType::DPCF);
            break;
        case RenderSettings::Shadow::Pcss:
            v->setShadowType(ShadowType::PCSS);
            break;
        case RenderSettings::Shadow::Vsm: {
            v->setShadowType(ShadowType::VSM);
            VsmShadowOptions vsm;
            vsm.anisotropy = 1;
            vsm.mipmapping = true;
            v->setVsmShadowOptions(vsm);
            break;
        }
        case RenderSettings::Shadow::Pcf:
            v->setShadowType(ShadowType::PCF);
            break;
    }
    SoftShadowOptions soft;  // DPCF / PCSS のぼけ方（既定のまま）
    v->setSoftShadowOptions(soft);

    // 露出は「写真と同じ 3 つの値」。ライトが lux / lumen の実単位なので、
    // ここを変えると現実のカメラと同じように絵の明るさが変わる。
    slot.camera->setExposure(s.aperture, 1.0f / s.shutter, s.sensitivity);
    if (s.dofFocus > 0.0f) slot.camera->setFocusDistance(s.dofFocus);
}

// 影の設定はライトごと（Filament の LightManager が持つ）。既にあるライトへ
// 流し込むので、シーンを読み直さなくても品質を上げ下げできる。
void Renderer::applyShadowSettings() {
    auto& lm = engine_->getLightManager();
    for (const auto& e : lightEntities_) {
        if (e.isNull()) continue;
        const auto li = lm.getInstance(e);
        if (!li) continue;
        lm.setShadowOptions(li, shadowOptionsFrom(settings_, lm.isDirectional(li)));
    }
}

// 材質をマテリアルインスタンスへ。自己発光は baseColor を使い回すのではなく
// 白で足す（色は baseColor 側で付く）。w = 1 は「露出の影響を受ける」の意味で、
// これを立てておかないと露出を変えたときに発光だけ取り残される。
void Renderer::applyMaterialParams(filament::MaterialInstance* mi,
                                   const ShapeMaterial& m) const {
    if (!mi) return;
    mi->setParameter("roughness", m.roughness);
    mi->setParameter("metallic", m.metallic);
    mi->setParameter("reflectance", m.reflectance);
    mi->setParameter("clearCoat", m.clearCoat);
    mi->setParameter("clearCoatRoughness", m.clearCoatRoughness);
    mi->setParameter("emissiveScale", m.emissive);
}

std::size_t Renderer::addShape(ShapeMesh mesh) {
    if (mesh == ShapeMesh::Sphere) ensureSphereMesh();
    if (mesh == ShapeMesh::Cylinder) ensureCylinderMesh();

    // 空きスロットがあれば再利用。エディタで置いては消してを繰り返しても
    // エンティティ番号が無限に伸びない。
    std::size_t id;
    if (!freeShapes_.empty()) {
        id = freeShapes_.back();
        freeShapes_.pop_back();
    } else {
        shapes_.push_back(ShapeSlot{});
        id = shapes_.size() - 1;
    }

    ShapeSlot& slot = shapes_[id];
    slot.mi = nullptr;      // 色は共有インスタンス（setShapeColor で個別化）
    slot.highlight = -1;
    slot.used = true;
    // 前にこの席を使っていた物の色と材質を引き継がない。
    slot.color = float3{0.80f, 0.36f, 0.18f};
    slot.material = ShapeMaterial{};

    VertexBuffer* meshVb = vb_;
    IndexBuffer* meshIb = ib_;
    uint32_t meshIndices = 36;
    if (mesh == ShapeMesh::Sphere) {
        meshVb = sphereVb_;
        meshIb = sphereIb_;
        meshIndices = sphereIndexCount_;
    } else if (mesh == ShapeMesh::Cylinder) {
        meshVb = cylinderVb_;
        meshIb = cylinderIb_;
        meshIndices = cylinderIndexCount_;
    }
    slot.entity = EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {1, 1, 1}})
        .material(0, matInstance_)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, meshVb, meshIb,
                  0, meshIndices)
        .culling(true)  // frustum-cull off-screen boxes (matters at high counts)
        .castShadows(true)
        .receiveShadows(true)
        .build(*engine_, slot.entity);
    scene_->addEntity(slot.entity);
    return id;
}

void Renderer::removeShape(std::size_t id) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    // 番号は空きリストへ戻すので、次の addShape が同じスロットを使う。
    ShapeSlot& slot = shapes_[id];
    scene_->remove(slot.entity);
    engine_->destroy(slot.entity);
    utils::EntityManager::get().destroy(slot.entity);
    slot.entity = utils::Entity();
    if (slot.mi) {
        engine_->destroy(slot.mi);
        slot.mi = nullptr;
    }
    // ソフトボディの自前バッファ（Filament の destroy は GPU が使い終わる
    // まで遅延するので、前フレームが参照していても構わない）。
    if (slot.vb) {
        engine_->destroy(slot.vb);
        slot.vb = nullptr;
    }
    if (slot.ib) {
        engine_->destroy(slot.ib);
        slot.ib = nullptr;
    }
    slot.vertexCount = 0;
    slot.highlight = -1;
    slot.used = false;
    freeShapes_.push_back(id);
}

std::size_t Renderer::addSoftShape(std::size_t vertexCount,
                                   const std::vector<uint32_t>& indices) {
    std::size_t id;
    if (!freeShapes_.empty()) {
        id = freeShapes_.back();
        freeShapes_.pop_back();
    } else {
        shapes_.push_back(ShapeSlot{});
        id = shapes_.size() - 1;
    }
    ShapeSlot& slot = shapes_[id];
    // 変形メッシュは両面を描く（法線は頂点から作るので、たまたま裏を向いた
    // 三角形や、まだ閉じていない面が背面カリングで抜けないように）。専用の
    // マテリアルインスタンスをここで作っておけば setShapeColor はそれを使う。
    slot.mi = material_->createInstance();
    slot.mi->setParameter("baseColor", RgbType::LINEAR, float3{0.80f, 0.36f, 0.18f});
    slot.mi->setCullingMode(MaterialInstance::CullingMode::NONE);
    slot.highlight = -1;
    slot.used = true;
    slot.vertexCount = uint32_t(std::max<std::size_t>(vertexCount, 1));
    const std::size_t indexCount = std::max<std::size_t>(indices.size(), 3);

    // 頂点は毎フレーム setSoftShapeVertices が書き換える。最初は原点に
    // 潰しておく（未初期化のバッファを描かせない）。
    slot.vb = VertexBuffer::Builder()
                  .vertexCount(slot.vertexCount)
                  .bufferCount(2)
                  .attribute(VertexAttribute::POSITION, 0,
                             VertexBuffer::AttributeType::FLOAT3)
                  .attribute(VertexAttribute::TANGENTS, 1,
                             VertexBuffer::AttributeType::FLOAT4)
                  .build(*engine_);
    {
        auto* pos = new float3[slot.vertexCount];
        auto* tan = new quatf[slot.vertexCount];
        for (uint32_t i = 0; i < slot.vertexCount; ++i) {
            pos[i] = float3{0.0f};
            tan[i] = quatf{1.0f, 0.0f, 0.0f, 0.0f};  // (w, x, y, z) = 単位
        }
        slot.vb->setBufferAt(
            *engine_, 0,
            VertexBuffer::BufferDescriptor(
                pos, sizeof(float3) * slot.vertexCount,
                [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));
        slot.vb->setBufferAt(
            *engine_, 1,
            VertexBuffer::BufferDescriptor(
                tan, sizeof(quatf) * slot.vertexCount,
                [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));
    }
    // インデックスは固定（表面の位相は変わらない）。頂点数は小さいが
    // 一般性のために 32 ビットにしておく。
    {
        auto* idx = new uint32_t[indexCount];
        for (std::size_t i = 0; i < indexCount; ++i) {
            idx[i] = i < indices.size() ? std::min<uint32_t>(indices[i], slot.vertexCount - 1)
                                        : 0u;
        }
        slot.ib = IndexBuffer::Builder()
                      .indexCount(uint32_t(indexCount))
                      .bufferType(IndexBuffer::IndexType::UINT)
                      .build(*engine_);
        slot.ib->setBuffer(
            *engine_,
            IndexBuffer::BufferDescriptor(
                idx, sizeof(uint32_t) * indexCount,
                [](void* p, size_t, void*) { delete[] static_cast<uint32_t*>(p); }));
    }

    slot.entity = EntityManager::get().create();
    // フラスタムカリングは切る: 頂点は毎フレーム書き換わり、AABB も追いかけて
    // 更新するが、車輪のように速く動く物が境界で一瞬消えるのを避ける
    // （数個の物なので描画コストの差は無い。AABB は影の範囲のために保つ）。
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {1, 1, 1}})  // setSoftShapeVertices が毎回更新
        .material(0, slot.mi)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, slot.vb, slot.ib,
                  0, indexCount)
        .culling(false)
        .castShadows(true)
        .receiveShadows(true)
        .build(*engine_, slot.entity);
    scene_->addEntity(slot.entity);
    return id;
}

void Renderer::setSoftShapeVertices(std::size_t id, const float* positions,
                                    const float* normals, std::size_t vertexCount) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    ShapeSlot& slot = shapes_[id];
    if (!slot.vb || !positions || !normals) return;
    const std::size_t n = slot.vertexCount;
    if (vertexCount != n) return;  // 位相が変わったら作り直す約束（Scene 側）

    // 位置。バッファは Filament が非同期にコピーするのでヒープに置き、
    // 完了コールバックで解放する（線バッチと同じ流儀）。
    auto* pos = new float3[n];
    float3 lo{1e30f}, hi{-1e30f};
    for (std::size_t i = 0; i < n; ++i) {
        pos[i] = float3{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
        lo = min(lo, pos[i]);
        hi = max(hi, pos[i]);
    }
    slot.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            pos, sizeof(float3) * n,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));

    // 法線 → 接空間の四元数（lit マテリアルは TANGENTS で法線を受ける）。
    std::vector<float3> nrm(n);
    for (std::size_t i = 0; i < n; ++i) {
        nrm[i] = float3{normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]};
    }
    auto* tan = new quatf[n];
    auto* orient = filament::geometry::SurfaceOrientation::Builder()
                       .vertexCount(n)
                       .normals(nrm.data())
                       .build();
    orient->getQuats(tan, n);
    delete orient;
    slot.vb->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(
            tan, sizeof(quatf) * n,
            [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));

    // バウンディングボックスは頂点から（影の範囲とカリングがこれを見る）。
    auto& rm = engine_->getRenderableManager();
    const float3 center = (lo + hi) * 0.5f;
    const float3 half = max((hi - lo) * 0.5f, float3{0.01f});
    rm.setAxisAlignedBoundingBox(rm.getInstance(slot.entity),
                                 filament::Box{center, half});
}

filament::MaterialInstance* Renderer::ensureShapeInstance(ShapeSlot& slot) {
    if (!slot.mi) {
        slot.mi = material_->createInstance();
        slot.mi->setParameter("baseColor", RgbType::LINEAR, slot.color);
        applyMaterialParams(slot.mi, slot.material);
    }
    return slot.mi;
}

void Renderer::setShapeColor(std::size_t id, const float3& color) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    ShapeSlot& slot = shapes_[id];
    slot.color = color;
    ensureShapeInstance(slot)->setParameter("baseColor", RgbType::LINEAR, color);
    // ハイライト中なら、掴んでいる色を上書きしない（離したときに戻る）。
    if (slot.highlight >= 0) return;
    auto& rm = engine_->getRenderableManager();
    rm.setMaterialInstanceAt(rm.getInstance(slot.entity), 0, slot.mi);
}

void Renderer::setShapeMaterial(std::size_t id, const ShapeMaterial& material) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    ShapeSlot& slot = shapes_[id];
    slot.material = material;
    applyMaterialParams(ensureShapeInstance(slot), material);
    if (slot.highlight >= 0) return;
    auto& rm = engine_->getRenderableManager();
    rm.setMaterialInstanceAt(rm.getInstance(slot.entity), 0, slot.mi);
}

std::size_t Renderer::loadModel(const std::string& path) {
    // Created on first use: an engine that never loads a model pays nothing.
    if (!gltf_) gltf_ = std::make_unique<GltfLoader>(*engine_, *scene_);
    return gltf_->loadModel(path);  // throws AssetError on failure
}

float Renderer::modelSize(std::size_t modelId) const {
    return gltf_ ? gltf_->modelSize(modelId) : 0.0f;
}

std::size_t Renderer::addModelInstance(std::size_t modelId) {
    if (!gltf_) return kInvalidModel;
    const std::size_t id = gltf_->createInstance(modelId);
    return id == GltfLoader::kInvalid ? kInvalidModel : id;
}

void Renderer::releaseModelInstance(std::size_t instanceId) {
    if (gltf_) gltf_->releaseInstance(instanceId);
}

void Renderer::setModelInstanceTransform(
    std::size_t index, const filament::math::mat4f& transform) {
    if (gltf_) gltf_->setInstanceTransform(index, transform);
}

void Renderer::setModelInstanceTint(std::size_t index,
                                    const filament::math::float3& color,
                                    float amount) {
    if (gltf_) gltf_->setInstanceTint(index, color, amount);
}

void Renderer::configureHighlightColors(
    const std::vector<filament::math::float3>& colors) {
    for (auto* mi : highlightInstances_) engine_->destroy(mi);
    highlightInstances_.clear();
    // One instance per camera: same material, different baseColor. Swapping a
    // box between them is just a parameter change for the renderer.
    for (const auto& c : colors) {
        auto* mi = material_->createInstance();
        mi->setParameter("baseColor", RgbType::LINEAR, c);
        applyMaterialParams(mi, ShapeMaterial{});
        highlightInstances_.push_back(mi);
    }
}

float Renderer::verticalFovDegrees() const {
    return 45.0f;  // must match the setProjection call in the constructor
}

void Renderer::setBoxHighlighted(std::size_t id, int styleIndex) {
    if (id >= shapes_.size() || !shapes_[id].used) return;
    ShapeSlot& slot = shapes_[id];
    if (styleIndex >= int(highlightInstances_.size())) styleIndex = -1;
    if (slot.highlight == styleIndex) return;
    slot.highlight = styleIndex;

    // 解除したときは、そのオブジェクト自身の色（無ければ共有色）に戻す。
    MaterialInstance* mi = (styleIndex < 0)
                               ? (slot.mi ? slot.mi : matInstance_)
                               : highlightInstances_[styleIndex];
    auto& rm = engine_->getRenderableManager();
    rm.setMaterialInstanceAt(rm.getInstance(slot.entity), 0, mi);
}

std::size_t Renderer::addView() {
    ViewSlot slot;
    slot.width = width_;
    slot.height = height_;
    // CONFIG_READABLE lets us read the framebuffer back with readPixels().
    // Every view needs its own swap chain because each one is read back and
    // encoded separately.
    slot.swapChain = engine_->createSwapChain(
        uint32_t(slot.width), uint32_t(slot.height), SwapChain::CONFIG_READABLE);

    slot.cameraEntity = EntityManager::get().create();
    slot.camera = engine_->createCamera(slot.cameraEntity);
    const double aspect = double(slot.width) / double(slot.height);
    slot.camera->setProjection(45.0, aspect, 0.1, 200.0, Camera::Fov::VERTICAL);
    slot.camera->lookAt({7.0, 5.0, 9.0}, {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0});

    slot.view = engine_->createView();
    slot.view->setScene(scene_);   // all views share one scene
    slot.view->setCamera(slot.camera);
    slot.view->setViewport({0, 0, uint32_t(slot.width), uint32_t(slot.height)});
    // エディタ専用レイヤ（ギズモ）は既定で見せない。見せるビューは
    // setViewEditorLayerVisible で明示的に選ぶ。
    slot.view->setVisibleLayers(kLayerEditorOnly, 0);
    // 描画設定（後処理・露出）は全ビュー共通。あとから作ったビュー
    // （カメラの遅延生成）にもここで当てる - ページを開いた順で絵が
    // 変わらないため。
    applyViewSettings(slot);

    for (auto& cap : slot.captures) {
        cap.pixels.resize(std::size_t(slot.width) * std::size_t(slot.height) * 4);
        cap.ready = std::make_shared<std::atomic<bool>>(false);
    }

    views_.push_back(std::move(slot));
    return views_.size() - 1;
}

void Renderer::setViewEditorLayerVisible(std::size_t viewIndex, bool visible) {
    if (viewIndex >= views_.size()) return;
    views_[viewIndex].view->setVisibleLayers(kLayerEditorOnly,
                                             visible ? kLayerEditorOnly : 0);
}

void Renderer::requestViewResize(std::size_t viewIndex, int width, int height) {
    if (viewIndex >= views_.size() || width <= 0 || height <= 0) return;
    ViewSlot& slot = views_[viewIndex];
    if (width == slot.width && height == slot.height) {
        slot.pendingW = slot.pendingH = 0;  // nothing to do (or cancel)
        return;
    }
    slot.pendingW = width;
    slot.pendingH = height;
}

void Renderer::setCamera(const filament::math::double3& eye,
                         const filament::math::double3& target) {
    setCamera(0, eye, target);
}

void Renderer::setCamera(std::size_t viewIndex,
                         const filament::math::double3& eye,
                         const filament::math::double3& target) {
    if (viewIndex >= views_.size()) return;
    views_[viewIndex].camera->lookAt(eye, target, {0.0, 1.0, 0.0});
}

Renderer::~Renderer() {
    if (!engine_) return;
    finishPendingReadbacks();  // no buffer may be freed while the GPU has it
    for (auto& slot : shapes_) {
        if (!slot.used) continue;
        scene_->remove(slot.entity);
        engine_->destroy(slot.entity);
        if (slot.mi) engine_->destroy(slot.mi);
        if (slot.vb) engine_->destroy(slot.vb);  // ソフトボディの自前バッファ
        if (slot.ib) engine_->destroy(slot.ib);
    }
    shapes_.clear();
    scene_->remove(groundEntity_);
    engine_->destroy(groundEntity_);
    for (auto e : lightEntities_) {
        scene_->remove(e);
        engine_->destroy(e);
    }
    gltf_.reset();  // models must go before the engine
    if (skybox_) {
        scene_->setSkybox(nullptr);
        engine_->destroy(skybox_);
    }
    if (colorGrading_) {
        // ビューが参照したまま壊さない（このあと描かないので実害は無いが、
        // 「参照を外してから壊す」を守る方が読んで安心できる）。
        for (auto& slot : views_) slot.view->setColorGrading(nullptr);
        engine_->destroy(colorGrading_);
    }
    engine_->destroy(ibl_);
    if (iblTexture_) engine_->destroy(iblTexture_);
    engine_->destroy(groundTexture_);
    engine_->destroy(matInstance_);
    for (auto& line : grabLines_) destroyLine(line);
    grabLines_.clear();
    for (auto& line : jointLines_) destroyLine(line);
    jointLines_.clear();
    for (auto& batch : lineBatches_) {
        if (batch.inScene) scene_->remove(batch.entity);
        engine_->destroy(batch.entity);
        EntityManager::get().destroy(batch.entity);
        engine_->destroy(batch.vb);
        engine_->destroy(batch.ib);
        engine_->destroy(batch.mi);
    }
    lineBatches_.clear();
    for (auto& set : lineSets_) {
        if (set.lineCount > 0) {
            if (set.inScene) scene_->remove(set.entity);
            engine_->destroy(set.entity);
            EntityManager::get().destroy(set.entity);
            engine_->destroy(set.vb);
            engine_->destroy(set.ib);
        }
        if (set.mi) engine_->destroy(set.mi);
    }
    lineSets_.clear();
    if (lineMaterial_) engine_->destroy(lineMaterial_);
    for (auto* mi : highlightInstances_) engine_->destroy(mi);
    engine_->destroy(material_);
    engine_->destroy(groundMatInstance_);
    engine_->destroy(groundMaterial_);
    engine_->destroy(vb_);
    engine_->destroy(groundVb_);
    engine_->destroy(ib_);
    engine_->destroy(groundIb_);
    if (sphereVb_) engine_->destroy(sphereVb_);
    if (sphereIb_) engine_->destroy(sphereIb_);
    if (cylinderVb_) engine_->destroy(cylinderVb_);
    if (cylinderIb_) engine_->destroy(cylinderIb_);
    for (auto& slot : views_) {
        engine_->destroyCameraComponent(slot.cameraEntity);
        engine_->destroy(slot.cameraEntity);
        engine_->destroy(slot.view);
    }
    engine_->destroy(scene_);
    engine_->destroy(renderer_);
    for (auto& slot : views_) engine_->destroy(slot.swapChain);
    views_.clear();
    Engine::destroy(&engine_);
}

void Renderer::setBoxTransform(std::size_t i, const filament::math::mat4f& transform) {
    if (i >= shapes_.size() || !shapes_[i].used) return;
    auto& tcm = engine_->getTransformManager();
    tcm.setTransform(tcm.getInstance(shapes_[i].entity), transform);
}

void Renderer::renderFrame(
    const std::function<void(const uint8_t*, size_t)>& onFrame) {
    renderFrame(0, onFrame);
}

void Renderer::renderFrame(
    std::size_t viewIndex,
    const std::function<void(const uint8_t*, size_t)>& onFrame) {
    if (viewIndex >= views_.size()) return;
    ViewSlot& slot = views_[viewIndex];

    // Deliver the previous frame first, if the GPU has finished with it. Doing
    // this before rendering means the encoder gets work while the new frame is
    // still being drawn.
    // Oldest first, so frames reach the encoder in the order they were drawn.
    while (!slot.pending.empty()) {
        ViewSlot::Capture& done = slot.captures[slot.pending.front()];
        if (!done.ready->load(std::memory_order_acquire)) break;  // still copying
        onFrame(done.pixels.data(), done.pixels.size());
        done.ready->store(false, std::memory_order_release);
        done.inFlight = false;
        slot.pending.pop_front();
    }

    // A requested resize is applied only when no readback is in flight: the
    // GPU may still be copying into the old-size buffers. Until then no new
    // capture is started, so the queue drains within a frame or two.
    const bool resizePending = slot.pendingW != 0;
    if (resizePending && slot.pending.empty()) {
        slot.width = slot.pendingW;
        slot.height = slot.pendingH;
        slot.pendingW = slot.pendingH = 0;
        engine_->destroy(slot.swapChain);
        slot.swapChain = engine_->createSwapChain(
            uint32_t(slot.width), uint32_t(slot.height),
            SwapChain::CONFIG_READABLE);
        slot.view->setViewport(
            {0, 0, uint32_t(slot.width), uint32_t(slot.height)});
        // Same fov/near/far as addView - only the aspect follows the size.
        slot.camera->setProjection(45.0,
                                   double(slot.width) / double(slot.height),
                                   0.1, 200.0, Camera::Fov::VERTICAL);
        for (auto& cap : slot.captures) {
            cap.pixels.assign(
                std::size_t(slot.width) * std::size_t(slot.height) * 4, 0);
            cap.inFlight = false;
            cap.ready->store(false, std::memory_order_release);
        }
        slot.next = 0;
        LOGI("render", "view %zu resized to %dx%d", viewIndex, slot.width,
             slot.height);
    }

    if (!renderer_->beginFrame(slot.swapChain)) return;
    renderer_->render(slot.view);

    // Start a readback into whichever buffer is free. If both are still with
    // the GPU we skip the capture for this frame rather than blocking - the
    // stream drops a frame, which is far better than stalling the CPU.
    ViewSlot::Capture& cap = slot.captures[slot.next];
    if (!cap.inFlight && !slot.pendingW) {
        using namespace filament::backend;
        cap.ready->store(false, std::memory_order_release);
        // The flag is shared with the callback so a late completion after the
        // renderer is gone cannot touch freed memory.
        auto* flag = new std::shared_ptr<std::atomic<bool>>(cap.ready);
        PixelBufferDescriptor pbd(
            cap.pixels.data(), cap.pixels.size(), PixelDataFormat::RGBA,
            PixelDataType::UBYTE,
            [](void* /*buffer*/, size_t /*size*/, void* user) {
                auto* held = static_cast<std::shared_ptr<std::atomic<bool>>*>(user);
                (*held)->store(true, std::memory_order_release);
                delete held;
            },
            flag);

        // Reads the framebuffer back to CPU. On the Vulkan backend the data is
        // top-down, so no flip is applied downstream (see VideoStreamer).
        renderer_->readPixels(0, 0, uint32_t(slot.width),
                              uint32_t(slot.height), std::move(pbd));
        cap.inFlight = true;
        slot.pending.push_back(slot.next);
        slot.next = (slot.next + 1) % ViewSlot::kCaptureBuffers;
    }

    // No flushAndWait() here: that call was the stall. The frame is delivered
    // on a later call, once the copy has landed.
    renderer_->endFrame();
}

void Renderer::finishPendingReadbacks() {
    if (!engine_) return;
    engine_->flushAndWait();  // shutdown only: every callback has now fired
    for (auto& slot : views_) {
        for (auto& cap : slot.captures) cap.inFlight = false;
        slot.pending.clear();
    }
}

}  // namespace wizengine
