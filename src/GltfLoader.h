#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <math/mat4.h>
#include <math/vec3.h>

namespace filament {
class Engine;
class Scene;
}  // namespace filament

namespace filament::gltfio {
class AssetLoader;
class FilamentAsset;
class FilamentInstance;
class MaterialProvider;
}  // namespace filament::gltfio

namespace wizengine {

// Loads glTF (.gltf) and binary glTF (.glb) files into a Filament scene via
// gltfio, which brings the model's own materials and textures with it.
//
// 使い方は「原型 + 実体」の 2 段:
//   loadModel(path)        … ファイルを 1 回だけ読み、モデル番号を返す
//                            （同じパスの 2 回目以降はキャッシュを返す）
//   createInstance(model)  … そのモデルの実体を 1 個。メッシュ・マテリアル・
//                            テクスチャは原型と共有なので、実体は何個でも安い
//   releaseInstance(id)    … gltfio は実体を 1 個だけ壊せないので、スケール 0
//                            に潰して同じモデルの空き番号として再利用に回す
//
// シーン文書の <asset><mesh/> がそのままこの語彙に対応する。gltfio の型は
// この壁の外に漏らさない（Renderer が薄く包んで Scene から使う）。
//
// Built only when CMake found the gltfio libraries (WIZ_HAVE_GLTFIO). Without
// them loadModel() throws AssetError, so a scene that asks for a model still
// fails with a readable message instead of a null crash.
class GltfLoader {
public:
    static constexpr std::size_t kInvalid = static_cast<std::size_t>(-1);

    GltfLoader(filament::Engine& engine, filament::Scene& scene);
    ~GltfLoader();

    GltfLoader(const GltfLoader&) = delete;
    GltfLoader& operator=(const GltfLoader&) = delete;

    // ファイルからモデルの原型を読む（.glb / .gltf は中身で判別）。同じパスは
    // 1 回しか読まない。THROWS AssetError（見つからない・壊れている・glTF
    // 非対応ビルド）- 読む側が投げるので、シーンに足したモデルの検証漏れが
    // 起きない（AssetError.h の方針）。RENDER スレッド。
    std::size_t loadModel(const std::string& path);

    // モデルのバウンディングボックスの最長辺（モデル自身の単位）。
    // 起動ログに出して、<mesh scale> を決める目安にする。
    float modelSize(std::size_t modelId) const;

    // 実体を 1 個作り、実体番号を返す。release 済みの空きがあればそれを
    // 再利用する。失敗（範囲外・非対応ビルド）は kInvalid。RENDER スレッド。
    std::size_t createInstance(std::size_t modelId);
    // 実体を見えなくして（スケール 0）、同じモデルの空き番号として返す。
    void releaseInstance(std::size_t index);

    // 実体の姿勢と、掴み中のハイライト（ベース色を明るく + エミッシブ）。
    void setInstanceTransform(std::size_t index,
                              const filament::math::mat4f& transform);
    void setInstanceTint(std::size_t index, const filament::math::float3& color,
                         float amount);

private:
    struct Model {
        filament::gltfio::FilamentAsset* asset = nullptr;
        std::string path;   // assetPath 解決後（キャッシュの鍵）
        float size = 0.0f;  // バウンディングボックスの最長辺
        std::vector<std::size_t> freeInstances;  // release 済みの実体番号
    };

    filament::Engine& engine_;
    filament::Scene& scene_;
    filament::gltfio::AssetLoader* loader_ = nullptr;
    filament::gltfio::MaterialProvider* materials_ = nullptr;
    std::vector<Model> models_;
    std::vector<filament::gltfio::FilamentInstance*> instances_;
    std::vector<std::size_t> instanceModel_;  // 実体番号 → モデル番号
};

}  // namespace wizengine
