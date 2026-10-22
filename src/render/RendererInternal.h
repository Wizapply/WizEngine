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
//   RendererGround.cpp    : 地面
//   RendererTerrain.cpp   : テレイン（チャンク分割の高さメッシュ）
//   RendererTextures.cpp  : テクスチャ読込（地面とテレインで共用）

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
//   White   … 白 1 画素 = baseColor の単色
enum class TextureFallback { Checker, White };

// assets/ 相対の画像（PNG/JPEG/TGA/BMP）を sRGB の 2D テクスチャとして作り、
// ミップ列を自前のボックスフィルタで生成してアップロードする（generateMipmaps
// は版によって usage フラグが要るため使わない）。読めない・空なら fallback の
// 絵（リニア RGBA8）で作り、指定があったのに読めなければ警告する。失敗で
// 止めない（テクスチャはシーン文書 = 手で書ける値なので降格する）。
// label はログの主語（"ground" なら "ground texture: ..."）。戻り値は呼び出し側が
// engine.destroy する。RendererTextures.cpp。
filament::Texture* loadTextureRGBA(filament::Engine& engine,
                                   const std::string& texturePath,
                                   const char* label,
                                   TextureFallback fallback);

// エンティティの変換（無ければ変換の部品を作ってから当てる）。地面と
// テレインのチャンクが使う。
inline void setEntityTransform(filament::Engine& engine, utils::Entity entity,
                               const filament::math::mat4f& transform) {
    auto& tcm = engine.getTransformManager();
    if (!tcm.hasComponent(entity)) tcm.create(entity);
    tcm.setTransform(tcm.getInstance(entity), transform);
}

}  // namespace render_detail
