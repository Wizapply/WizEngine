#pragma once

// Renderer の実装ファイル（Renderer*.cpp）だけが使う私的ヘッダ。実装を担当ごとに
// 分けたので、共通の下準備（インクルード・小さな補助関数）を
// ここに 1 か所で持つ。公開 API は Renderer.h。他のファイルからは含めないこと。
//
//   Renderer.cpp          : 構築・破棄・ビュー・カメラ・フレームの描画と読み戻し・glTF への委譲
//   RendererSettings.cpp  : 描画設定（影・後処理・露出・色作り）
//   RendererLighting.cpp  : 環境光・スカイボックス・ライト
//   RendererShapes.cpp    : 形状スロット・ソフト形状・色と材質・ハイライト
//   RendererMeshes.cpp    : 立方体・球・円柱のメッシュ生成
//   RendererLines.cpp     : 線（グラブ線・ジョイント線・太線バッチ・細線セット）
//   RendererOverlays.cpp  : エディタ専用レイヤの頂点色つき半透明メッシュ
//   RendererGround.cpp    : 地面
//   RendererTerrain.cpp   : テレインのメッシュ（チャンク分割の高さメッシュ）
//   RendererTerrainSurface.cpp : テレインの表面（層のテクスチャ配列・スプラット・材質）
//   RendererTextures.cpp  : テクスチャ読込（地面とテレインで共用）

#include "render/Renderer.h"
#include "render/ImageScale.h"
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

#include <backend/BufferDescriptor.h>
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

#include "core/AssetError.h"
#include <iterator>
#include <stdexcept>

namespace render_detail {

inline std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open material package: " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

// 影の設定（ライトごと）。カスケードは平行光だけの概念なので、点光源・
// スポットには 1 を渡す（Filament は無視するが、意味の無い値を持たせない）。
inline filament::LightManager::ShadowOptions shadowOptionsFrom(
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

// 画像が読めない・指定が空のときの代わりの絵。
//   Checker … 2 x 2 の市松模様（地面の従来の見た目）
//   None    … 代わりの絵を作らない（画素は空）。呼び手が自分の代わりを持つ
//             （テレインの層 = RendererTerrainSurface.cpp の solidLayerPixels = 単色）
enum class TextureFallback { Checker, None };

// 以下 RendererTextures.cpp。テクスチャの部品は 3 つ（デコード → 大きさの
// 揃え直し → ミップ列のアップロード）で、地面の 2D テクスチャ（loadTextureRGBA
// = 3 つの合成）とテレインの層のテクスチャ配列が同じものを通る。大きさの
// 揃え直し（resizeRGBA）とミップの段数（mipLevelCount）は Filament を知らない
// 計算なので render/ImageScale.h（単体テストあり）。

// assets/ 相対の画像（PNG/JPEG/TGA/BMP）を RGBA8 の画素へデコードする。読めない・
// 空なら fallback の絵（リニア。None なら空）を入れ、指定があったのに読めなければ
// 警告する。
// 失敗で止めない（テクスチャはシーン文書 = 手で書ける値なので降格する）。
// label はログの主語（"ground" なら "ground texture: ..."）。
// 戻り値 = ファイルから読めた（画素は sRGB）か。
bool decodeTextureRGBA(const std::string& texturePath, const char* label,
                       TextureFallback fallback, std::vector<uint8_t>& pixels,
                       int& width, int& height);

// pixels（RGBA8、width × height = texture の段 0 の大きさ）を段 0 に入れ、
// ボックスフィルタ（ImageScale.h の halveRGBA）で作った縮小をテクスチャの全段へ
// 入れる（generateMipmaps は版によって usage フラグが要るため使わない）。layer は
// 配列の層の番号（2D テクスチャは 0）。画素は各段とも記述子へ移す（写さない）。
void uploadMipChain(filament::Engine& engine, filament::Texture* texture,
                    uint32_t layer, std::vector<uint8_t> pixels, int width,
                    int height);

// assets/ 相対の画像を 2D テクスチャとして作る（decodeTextureRGBA →
// uploadMipChain。読めた画像は sRGB、読めなければ市松模様 = Checker のリニア
// RGBA8）。戻り値は呼び出し側が engine.destroy する。
filament::Texture* loadTextureRGBA(filament::Engine& engine,
                                   const std::string& texturePath,
                                   const char* label);

// assets/ の .filamat を読んでマテリアルを作る。無い・空・壊れている場合は
// AssetError を投げる（黙って描かないのではなく、原因を示して止める）。
// what はエラー文の主語（"line material" など）。戻り値は呼び出し側が壊す。
inline filament::Material* loadMaterialPackage(filament::Engine& engine,
                                               const char* file,
                                               const char* what) {
    wizengine::requireFile(file, what);
    const std::string path = wizengine::assetPath(file);
    const auto pkg = readFile(path);
    if (pkg.empty()) throw wizengine::AssetError(path, "is empty");
    filament::Material* material = filament::Material::Builder()
                                       .package(pkg.data(), pkg.size())
                                       .build(engine);
    if (!material) throw wizengine::AssetError(path, "is not a valid .filamat");
    return material;
}

// 頂点・インデックスの転送用に count 個をヒープへ写した BufferDescriptor。
// Filament は非同期にコピーするので、呼び出し元の配列は渡せない -
// 写した領域は転送の完了コールバックで解放する。
template <class T>
filament::backend::BufferDescriptor copyToDescriptor(const T* data,
                                                     std::size_t count) {
    auto* copy = new T[count];
    std::memcpy(copy, data, sizeof(T) * count);
    return filament::backend::BufferDescriptor(
        copy, sizeof(T) * count,
        [](void* p, size_t, void*) { delete[] static_cast<T*>(p); });
}

// RGBA8 の画素の配列をそのまま記述子へ移す（copyToPixelDescriptor と違い写さない。
// 配列は Filament が読み終えたとき = 転送の完了コールバックで解放する）。
inline filament::Texture::PixelBufferDescriptor moveToPixelDescriptor(
    std::vector<uint8_t>&& rgba) {
    auto* owned = new std::vector<uint8_t>(std::move(rgba));
    return filament::Texture::PixelBufferDescriptor(
        owned->data(), owned->size(), filament::Texture::Format::RGBA,
        filament::Texture::Type::UBYTE,
        [](void*, size_t, void* user) { delete static_cast<std::vector<uint8_t>*>(user); },
        owned);
}

// テクスチャの転送用に RGBA8 の画素 bytes バイトをヒープへ写した
// PixelBufferDescriptor（copyToDescriptor の画素版。写した領域は転送の完了
// コールバックで解放する）。
inline filament::Texture::PixelBufferDescriptor copyToPixelDescriptor(
    const uint8_t* rgba, std::size_t bytes) {
    auto* copy = new uint8_t[bytes];
    std::memcpy(copy, rgba, bytes);
    return filament::Texture::PixelBufferDescriptor(
        copy, bytes, filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE,
        [](void* p, size_t, void*) { delete[] static_cast<uint8_t*>(p); });
}

// エディタ専用レイヤに乗るレンダラブルを 1 つ作る（カリングしない・影を
// 落とさず受けない）。シーンへの追加は呼び出し側（見せるときだけ足す）。
inline utils::Entity buildEditorRenderable(
    filament::Engine& engine, filament::RenderableManager::PrimitiveType type,
    filament::VertexBuffer* vb, filament::IndexBuffer* ib,
    std::size_t indexCount, filament::MaterialInstance* mi) {
    utils::Entity entity = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .boundingBox({{-1000.0f, -1000.0f, -1000.0f},
                      {1000.0f, 1000.0f, 1000.0f}})  // never culled
        .geometry(0, type, vb, ib, 0, indexCount)
        .material(0, mi)
        .culling(false)
        .castShadows(false)
        .receiveShadows(false)
        .build(engine, entity);
    auto& rm = engine.getRenderableManager();
    rm.setLayerMask(rm.getInstance(entity), 0xFF,
                    wizengine::Renderer::kLayerEditorOnly);
    return entity;
}

// 自前のバッファを持つレンダラブルを壊す（シーンから外す → エンティティ →
// 頂点 / インデックスバッファ）。壊した物は空に戻す。マテリアルインスタンスは
// 作り直しをまたいで使い回すので、ここでは壊さない。
inline void destroyRenderable(filament::Engine& engine, filament::Scene& scene,
                              utils::Entity& entity, filament::VertexBuffer*& vb,
                              filament::IndexBuffer*& ib, bool& inScene) {
    if (inScene) scene.remove(entity);
    inScene = false;
    engine.destroy(entity);
    utils::EntityManager::get().destroy(entity);
    entity = {};
    engine.destroy(vb);
    vb = nullptr;
    engine.destroy(ib);
    ib = nullptr;
}

// エンティティの変換（無ければ変換の部品を作ってから当てる）。地面と
// テレインのチャンクが使う。
inline void setEntityTransform(filament::Engine& engine, utils::Entity entity,
                               const filament::math::mat4f& transform) {
    auto& tcm = engine.getTransformManager();
    if (!tcm.hasComponent(entity)) tcm.create(entity);
    tcm.setTransform(tcm.getInstance(entity), transform);
}

}  // namespace render_detail
