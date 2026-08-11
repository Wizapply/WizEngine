#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <math/mat4.h>
#include <math/quat.h>
#include <math/vec2.h>
#include <math/vec3.h>
#include <utils/Entity.h>

namespace filament {
class Engine;
class Renderer;
class Scene;
class View;
class Camera;
class SwapChain;
class VertexBuffer;
class IndexBuffer;
class Material;
class MaterialInstance;
class IndirectLight;
class Texture;
}  // namespace filament

// Headless Filament render engine. It sets up the device, camera, lights and
// shared cube mesh; the actual scene contents are added by the caller (Scene)
// via addShape() and addGround(). renderFrame() draws one frame offscreen and
// hands back the RGBA pixels.
namespace wizengine {

class GltfLoader;

// Everything the renderer needs to create or update a light, in renderer
// vocabulary (no Filament types leak out of Renderer.cpp). LightObject fills
// one of these; the scene never touches it directly.
struct LightDesc {
    enum class Type { Directional, Point, Spot };
    Type type = Type::Directional;
    filament::math::float3 color{1.0f, 1.0f, 1.0f};  // linear RGB
    float intensity = 70000.0f;  // lux (directional) / lumens (point, spot)
    filament::math::float3 direction{0.0f, -1.0f, 0.0f};
    filament::math::float3 position{0.0f, 3.0f, 0.0f};
    bool castShadows = false;
    float falloffRadius = 20.0f;    // point/spot reach in metres
    float spotInnerRadians = 0.4f;  // spot cone, full brightness
    float spotOuterRadians = 0.6f;  // spot cone, cutoff
};

// 組み込みメッシュの種類。エディタで置けるのはこの 2 つ（glTF モデルは
// インスタンスプール側で扱う）。
enum class ShapeMesh { Box, Sphere };

// 線バッチに詰める 1 図形。太線と塗りつぶしの面を同じ入れ物に混ぜられる。
//
//   width > 0 … a→b を太さ width の線として描く（下記のとおり板 2 枚）
//   width = 0 … a,b,c,d を四隅とする塗りつぶしの四角。三角形は d に c を
//               入れれば（＝潰れた四角として）そのまま描ける
//
// バッチの 1 スロットは「四角 2 枚 = 4 三角形」ぶんの席なので、太線はそれを
// 丸ごと、塗りつぶしは片側だけ使う。どちらも同じ席に収まるので、インデックス
// バッファもレンダラブルも作り直さずに混在させられる。
struct BatchShape {
    filament::math::float3 a{}, b{}, c{}, d{};
    float width = 0.0f;
};

inline bool operator==(const BatchShape& x, const BatchShape& y) {
    auto same = [](const filament::math::float3& p,
                   const filament::math::float3& q) {
        return p.x == q.x && p.y == q.y && p.z == q.z;
    };
    return x.width == y.width && same(x.a, y.a) && same(x.b, y.b) &&
           same(x.c, y.c) && same(x.d, y.d);
}

class Renderer {
public:
    Renderer(int width, int height, const std::string& materialPath);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // ---- Scene setup: 実行時に増減する形状（Scene から呼ぶ）-------------
    // addShape は空きスロットがあれば再利用するので、番号は密に詰まる代わりに
    // 「削除した番号が後から別の物に化ける」ことがある。Scene は必ず
    // 生存フラグと一緒に持つこと。すべて RENDER スレッドから呼ぶ。
    std::size_t addShape(ShapeMesh mesh);
    void removeShape(std::size_t id);
    // オブジェクトごとの色。最初の呼び出しでそのスロット専用のマテリアル
    // インスタンスを作る（共有インスタンスを書き換えると全部の色が変わる）。
    void setShapeColor(std::size_t id, const filament::math::float3& color);
    // ---- Lights ----------------------------------------------------------
    // Lights are owned by the scene as LightObjects (see scene.cpp,
    // lightConfigs()); the renderer only holds the Filament entities.
    // addLight() during Scene::build (single-threaded), updateLight() from
    // the render thread. Type and castShadows are fixed at creation; the
    // mutable properties go through updateLight.
    std::size_t addLight(const LightDesc& desc);
    void updateLight(std::size_t index, const filament::math::float3& color,
                     float intensity, const filament::math::float3& direction,
                     const filament::math::float3& position);
    // Colour shared by every box (lit material).
    void setBoxColor(const filament::math::float3& color);
    // One highlight style per camera, configured once by the scene, so each
    // viewer's selection has its own colour and several can be lit at once.
    void configureHighlightColors(
        const std::vector<filament::math::float3>& colors);
    // Draw a box with the given camera's highlight colour; -1 restores it.
    void setBoxHighlighted(std::size_t id, int styleIndex);
    // Vertical field of view in degrees - the picking code needs it to build
    // a ray through a screen position.
    float verticalFovDegrees() const;
    double aspect() const { return double(width_) / double(height_); }
    // tileMeters: how many metres one full repeat of the texture covers.
    // texturePath: image file (PNG etc) for the ground; if empty or missing,
    // a checkerboard is generated instead.
    void addGround(float halfSize, const filament::math::float3& color,
                   float tileMeters, const std::string& texturePath);

    // Load a glTF (.gltf) or binary glTF (.glb) model into the scene. Returns
    // an id for setModelTransform(). Throws AssetError if it cannot be loaded,
    // so any model named by the scene is validated without the caller having
    // to remember to check a return value.
    static constexpr std::size_t kInvalidModel = static_cast<std::size_t>(-1);
    std::size_t addModel(const std::string& path);

    // Use a glTF/GLB model instead of the built-in cube for the dynamic
    // objects: loads `path` once and creates `count` instances of it. Returns
    // Throws AssetError if the model cannot be loaded. Instance transforms go
    // through setModelInstanceTransform() instead of setBoxTransform().
    void createModelInstances(const std::string& path, std::size_t count);
    void setModelInstanceTransform(std::size_t index,
                                   const filament::math::mat4f& transform);
    // Largest dimension of the instanced model in its own units (0 if none).
    float modelInstanceSize() const;

    // Replaces the flat ambient light with an environment generated from an
    // HDR panorama. `name` is a Radiance .hdr file under assets/, e.g.
    // "studio.hdr"; it is decoded and prefiltered into a cubemap on the GPU
    // at load time (no build step - swap the file and restart). Throws
    // AssetError when the file is missing or not a decodable .hdr.
    //
    // This matters for glTF models: metals reflect the environment and have no
    // diffuse colour of their own, so without one they render black wherever
    // the direct lights do not hit them.
    bool loadEnvironment(const std::string& name, float intensity);

    // ---- Grab lines -------------------------------------------------------
    // A line drawn in the scene from a grabbed object to the point the user is
    // pulling it towards - one per camera, in that camera's highlight colour.
    // configureGrabLines() once at setup, setGrabLine() per frame (render
    // thread). Being real geometry, the line is visible from every camera.
    void configureGrabLines(const std::vector<filament::math::float3>& colors);
    void setGrabLine(std::size_t index, const filament::math::float3& from,
                     const filament::math::float3& to, bool visible);

    // ---- ジョイントの可視化 ----------------------------------------------
    // エディタで作った拘束を線で描く。グラブ線と同じ仕組み（1本 = 2頂点の
    // レンダラブル）で、本数だけ必要に応じて増やす。色は本ごとに指定できる
    // ので、種類（ちょうつがい・ボール…）を色分けできる。
    void setJointLineCount(std::size_t count);
    void setJointLine(std::size_t index, const filament::math::float3& from,
                      const filament::math::float3& to,
                      const filament::math::float3& color, bool visible);

    // ---- 太線バッチ（ギズモ用）-------------------------------------------
    // Filament に線の太さは無い（LINES はどのバックエンドでも 1 ピクセル）。
    // 太く見せるには線を面にするしかないので、1 本を「直交する 2 枚の板」＝
    // 4 三角形として描く。カメラを向く 1 枚の板にすると別のビューから真横に
    // なって消えてしまう - このシーンはビューが複数あって線を共有するため、
    // 向きに依存しないこの形を選んでいる。
    //
    // ギズモは 1 個で 150 本を超えるので、色ごとに 1 レンダラブルへ詰める。
    // 頂点数（＝プリミティブ数）は固定にして、余った分は面積 0 の三角形で
    // 埋める - 実行中にジオメトリの個数を変える API は Filament の版によって
    // 名前が違うので、触らずに済ませたい。
    //
    // configureLineBatches() は起動時に1回、setLineBatch() は毎フレーム
    // （中身が変わらなければ GPU への転送は省かれる）。すべて RENDER スレッド。
    void configureLineBatches(const std::vector<filament::math::float3>& colors,
                              std::size_t maxSegments);
    // 太さは図形ごとに持つ。ビューが複数あってカメラからの距離が違うので、
    // 1 個の値だとどれか 1 台にしか合わせられないため。空にすると非表示。
    void setLineBatch(std::size_t index, const std::vector<BatchShape>& shapes);

    // ---- レイヤ ----------------------------------------------------------
    // ギズモのように「エディタカメラのビューにだけ」見せたい物のための
    // レイヤ分け。シーンは全ビュー共有のままなので、レンダラブルの
    // レイヤマスクとビューの可視レイヤで映る/映らないを切り替える。
    // 通常のレンダラブルは kLayerScene（Filament の既定）に居て全ビューに映る。
    static constexpr uint8_t kLayerScene = 0x01;
    static constexpr uint8_t kLayerEditorOnly = 0x02;
    // このビューでエディタ専用レイヤ（＝線分バッチ）を見せるか。addView は
    // どのビューも「見せない」で作るので、エディタカメラのビューだけ
    // 明示的に true にする（main.cpp）。RENDER スレッド。
    void setViewEditorLayerVisible(std::size_t viewIndex, bool visible);
    std::size_t lineBatchCapacity() const { return lineBatchCapacity_; }

    // Tint a single model instance towards the given colour (amount 0 =
    // untouched). Used for the per-camera grab highlight on glTF models.
    void setModelInstanceTint(std::size_t index,
                              const filament::math::float3& color, float amount);
    void setModelTransform(std::size_t id,
                           const filament::math::float3& position,
                           const filament::math::quatf& rotation, float scale);
    // A view is one camera's worth of rendering: its own Filament camera,
    // view and swap chain, all drawing the same scene. View 0 always exists;
    // addView() creates more, one per extra camera.
    std::size_t addView();
    std::size_t viewCount() const { return views_.size(); }

    // Requests a new output size for one view. RENDER thread only. Applied
    // inside renderFrame() once that view has no readbacks in flight - the
    // GPU may still be copying into the old-size buffers, so the swap chain
    // and capture buffers cannot be replaced immediately.
    void requestViewResize(std::size_t viewIndex, int width, int height);

    void setCamera(const filament::math::double3& eye,
                   const filament::math::double3& target);
    void setCamera(std::size_t viewIndex, const filament::math::double3& eye,
                   const filament::math::double3& target);

    void setBoxTransform(std::size_t id, const filament::math::mat4f& transform);
    void renderFrame(const std::function<void(const uint8_t*, size_t)>& onFrame);
    // Renders one specific view. Views share the scene, so the geometry is
    // only updated once per frame no matter how many cameras there are.
    //
    // The readback is ASYNCHRONOUS: this returns as soon as the frame is
    // submitted, and onFrame is invoked for the frame captured one or two
    // frames ago, once the GPU has actually delivered it. That costs a frame
    // of latency and buys back the stall that waiting for the copy used to
    // cost - time the CPU now spends on physics instead. onFrame is always
    // called on this (render) thread, never from a driver thread.
    void renderFrame(std::size_t viewIndex,
                     const std::function<void(const uint8_t*, size_t)>& onFrame);

    // Blocks until every in-flight readback has completed. Only needed at
    // shutdown, so no buffer is freed while the GPU still owns it.
    void finishPendingReadbacks();

private:
    int width_;
    int height_;

    filament::Engine* engine_ = nullptr;
    filament::Renderer* renderer_ = nullptr;
    filament::Scene* scene_ = nullptr;
    // One entry per camera.
    struct ViewSlot {
        filament::View* view = nullptr;
        filament::Camera* camera = nullptr;
        filament::SwapChain* swapChain = nullptr;
        utils::Entity cameraEntity;

        int width = 0, height = 0;          // this view's output size
        int pendingW = 0, pendingH = 0;     // 0 = no resize requested

        // Two capture buffers per view, used alternately: the GPU fills one
        // while the previous one is handed to the encoder. A single buffer
        // would have to be waited on before it could be reused, which is
        // exactly the stall this avoids.
        static constexpr int kCaptureBuffers = 2;
        struct Capture {
            std::vector<uint8_t> pixels;
            // Set by the readPixels callback (a driver thread) and read by the
            // render thread, hence atomic.
            std::shared_ptr<std::atomic<bool>> ready;
            bool inFlight = false;
        };
        Capture captures[kCaptureBuffers];
        int next = 0;  // buffer to fill on the next frame
        // Buffers handed to the GPU and not yet delivered, oldest first.
        // A single "pending" slot is not enough: with two captures in flight
        // the older one would be forgotten and its buffer never released.
        std::deque<int> pending;
    };
    std::vector<ViewSlot> views_;
    filament::VertexBuffer* vb_ = nullptr;
    filament::IndexBuffer* ib_ = nullptr;
    filament::VertexBuffer* groundVb_ = nullptr;
    filament::IndexBuffer* groundIb_ = nullptr;
    // 球メッシュ（UV球）。使うシーンだけが作る＝箱しか置かないなら費用ゼロ。
    filament::VertexBuffer* sphereVb_ = nullptr;
    filament::IndexBuffer* sphereIb_ = nullptr;
    uint32_t sphereIndexCount_ = 0;
    void ensureSphereMesh();

    filament::Material* material_ = nullptr;
    filament::MaterialInstance* matInstance_ = nullptr;
    std::vector<filament::MaterialInstance*> highlightInstances_;

    // 実行時に増減する形状スロット。削除は「エンティティを壊して空きに戻す」
    // で、番号自体は残す（Scene 側の描画 ID を安定させるため）。
    struct ShapeSlot {
        utils::Entity entity;
        filament::MaterialInstance* mi = nullptr;  // 個別色。null = 共有
        int highlight = -1;                        // -1 = ハイライト無し
        bool used = false;
    };
    std::vector<ShapeSlot> shapes_;
    std::vector<std::size_t> freeShapes_;

    // 2頂点 1本の線。グラブ線とジョイント線で同じ作りを使う。
    struct LineEntity {
        utils::Entity entity;
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
        filament::MaterialInstance* mi = nullptr;
        bool inScene = false;
    };
    std::vector<LineEntity> grabLines_;
    std::vector<LineEntity> jointLines_;

    // 色ごとの太線バッチ。頂点バッファは固定長で、前回と同じ内容なら
    // 転送しない（静止したエディタ画面で毎フレーム数十 KB 送らないため）。
    struct LineBatch {
        utils::Entity entity;
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
        filament::MaterialInstance* mi = nullptr;
        std::vector<BatchShape> current;  // 直近に送った内容
        bool inScene = false;
    };
    std::vector<LineBatch> lineBatches_;
    std::size_t lineBatchCapacity_ = 0;  // 1バッチあたりの線分数

    filament::Material* lineMaterial_ = nullptr;
    // line.filamat の読み込み（初回のみ）。読めなければ false = 線は無効。
    bool ensureLineMaterial();
    LineEntity createLine(const filament::math::float3& color);
    void updateLine(LineEntity& line, const filament::math::float3& from,
                    const filament::math::float3& to, bool visible);
    void destroyLine(LineEntity& line);
    filament::Material* groundMaterial_ = nullptr;
    filament::MaterialInstance* groundMatInstance_ = nullptr;
    filament::IndirectLight* ibl_ = nullptr;
    filament::Texture* iblTexture_ = nullptr;  // reflections, when an IBL is loaded
    std::unique_ptr<GltfLoader> gltf_;
    filament::Texture* groundTexture_ = nullptr;

    utils::Entity groundEntity_;
    std::vector<utils::Entity> lightEntities_;  // one per addLight(), in order

    // Kept alive for the vertex buffers (Filament references, not copies).
    filament::math::quatf cubeTangents_[24];
    filament::math::quatf groundTangents_[4];
    filament::math::float3 groundPos_[4];
    filament::math::float2 groundUv_[4];

};

}  // namespace wizengine
