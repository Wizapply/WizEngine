#include "render/Renderer.h"
#include "core/Log.h"

#include <filament/Box.h>
#include <filament/Camera.h>
#include <filament/Color.h>
#include <filament/ColorGrading.h>
#include <filament/Options.h>
#include <filament/Skybox.h>
#include <filament/ToneMapper.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/IndirectLight.h>
#include <filament/Texture.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <backend/PixelBufferDescriptor.h>
#include <geometry/SurfaceOrientation.h>
#include <utils/EntityManager.h>

#include <math/vec3.h>
#include <math/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <vector>

#include "render/GltfLoader.h"
#include "core/AssetError.h"
#include "render/EnvironmentLoader.h"
#include "render/ImageLoader.h"
#include <iterator>
#include <stdexcept>

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;

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

// Flat ground quad at y=0, facing +Y. Positions are built per-size in
// addGround(); here we keep the shared normals and indices.
const float3 kGroundNrm[4] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
const uint16_t kGroundIdx[6] = {0, 2, 1, 0, 3, 2};

// Two triangles per face: (0,1,2) (2,3,0), offset by 4*face.
const uint16_t kCubeIdx[36] = {
    0, 1, 2, 2, 3, 0,        4, 5, 6, 6, 7, 4,
    8, 9, 10, 10, 11, 8,     12, 13, 14, 14, 15, 12,
    16, 17, 18, 18, 19, 16,  20, 21, 22, 22, 23, 20,
};

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open material package: " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

// UV 球の分割数。エディタで置く球はたいてい小さいので、これで十分に丸い。
constexpr int kSphereRings = 16;    // 緯度方向
constexpr int kSphereSectors = 24;  // 経度方向
// M_PI は MSVC だと _USE_MATH_DEFINES が要るので、自前で持つ。
constexpr float kPi = 3.14159265358979323846f;

// 太線 1 本ぶんの頂点数と三角形の頂点インデックス数。板 2 枚 = 4 三角形。
constexpr std::size_t kTubeVertices = 8;
constexpr std::size_t kTubeIndices = 12;

// 線分 a→b を「直交する 2 枚の板」に展開して out[0..7] へ書く。
// 板 1 は u 方向、板 2 は v 方向に幅を持つ。どちらか一方は必ずカメラに対して
// 開くので、ビューごとに作り直さなくても太く見える。
void buildTube(filament::math::float3* out, const filament::math::float3& a,
               const filament::math::float3& b, float half) {
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-9f || half <= 0.0f) {
        // 長さ 0 の線分（＝バッチの余り）は面積 0 の三角形にして消す。
        for (std::size_t i = 0; i < kTubeVertices; ++i) out[i] = a;
        return;
    }
    dx /= len;
    dy /= len;
    dz /= len;

    // 線と最も平行でない軸を基準に、直交する 2 方向を作る。
    const bool useY = std::abs(dy) < 0.9f;
    const float rx = useY ? 0.0f : 1.0f;
    const float ry = useY ? 1.0f : 0.0f;
    const float rz = 0.0f;

    float ux = dy * rz - dz * ry;
    float uy = dz * rx - dx * rz;
    float uz = dx * ry - dy * rx;
    const float ul = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (ul < 1e-9f) {
        for (std::size_t i = 0; i < kTubeVertices; ++i) out[i] = a;
        return;
    }
    ux = ux / ul * half;
    uy = uy / ul * half;
    uz = uz / ul * half;

    // v = dir x u（u と dir が直交かつ単位なので、これも長さ half になる）
    const float vx = dy * uz - dz * uy;
    const float vy = dz * ux - dx * uz;
    const float vz = dx * uy - dy * ux;

    out[0] = {a.x - ux, a.y - uy, a.z - uz};
    out[1] = {a.x + ux, a.y + uy, a.z + uz};
    out[2] = {b.x + ux, b.y + uy, b.z + uz};
    out[3] = {b.x - ux, b.y - uy, b.z - uz};
    out[4] = {a.x - vx, a.y - vy, a.z - vz};
    out[5] = {a.x + vx, a.y + vy, a.z + vz};
    out[6] = {b.x + vx, b.y + vy, b.z + vz};
    out[7] = {b.x - vx, b.y - vy, b.z - vz};
}

// 影の設定（ライトごと）。カスケードは平行光だけの概念なので、点光源・
// スポットには 1 を渡す（Filament は無視するが、意味の無い値を持たせない）。
filament::LightManager::ShadowOptions shadowOptionsFrom(
    const wizengine::RenderSettings& s, bool directional) {
    filament::LightManager::ShadowOptions o;
    o.mapSize = s.shadowMap;
    o.shadowCascades = directional ? s.cascades : uint8_t(1);
    // 接地部の細かい影。影マップの解像度では拾えない「物と床の隙間」を
    // スクリーン空間のレイマーチで足す（浮いて見える問題への定番の処置）。
    o.screenSpaceContactShadows = s.contactShadows;
    // DPCF / PCSS の柔らかさは「光源の見かけの大きさ」で決まる。既定の
    // 0.02 は小さすぎて PCF とほとんど変わらないので、少し大きくする
    // （太陽の視半径ぶん = 現実の影の縁のぼけ方に近い）。
    if (s.shadow == wizengine::RenderSettings::Shadow::Dpcf ||
        s.shadow == wizengine::RenderSettings::Shadow::Pcss) {
        o.shadowBulbRadius = 0.1f;
    }
    return o;
}

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

// 一様な弱いアンビエント（環境マップ無し）。起動時と、シーンが環境光を
// 持たないとき（<environment hdr=""> やシーンの全消し）に使う。
void Renderer::installFlatAmbient() {
    const float3 ambientSH[1] = {float3{1.0f, 1.0f, 1.05f}};
    filament::IndirectLight* flat = filament::IndirectLight::Builder()
                                        .irradiance(1, ambientSH)
                                        .intensity(30000.0f)
                                        .build(*engine_);
    scene_->setIndirectLight(flat);
    // 背景は環境マップから作るので、一様アンビエントに戻すときは一緒に外す
    // （消えるテクスチャを参照させない）。
    if (skybox_) {
        scene_->setSkybox(nullptr);
        engine_->destroy(skybox_);
        skybox_ = nullptr;
    }
    if (ibl_) engine_->destroy(ibl_);
    if (iblTexture_) {
        engine_->destroy(iblTexture_);
        iblTexture_ = nullptr;
    }
    ibl_ = flat;
}

void Renderer::clearEnvironment() {
    installFlatAmbient();
}

void Renderer::ensureSphereMesh() {
    if (sphereVb_) return;

    // 半径 0.5 の単位球。箱と同じく「サイズ 1 で直径 1」になるので、
    // Scene 側は形が変わってもスケール行列を同じ考え方で組める。
    constexpr int rings = kSphereRings;
    constexpr int sectors = kSphereSectors;
    // 個数は最初から size_t で持つ。int を受け口でキャストすると
    // `std::vector<float3> normals(std::size_t(vertexCount));` が
    // 「関数の宣言」に解釈されてしまう（most vexing parse）。
    const std::size_t vertexCount =
        std::size_t(rings + 1) * std::size_t(sectors + 1);
    const std::size_t indexCount = std::size_t(rings) * std::size_t(sectors) * 6;

    auto* positions = new float3[vertexCount];
    std::vector<float3> normals(vertexCount);
    for (int r = 0; r <= rings; ++r) {
        const float theta = kPi * float(r) / float(rings);
        const float sinT = std::sin(theta);
        const float cosT = std::cos(theta);
        for (int s = 0; s <= sectors; ++s) {
            const float phi = 2.0f * kPi * float(s) / float(sectors);
            const float3 n{sinT * std::cos(phi), cosT, sinT * std::sin(phi)};
            const int i = r * (sectors + 1) + s;
            normals[std::size_t(i)] = n;
            positions[i] = float3{n.x * 0.5f, n.y * 0.5f, n.z * 0.5f};
        }
    }

    // 法線から接空間へ。頂点数が可変なので、立方体のように固定長メンバへは
    // 置けない（この配列は VertexBuffer へ渡したあと解放コールバックで消す）。
    auto* tangents = new quatf[vertexCount];
    auto* orient = filament::geometry::SurfaceOrientation::Builder()
                       .vertexCount(vertexCount)
                       .normals(normals.data())
                       .build();
    orient->getQuats(tangents, vertexCount);
    delete orient;

    auto* indices = new uint16_t[indexCount];
    int k = 0;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            const uint16_t a = uint16_t(r * (sectors + 1) + s);
            const uint16_t b = uint16_t(a + sectors + 1);
            // 外向きが表になる巻き方（a, a+1, b）／（b, a+1, b+1）。
            indices[k++] = a;
            indices[k++] = uint16_t(a + 1);
            indices[k++] = b;
            indices[k++] = b;
            indices[k++] = uint16_t(a + 1);
            indices[k++] = uint16_t(b + 1);
        }
    }

    sphereVb_ = VertexBuffer::Builder()
                    .vertexCount(uint32_t(vertexCount))
                    .bufferCount(2)
                    .attribute(VertexAttribute::POSITION, 0,
                               VertexBuffer::AttributeType::FLOAT3)
                    .attribute(VertexAttribute::TANGENTS, 1,
                               VertexBuffer::AttributeType::FLOAT4)
                    .build(*engine_);
    sphereVb_->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            positions, sizeof(float3) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));
    sphereVb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(
            tangents, sizeof(quatf) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));

    sphereIb_ = IndexBuffer::Builder()
                    .indexCount(uint32_t(indexCount))
                    .bufferType(IndexBuffer::IndexType::USHORT)
                    .build(*engine_);
    sphereIb_->setBuffer(
        *engine_,
        IndexBuffer::BufferDescriptor(
            indices, sizeof(uint16_t) * indexCount,
            [](void* p, size_t, void*) { delete[] static_cast<uint16_t*>(p); }));
    sphereIndexCount_ = uint32_t(indexCount);
}

void Renderer::ensureCylinderMesh() {
    if (cylinderVb_) return;

    // 軸が X、半径 0.5、長さ 1 の単位円柱（x = -0.5 .. +0.5）。側面は
    // 2 つのリング、両端は中心 + リングの扇で塞ぐ。法線は側面が放射方向、
    // 蓋が ±X なので、蓋とリングの頂点は側面と共有しない（角が丸まらない）。
    constexpr int sectors = kSphereSectors;
    const std::size_t ring = std::size_t(sectors + 1);
    // 側面 2 リング + 蓋 2 枚（中心 1 + リング）。
    const std::size_t vertexCount = ring * 2 + (ring + 1) * 2;
    const std::size_t indexCount =
        std::size_t(sectors) * 6 + std::size_t(sectors) * 3 * 2;

    auto* positions = new float3[vertexCount];
    std::vector<float3> normals(vertexCount);
    std::size_t v = 0;
    for (int side = 0; side < 2; ++side) {
        const float x = side == 0 ? -0.5f : 0.5f;
        for (int s = 0; s <= sectors; ++s) {
            const float phi = 2.0f * kPi * float(s) / float(sectors);
            const float3 n{0.0f, std::cos(phi), std::sin(phi)};
            normals[v] = n;
            positions[v] = float3{x, n.y * 0.5f, n.z * 0.5f};
            ++v;
        }
    }
    const std::size_t capStart[2] = {v, v + ring + 1};
    for (int side = 0; side < 2; ++side) {
        const float x = side == 0 ? -0.5f : 0.5f;
        const float3 n{x < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
        normals[v] = n;
        positions[v] = float3{x, 0.0f, 0.0f};
        ++v;
        for (int s = 0; s <= sectors; ++s) {
            const float phi = 2.0f * kPi * float(s) / float(sectors);
            normals[v] = n;
            positions[v] = float3{x, 0.5f * std::cos(phi), 0.5f * std::sin(phi)};
            ++v;
        }
    }

    auto* tangents = new quatf[vertexCount];
    auto* orient = filament::geometry::SurfaceOrientation::Builder()
                       .vertexCount(vertexCount)
                       .normals(normals.data())
                       .build();
    orient->getQuats(tangents, vertexCount);
    delete orient;

    auto* indices = new uint16_t[indexCount];
    std::size_t k = 0;
    for (int s = 0; s < sectors; ++s) {
        // 側面: 左リング a, a+1 と右リング b, b+1。外向き（放射方向）が表に
        // なる巻き方 (a, a+1, b) / (a+1, b+1, b)。
        const uint16_t a = uint16_t(s);
        const uint16_t b = uint16_t(ring + std::size_t(s));
        indices[k++] = a;
        indices[k++] = uint16_t(a + 1);
        indices[k++] = b;
        indices[k++] = uint16_t(a + 1);
        indices[k++] = uint16_t(b + 1);
        indices[k++] = b;
    }
    for (int side = 0; side < 2; ++side) {
        const uint16_t c = uint16_t(capStart[side]);
        for (int s = 0; s < sectors; ++s) {
            const uint16_t p = uint16_t(capStart[side] + 1 + std::size_t(s));
            indices[k++] = c;
            if (side == 0) {  // -X の蓋は外から見て逆回り
                indices[k++] = uint16_t(p + 1);
                indices[k++] = p;
            } else {
                indices[k++] = p;
                indices[k++] = uint16_t(p + 1);
            }
        }
    }

    cylinderVb_ = VertexBuffer::Builder()
                      .vertexCount(uint32_t(vertexCount))
                      .bufferCount(2)
                      .attribute(VertexAttribute::POSITION, 0,
                                 VertexBuffer::AttributeType::FLOAT3)
                      .attribute(VertexAttribute::TANGENTS, 1,
                                 VertexBuffer::AttributeType::FLOAT4)
                      .build(*engine_);
    cylinderVb_->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            positions, sizeof(float3) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));
    cylinderVb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(
            tangents, sizeof(quatf) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<quatf*>(p); }));

    cylinderIb_ = IndexBuffer::Builder()
                      .indexCount(uint32_t(indexCount))
                      .bufferType(IndexBuffer::IndexType::USHORT)
                      .build(*engine_);
    cylinderIb_->setBuffer(
        *engine_,
        IndexBuffer::BufferDescriptor(
            indices, sizeof(uint16_t) * indexCount,
            [](void* p, size_t, void*) { delete[] static_cast<uint16_t*>(p); }));
    cylinderIndexCount_ = uint32_t(indexCount);
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

std::size_t Renderer::addLight(const LightDesc& desc) {
    LightManager::Type type = LightManager::Type::DIRECTIONAL;
    switch (desc.type) {
        case LightDesc::Type::Directional:
            type = LightManager::Type::DIRECTIONAL;
            break;
        case LightDesc::Type::Point:
            type = LightManager::Type::POINT;
            break;
        case LightDesc::Type::Spot:
            // FOCUSED_SPOT keeps the light's total energy constant when the
            // cone angle changes - the physically sensible behaviour. Plain
            // SPOT keeps the per-area brightness instead.
            type = LightManager::Type::FOCUSED_SPOT;
            break;
    }

    utils::Entity e = EntityManager::get().create();
    LightManager::Builder builder(type);
    builder.color(desc.color)
        .intensity(desc.intensity)
        .direction(desc.direction)
        .castShadows(desc.castShadows)
        .shadowOptions(shadowOptionsFrom(
            settings_, type == LightManager::Type::DIRECTIONAL));
    if (desc.type != LightDesc::Type::Directional) {
        builder.position(desc.position).falloff(desc.falloffRadius);
    }
    if (desc.type == LightDesc::Type::Spot) {
        builder.spotLightCone(desc.spotInnerRadians, desc.spotOuterRadians);
    }
    builder.build(*engine_, e);
    scene_->addEntity(e);
    // removeLight で空いた席があれば再利用（番号を詰めない）。
    for (std::size_t i = 0; i < lightEntities_.size(); ++i) {
        if (lightEntities_[i].isNull()) {
            lightEntities_[i] = e;
            return i;
        }
    }
    lightEntities_.push_back(e);
    return lightEntities_.size() - 1;
}

void Renderer::removeLight(std::size_t index) {
    if (index >= lightEntities_.size()) return;
    utils::Entity& e = lightEntities_[index];
    if (e.isNull()) return;
    // removeShape と同じ後始末（engine_->destroy がコンポーネントも壊す）。
    scene_->remove(e);
    engine_->destroy(e);
    EntityManager::get().destroy(e);
    e = utils::Entity();  // 空席の印。addLight が再利用する
}

void Renderer::updateLight(std::size_t index, const float3& color,
                           float intensity, const float3& direction,
                           const float3& position) {
    if (index >= lightEntities_.size() || lightEntities_[index].isNull()) return;
    auto& lm = engine_->getLightManager();
    const auto li = lm.getInstance(lightEntities_[index]);
    if (!li) return;
    lm.setColor(li, color);
    lm.setIntensity(li, intensity);
    lm.setDirection(li, direction);
    // Harmless on a directional light (which ignores its position).
    lm.setPosition(li, position);
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

bool Renderer::loadEnvironment(const std::string& hdrName, float intensity) {
    // Decode + GPU prefilter live in EnvironmentLoader; this method only
    // installs the result in the scene and manages ownership of the previous
    // environment.
    const EnvironmentIBL env = loadEnvironmentIBL(*engine_, hdrName, intensity);
    scene_->setIndirectLight(env.light);
    // 背景（スカイボックス）は古いキューブマップを参照しているので、
    // 差し替える前に外す。
    if (skybox_) {
        scene_->setSkybox(nullptr);
        engine_->destroy(skybox_);
        skybox_ = nullptr;
    }
    if (ibl_) engine_->destroy(ibl_);
    if (iblTexture_) engine_->destroy(iblTexture_);
    ibl_ = env.light;
    iblTexture_ = env.reflections;
    envIntensity_ = intensity;
    refreshSkybox();
    return true;
}

void Renderer::setSkyboxEnabled(bool enabled) {
    if (skyboxWanted_ == enabled && (skybox_ != nullptr) == enabled) return;
    skyboxWanted_ = enabled;
    refreshSkybox();
}

// いまの環境マップから背景を作り直す（無効・環境マップ無しなら外す）。
// プリフィルタ済みのキューブマップをそのまま使う: mip 0 は元のパノラマ
// そのものなので、映り込みと背景が必ず一致する（別々に持つとずれる）。
void Renderer::refreshSkybox() {
    const bool want = skyboxWanted_ && iblTexture_ != nullptr;
    if (!want) {
        if (skybox_) {
            scene_->setSkybox(nullptr);
            engine_->destroy(skybox_);
            skybox_ = nullptr;
        }
        return;
    }
    Skybox* next = Skybox::Builder()
                       .environment(iblTexture_)
                       .intensity(envIntensity_)
                       .build(*engine_);
    if (!next) {
        LOGW("render", "skybox could not be built - keeping the flat background");
        return;
    }
    scene_->setSkybox(next);
    if (skybox_) engine_->destroy(skybox_);
    skybox_ = next;
}

bool Renderer::ensureLineMaterial() {
    if (lineMaterial_) return true;
    const auto pkg = readFile(assetPath("line.filamat"));
    if (pkg.empty()) {
        LOGW("render", "line.filamat not found - lines disabled");
        return false;
    }
    lineMaterial_ =
        Material::Builder().package(pkg.data(), pkg.size()).build(*engine_);
    return lineMaterial_ != nullptr;
}

Renderer::LineEntity Renderer::createLine(const filament::math::float3& color) {
    LineEntity line;
    // Two vertices, rewritten every frame; the index buffer never changes.
    line.vb = VertexBuffer::Builder()
                  .vertexCount(2)
                  .bufferCount(1)
                  .attribute(VertexAttribute::POSITION, 0,
                             VertexBuffer::AttributeType::FLOAT3, 0,
                             sizeof(float) * 3)
                  .build(*engine_);
    static const uint16_t kIndices[2] = {0, 1};
    line.ib = IndexBuffer::Builder()
                  .indexCount(2)
                  .bufferType(IndexBuffer::IndexType::USHORT)
                  .build(*engine_);
    line.ib->setBuffer(
        *engine_,
        IndexBuffer::BufferDescriptor(kIndices, sizeof(kIndices), nullptr));

    line.mi = lineMaterial_->createInstance();
    line.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                          float4{color.x, color.y, color.z, 1.0f});

    line.entity = EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{-1000.0f, -1000.0f, -1000.0f},
                      {1000.0f, 1000.0f, 1000.0f}})  // never culled
        .geometry(0, RenderableManager::PrimitiveType::LINES, line.vb, line.ib,
                  0, 2)
        .material(0, line.mi)
        .culling(false)
        .castShadows(false)
        .receiveShadows(false)
        .build(*engine_, line.entity);
    return line;  // added to the scene only while visible
}

void Renderer::updateLine(LineEntity& line, const filament::math::float3& from,
                          const filament::math::float3& to, bool visible) {
    if (!visible) {
        if (line.inScene) {
            scene_->remove(line.entity);
            line.inScene = false;
        }
        return;
    }

    // The buffer must outlive the call: Filament copies it asynchronously, so
    // hand over heap memory and free it from the completion callback.
    auto* verts = new float[6]{from.x, from.y, from.z, to.x, to.y, to.z};
    line.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            verts, sizeof(float) * 6,
            [](void* buffer, size_t, void*) {
                delete[] static_cast<float*>(buffer);
            }));

    if (!line.inScene) {
        scene_->addEntity(line.entity);
        line.inScene = true;
    }
}

void Renderer::destroyLine(LineEntity& line) {
    if (line.inScene) scene_->remove(line.entity);
    engine_->destroy(line.entity);
    EntityManager::get().destroy(line.entity);
    engine_->destroy(line.vb);
    engine_->destroy(line.ib);
    engine_->destroy(line.mi);
    line = LineEntity{};
}

void Renderer::configureGrabLines(
    const std::vector<filament::math::float3>& colors) {
    if (!ensureLineMaterial()) return;
    for (const auto& c : colors) grabLines_.push_back(createLine(c));
}

void Renderer::setGrabLine(std::size_t index,
                           const filament::math::float3& from,
                           const filament::math::float3& to, bool visible) {
    if (index >= grabLines_.size()) return;
    updateLine(grabLines_[index], from, to, visible);
}

void Renderer::setJointLineCount(std::size_t count) {
    if (count == jointLines_.size()) return;
    if (count > jointLines_.size()) {
        if (!ensureLineMaterial()) return;
        while (jointLines_.size() < count) {
            // 色は setJointLine で毎回入れ直すので、ここでは白で作る。
            jointLines_.push_back(createLine({1.0f, 1.0f, 1.0f}));
        }
        return;
    }
    while (jointLines_.size() > count) {
        destroyLine(jointLines_.back());
        jointLines_.pop_back();
    }
}

void Renderer::setJointLine(std::size_t index,
                            const filament::math::float3& from,
                            const filament::math::float3& to,
                            const filament::math::float3& color, bool visible) {
    if (index >= jointLines_.size()) return;
    LineEntity& line = jointLines_[index];
    if (visible && line.mi) {
        line.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                              float4{color.x, color.y, color.z, 1.0f});
    }
    updateLine(line, from, to, visible);
}

void Renderer::configureLineBatches(
    const std::vector<filament::math::float3>& colors,
    std::size_t maxSegments) {
    if (!ensureLineMaterial() || maxSegments == 0) return;
    lineBatchCapacity_ = maxSegments;

    // 三角形の並びは固定（線分あたり 8 頂点 / 12 インデックス）。毎フレーム
    // 書き換えるのは頂点座標だけで、ジオメトリの構成には触らない。
    const std::size_t vertexCount = maxSegments * kTubeVertices;
    const std::size_t indexCount = maxSegments * kTubeIndices;
    for (const auto& c : colors) {
        LineBatch batch;
        batch.vb = VertexBuffer::Builder()
                       .vertexCount(uint32_t(vertexCount))
                       .bufferCount(1)
                       .attribute(VertexAttribute::POSITION, 0,
                                  VertexBuffer::AttributeType::FLOAT3, 0,
                                  sizeof(float) * 3)
                       .build(*engine_);

        auto* indices = new uint16_t[indexCount];
        for (std::size_t s = 0; s < maxSegments; ++s) {
            const uint16_t base = uint16_t(s * kTubeVertices);
            uint16_t* out = indices + s * kTubeIndices;
            // 板 1（u 方向）と板 2（v 方向）で 2 三角形ずつ。
            const uint16_t quad[2][4] = {
                {uint16_t(base + 0), uint16_t(base + 1), uint16_t(base + 2),
                 uint16_t(base + 3)},
                {uint16_t(base + 4), uint16_t(base + 5), uint16_t(base + 6),
                 uint16_t(base + 7)}};
            for (int q = 0; q < 2; ++q) {
                out[q * 6 + 0] = quad[q][0];
                out[q * 6 + 1] = quad[q][1];
                out[q * 6 + 2] = quad[q][2];
                out[q * 6 + 3] = quad[q][0];
                out[q * 6 + 4] = quad[q][2];
                out[q * 6 + 5] = quad[q][3];
            }
        }
        batch.ib = IndexBuffer::Builder()
                       .indexCount(uint32_t(indexCount))
                       .bufferType(IndexBuffer::IndexType::USHORT)
                       .build(*engine_);
        batch.ib->setBuffer(
            *engine_,
            IndexBuffer::BufferDescriptor(
                indices, sizeof(uint16_t) * indexCount,
                [](void* p, size_t, void*) {
                    delete[] static_cast<uint16_t*>(p);
                }));

        batch.mi = lineMaterial_->createInstance();
        batch.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                               float4{c.x, c.y, c.z, 1.0f});

        batch.entity = EntityManager::get().create();
        RenderableManager::Builder(1)
            .boundingBox({{-1000.0f, -1000.0f, -1000.0f},
                          {1000.0f, 1000.0f, 1000.0f}})  // never culled
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, batch.vb,
                      batch.ib, 0, indexCount)
            .material(0, batch.mi)
            .culling(false)
            .castShadows(false)
            .receiveShadows(false)
            .build(*engine_, batch.entity);

        // エディタ専用レイヤへ。これでバッチはシーンに入っていても、
        // setViewEditorLayerVisible で選んだビューにしか映らない。
        auto& rm = engine_->getRenderableManager();
        rm.setLayerMask(rm.getInstance(batch.entity), 0xFF, kLayerEditorOnly);

        lineBatches_.push_back(std::move(batch));
    }
}

void Renderer::setLineBatch(std::size_t index,
                            const std::vector<BatchShape>& shapes) {
    if (index >= lineBatches_.size()) return;
    LineBatch& batch = lineBatches_[index];

    if (shapes.empty()) {
        if (batch.inScene) {
            scene_->remove(batch.entity);
            batch.inScene = false;
        }
        batch.current.clear();
        return;
    }

    // 中身が前回と同じなら転送しない。エディタで手が止まっているあいだ、
    // 毎フレーム同じ数十 KB を GPU に送り続けても意味がない。
    if (batch.current == shapes) return;
    batch.current = shapes;

    // 余りは面積 0 の三角形で埋める。プリミティブ数を変えずに見た目だけ
    // 消せるので、ジオメトリを組み直す必要がない。
    const std::size_t vertexCount = lineBatchCapacity_ * kTubeVertices;
    auto* verts = new float3[vertexCount];
    const std::size_t used = std::min(shapes.size(), lineBatchCapacity_);
    for (std::size_t s = 0; s < used; ++s) {
        const BatchShape& shape = shapes[s];
        float3* out = verts + s * kTubeVertices;
        if (shape.width > 0.0f) {
            buildTube(out, shape.a, shape.b, shape.width * 0.5f);
            continue;
        }
        // 塗りつぶしの四角は前半（頂点 0-3 = 三角形 2 枚）だけを使い、
        // 後半は 1 点に潰して消す。
        out[0] = shape.a;
        out[1] = shape.b;
        out[2] = shape.c;
        out[3] = shape.d;
        for (std::size_t i = 4; i < kTubeVertices; ++i) out[i] = shape.a;
    }
    const float3 pad = used > 0 ? shapes[used - 1].a : float3{0.0f};
    for (std::size_t s = used; s < lineBatchCapacity_; ++s) {
        for (std::size_t i = 0; i < kTubeVertices; ++i) {
            verts[s * kTubeVertices + i] = pad;
        }
    }

    batch.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            verts, sizeof(float3) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));

    if (!batch.inScene) {
        scene_->addEntity(batch.entity);
        batch.inScene = true;
    }
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

void Renderer::addGround(float halfSize, const filament::math::float3& color,
                         float tileMeters, const std::string& texturePath,
                         const ShapeMaterial& material) {
    // 2 回目以降の呼び出しは作り直し（シーン文書の <ground> が実行時に
    // 変わるため）。Filament の destroy は使用中の GPU 資源を安全に遅延破棄
    // するので、前フレームが参照していても構わない。
    if (!groundEntity_.isNull()) {
        scene_->remove(groundEntity_);
        engine_->destroy(groundEntity_);
        EntityManager::get().destroy(groundEntity_);
        groundEntity_ = utils::Entity();
    }
    if (groundVb_) { engine_->destroy(groundVb_); groundVb_ = nullptr; }
    if (groundIb_) { engine_->destroy(groundIb_); groundIb_ = nullptr; }
    if (groundTexture_) {
        engine_->destroy(groundTexture_);
        groundTexture_ = nullptr;
    }

    groundMatInstance_->setParameter("baseColor", RgbType::LINEAR, color);
    // 地面の材質（つや消しのままか、濡れた路面のように映り込ませるか）。
    // 地面は自己発光もクリアコートも持たないので、必要な 3 つだけ。
    groundMatInstance_->setParameter("roughness", material.roughness);
    groundMatInstance_->setParameter("metallic", material.metallic);
    groundMatInstance_->setParameter("reflectance", material.reflectance);

    // ---- Ground texture --------------------------------------------------
    // An image file (PNG/JPEG/TGA/BMP) when the scene names one. 読めない・
    // 無いときは市松模様に落として警告する（テクスチャはシーン文書の内容 =
    // 手で書けるので、glTF メッシュと同じく止めずに降格する）。Image
    // colours are sRGB; the checker is a linear multiplier on baseColor.
    std::vector<uint8_t> pixels;
    int texW = 0;
    int texH = 0;
    const bool fromFile =
        !texturePath.empty() &&
        loadImageRGBA(assetPath(texturePath), pixels, texW, texH);
    if (fromFile) {
        LOGI("render", "ground texture: %s (%dx%d)",
             assetPath(texturePath).c_str(), texW, texH);
    } else {
        if (!texturePath.empty()) {
            LOGW("render", "ground texture '%s' not found, using generated checker",
                 texturePath.c_str());
        }
        texW = texH = 256;  // 2 x 2 squares, 128 px each
        pixels.resize(size_t(texW) * size_t(texH) * 4);
        for (int y = 0; y < texH; ++y) {
            for (int x = 0; x < texW; ++x) {
                const bool dark = (x < texW / 2) != (y < texH / 2);
                const uint8_t v = dark ? 105 : 255;
                uint8_t* p = pixels.data() + (size_t(y) * texW + size_t(x)) * 4;
                p[0] = v;
                p[1] = v;
                p[2] = v;
                p[3] = 255;
            }
        }
    }

    // Mip level count from the larger dimension.
    uint8_t levels = 1;
    for (int m = (texW > texH ? texW : texH); m > 1; m >>= 1) ++levels;

    groundTexture_ =
        filament::Texture::Builder()
            .width(uint32_t(texW))
            .height(uint32_t(texH))
            .levels(levels)
            .format(fromFile ? filament::Texture::InternalFormat::SRGB8_A8
                             : filament::Texture::InternalFormat::RGBA8)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .build(*engine_);

    // Upload every mip level ourselves (simple box filter). Filament's
    // generateMipmaps() needs a texture usage flag whose name varies between
    // versions, so building the chain by hand keeps this version-proof.
    std::vector<uint8_t> mip = pixels;
    int w = texW;
    int h = texH;
    for (int level = 0; level < int(levels); ++level) {
        const size_t bytes = size_t(w) * size_t(h) * 4;
        auto* buf = new uint8_t[bytes];
        std::memcpy(buf, mip.data(), bytes);
        filament::Texture::PixelBufferDescriptor pb(
            buf, bytes, filament::Texture::Format::RGBA,
            filament::Texture::Type::UBYTE,
            [](void* b, size_t, void*) { delete[] static_cast<uint8_t*>(b); });
        groundTexture_->setImage(*engine_, uint8_t(level), std::move(pb));

        if (w == 1 && h == 1) break;
        const int nw = (w > 1) ? w / 2 : 1;
        const int nh = (h > 1) ? h / 2 : 1;
        std::vector<uint8_t> next(size_t(nw) * size_t(nh) * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                const int x0 = (w > 1) ? 2 * x : 0;
                const int y0 = (h > 1) ? 2 * y : 0;
                const int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
                const int y1 = (y0 + 1 < h) ? y0 + 1 : y0;
                for (int c = 0; c < 4; ++c) {
                    const int sum = mip[(size_t(y0) * w + x0) * 4 + c] +
                                    mip[(size_t(y0) * w + x1) * 4 + c] +
                                    mip[(size_t(y1) * w + x0) * 4 + c] +
                                    mip[(size_t(y1) * w + x1) * 4 + c];
                    next[(size_t(y) * nw + size_t(x)) * 4 + c] = uint8_t(sum / 4);
                }
            }
        }
        mip.swap(next);
        w = nw;
        h = nh;
    }

    filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
        filament::TextureSampler::MagFilter::LINEAR,
        filament::TextureSampler::WrapMode::REPEAT);
    sampler.setAnisotropy(8.0f);
    groundMatInstance_->setParameter("checker", groundTexture_, sampler);

    // Flat quad at y=0 facing +Y, sized to halfSize. Positions/UVs kept in
    // members (Filament references, not copies).
    groundPos_[0] = {-halfSize, 0.0f, -halfSize};
    groundPos_[1] = {halfSize, 0.0f, -halfSize};
    groundPos_[2] = {halfSize, 0.0f, halfSize};
    groundPos_[3] = {-halfSize, 0.0f, halfSize};

    // One texture repeat spans tileMeters metres.
    const float tiles = (tileMeters > 0.0f) ? (2.0f * halfSize) / tileMeters : 1.0f;
    groundUv_[0] = {0.0f, 0.0f};
    groundUv_[1] = {tiles, 0.0f};
    groundUv_[2] = {tiles, tiles};
    groundUv_[3] = {0.0f, tiles};

    auto* groundOrient = filament::geometry::SurfaceOrientation::Builder()
                             .vertexCount(4)
                             .normals(kGroundNrm)
                             .build();
    groundOrient->getQuats(groundTangents_, 4);
    delete groundOrient;

    groundVb_ = VertexBuffer::Builder()
                    .vertexCount(4)
                    .bufferCount(3)
                    .attribute(VertexAttribute::POSITION, 0,
                               VertexBuffer::AttributeType::FLOAT3)
                    .attribute(VertexAttribute::TANGENTS, 1,
                               VertexBuffer::AttributeType::FLOAT4)
                    .attribute(VertexAttribute::UV0, 2,
                               VertexBuffer::AttributeType::FLOAT2)
                    .build(*engine_);
    groundVb_->setBufferAt(*engine_, 0,
                           VertexBuffer::BufferDescriptor(groundPos_, sizeof(groundPos_)));
    groundVb_->setBufferAt(
        *engine_, 1,
        VertexBuffer::BufferDescriptor(groundTangents_, sizeof(groundTangents_)));
    groundVb_->setBufferAt(*engine_, 2,
                           VertexBuffer::BufferDescriptor(groundUv_, sizeof(groundUv_)));

    groundIb_ = IndexBuffer::Builder()
                    .indexCount(6)
                    .bufferType(IndexBuffer::IndexType::USHORT)
                    .build(*engine_);
    groundIb_->setBuffer(
        *engine_, IndexBuffer::BufferDescriptor(kGroundIdx, sizeof(kGroundIdx)));

    groundEntity_ = EntityManager::get().create();
    RenderableManager::Builder(1)
        .boundingBox({{0, 0, 0}, {halfSize, 0.1f, halfSize}})
        .material(0, groundMatInstance_)
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, groundVb_, groundIb_, 0, 6)
        .culling(false)
        .castShadows(false)
        .receiveShadows(true)
        .build(*engine_, groundEntity_);
    scene_->addEntity(groundEntity_);
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

std::size_t Renderer::addLineSet(const filament::math::float3& color) {
    LineSet set;
    if (ensureLineMaterial()) {
        set.mi = lineMaterial_->createInstance();
        set.mi->setParameter("baseColor", RgbaType::PREMULTIPLIED_LINEAR,
                             float4{color.x, color.y, color.z, 1.0f});
    }
    // エンティティとバッファは最初の setLineSet で本数が決まってから作る。
    lineSets_.push_back(set);
    return lineSets_.size() - 1;
}

void Renderer::setLineSet(std::size_t id,
                          const std::vector<filament::math::float3>& points) {
    if (id >= lineSets_.size()) return;
    LineSet& set = lineSets_[id];
    if (!set.mi) return;  // line.filamat が無い環境では線は無効

    const std::size_t lines = points.size() / 2;
    if (lines == 0) {
        if (set.inScene) {
            scene_->remove(set.entity);
            set.inScene = false;
        }
        return;
    }

    // 本数が変わったらレンダラブルごと作り直す。頂点はぴったりの数だけ
    // 確保するので、余りを埋める工夫（面積 0 の三角形）も要らない。
    if (lines != set.lineCount) {
        if (set.lineCount > 0) {
            if (set.inScene) scene_->remove(set.entity);
            engine_->destroy(set.entity);
            EntityManager::get().destroy(set.entity);
            engine_->destroy(set.vb);
            engine_->destroy(set.ib);
            set.inScene = false;
        }
        set.lineCount = lines;
        const std::size_t vertexCount = lines * 2;

        set.vb = VertexBuffer::Builder()
                     .vertexCount(uint32_t(vertexCount))
                     .bufferCount(1)
                     .attribute(VertexAttribute::POSITION, 0,
                                VertexBuffer::AttributeType::FLOAT3, 0,
                                sizeof(float) * 3)
                     .build(*engine_);

        auto* indices = new uint16_t[vertexCount];
        for (std::size_t i = 0; i < vertexCount; ++i) indices[i] = uint16_t(i);
        set.ib = IndexBuffer::Builder()
                     .indexCount(uint32_t(vertexCount))
                     .bufferType(IndexBuffer::IndexType::USHORT)
                     .build(*engine_);
        set.ib->setBuffer(
            *engine_,
            IndexBuffer::BufferDescriptor(
                indices, sizeof(uint16_t) * vertexCount,
                [](void* p, size_t, void*) {
                    delete[] static_cast<uint16_t*>(p);
                }));

        set.entity = EntityManager::get().create();
        RenderableManager::Builder(1)
            .boundingBox({{-1000.0f, -1000.0f, -1000.0f},
                          {1000.0f, 1000.0f, 1000.0f}})  // never culled
            .geometry(0, RenderableManager::PrimitiveType::LINES, set.vb,
                      set.ib, 0, vertexCount)
            .material(0, set.mi)
            .culling(false)
            .castShadows(false)
            .receiveShadows(false)
            .build(*engine_, set.entity);
        auto& rm = engine_->getRenderableManager();
        rm.setLayerMask(rm.getInstance(set.entity), 0xFF, kLayerEditorOnly);
    }

    const std::size_t vertexCount = lines * 2;
    auto* verts = new float3[vertexCount];
    std::memcpy(verts, points.data(), sizeof(float3) * vertexCount);
    set.vb->setBufferAt(
        *engine_, 0,
        VertexBuffer::BufferDescriptor(
            verts, sizeof(float3) * vertexCount,
            [](void* p, size_t, void*) { delete[] static_cast<float3*>(p); }));

    if (!set.inScene) {
        scene_->addEntity(set.entity);
        set.inScene = true;
    }
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
