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
#include <math/vec4.h>
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
class Skybox;
class ColorGrading;
}  // namespace filament

// ヘッドレスの Filament 描画エンジン。コンストラクタが作るのは下地だけ
// （デバイス・ビュー 0 とカメラ・材質・共有の立方体メッシュ・一様な環境光）。
// ライトは作らない - シーンの中身と同じく呼び出し側（Scene）が addLight() /
// addShape() / addGround() で足す。renderFrame() がオフスクリーンで 1 フレーム
// 描いて RGBA のピクセルを返す。
namespace wizengine {

class GltfLoader;

// ライトを作る・更新するのに要る値を、レンダラの語彙でまとめたもの。
// Filament のエンジンの型（LightManager など）は Renderer*.cpp の外に出さない
// （ここで使うのは math の値型だけ）。Scene はエディタ側の LightDesc
// （オイラー角）をこれ（方向ベクトル）に直して渡す。
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

// 形状 1 個ぶんの材質（Filament の lit マテリアルのパラメータ）。色は
// setShapeColor が別に持つ - 色だけを変える操作（イベントの SetColor、
// 掴んだときのハイライト）が材質を巻き込まないようにするため。
// 既定値は従来の見た目（艶消しの誘電体）。
struct ShapeMaterial {
    float roughness = 0.75f;
    float metallic = 0.0f;
    float reflectance = 0.5f;
    float clearCoat = 0.0f;
    float clearCoatRoughness = 0.03f;
    float emissive = 0.0f;  // baseColor の何倍を自己発光として足すか
};

// 「どう撮るか」の設定。シーン文書の <visual>（editor::RenderDesc）を
// レンダラの語彙に直したもので、Filament の View / Camera / ColorGrading と
// ライトの影オプションへそのまま流れる。既定値は**これまでの絵**
// （後処理はどれも切ってあり、露出も Filament の既定と同じ）。
//
// setRenderSettings は全ビューに一度に効く（シーンの設定であって、カメラ
// ごとの設定ではない）。あとから作ったビュー（カメラの遅延生成）にも
// addView が同じ設定を当てるので、ページを開いた順で絵が変わらない。
struct RenderSettings {
    enum class Shadow { Pcf, Dpcf, Pcss, Vsm };
    enum class Tonemap { AcesLegacy, Aces, Filmic, Agx, PbrNeutral, Linear };

    // 影とアンチエイリアス
    uint32_t shadowMap = 1024;
    uint8_t cascades = 1;
    Shadow shadow = Shadow::Pcf;
    bool contactShadows = false;
    uint8_t msaa = 1;  // 1 = 無効
    bool taa = false;
    bool fxaa = true;
    // 後処理
    bool postProcess = true;
    bool ssao = false;
    float ssaoIntensity = 1.0f;
    float bloom = 0.0f;  // 0 = 無効
    bool ssr = false;
    float dofFocus = 0.0f;  // 合焦距離 (m)。0 = 被写界深度なし
    float dofBlur = 1.0f;
    float vignette = 0.0f;  // 0 = 無効
    // 露出（写真と同じ 3 つ）
    float aperture = 16.0f;
    float shutter = 125.0f;  // 1/shutter 秒
    float sensitivity = 100.0f;  // ISO
    // フィルム
    Tonemap tonemap = Tonemap::AcesLegacy;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
};

// 組み込みメッシュの種類。エディタで置けるのは Box / Sphere（glTF モデルは
// インスタンスプール側で扱う）。Cylinder は車両のタイヤ用: 軸が X で、
// 直径 1・長さ 1 の単位円柱（スケール (幅, 直径, 直径) で車輪になる）。
enum class ShapeMesh { Box, Sphere, Cylinder };

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

// テレインのチャンク 1 枚ぶんの頂点（setTerrainChunk に渡す）。Renderer は
// 素の配列を受けるだけで、高さの作り方（ノイズ・スカルプト）もシーン文書も
// 知らない。
//
//   positions … 3 個で 1 頂点（ワールド座標 m）
//   normals   … 3 個で 1 頂点（単位ベクトル）
//   uvs       … 2 個で 1 頂点。**メートル単位**（ワールドの x, z を推奨）。
//               タイルは材質が 1 / tileMeters を掛けて決める
//   indices   … 3 個で 1 三角形（上から見て反時計回り）。空なら前回のまま
//               （addTerrain 直後は全部 0 なので、**最初の 1 回は必ず渡す**。
//               渡さないとそのチャンクは何も描かれない）
//
// 頂点数は (quads + 1)^2、インデックス数は quads^2 * 6 で**固定**
// （addTerrain の quadsPerChunk）。穴は面積 0 の三角形（同じ頂点を 3 回）で
// 表す - 実行中にプリミティブ数を変えない（ギズモの太線バッチと同じ方針）。
struct TerrainChunkMesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
};

// テレインの表面（setTerrainSurface に渡す）。層は塗り分けの重み（スプラット、
// setTerrainSplat）の RGBA の順で、各層が画像 3 枚（色・法線・マスク。3 枚とも
// 同じタイル）とそのタイルの幅・粗さ・金属・法線の強さを持つ。色はテレイン全体で
// 1 つ（全層に掛かる）。層の値は解決済み（「テレイン全体に従う」は呼び手が
// 全体の値に置き換えてから渡す。Renderer は従うかどうかを知らない）。
//   texture     … 色の画像（assets/ 相対。空 = 白 1 色 = color がそのまま色、sRGB）
//   normal      … 法線マップ（assets/ 相対。空 = 平ら。リニア、緑が画像の上）
//   mask        … マスク（assets/ 相対。R = 遮蔽・G = 粗さ・B = 金属、リニア。
//                 空 = 無し = roughness / metallic の数値を使う。あれば数値は使わない）
//   tileMeters  … 画像 1 枚の幅 (m)
//   roughness / metallic … 0〜1（マスクの無い層で効く）
//   normalScale … 法線マップの強さ（1 = 原寸、0 = 平ら）
// 数値（tileMeters・roughness・metallic・normalScale）は呼ぶ側で検査済み
// （範囲の規則は Scene が持つ）。Renderer はそのまま材質へ渡す。
//   color       … リニア rgba（a は使わない = 不透明）
struct TerrainSurface {
    // 層の数。terrain::kSplatChannels と同じ値に保つこと（Renderer は src/terrain
    // の型を公開 API に出さないので、両方を見る Scene が static_assert で照合する）。
    static constexpr int kLayers = 4;
    struct Layer {
        std::string texture;
        std::string normal;
        std::string mask;
        float tileMeters = 2.0f;
        float roughness = 0.9f;
        float metallic = 0.0f;
        float normalScale = 1.0f;
    };
    Layer layers[kLayers];
    filament::math::float4 color{1.0f, 1.0f, 1.0f, 1.0f};
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
    // 材質（PBR）。色と同じくスロットごとのマテリアルインスタンスに入る。
    // 掴んでハイライトしている間は形状のインスタンスが差し替わるので見えず、
    // 離すと戻る（色と同じ振る舞い）。RENDER スレッド。
    void setShapeMaterial(std::size_t id, const ShapeMaterial& material);

    // ---- ソフトボディ（毎フレーム頂点を書き換える三角メッシュ）----------
    // 形状スロットの 1 つとして作るので、色・ハイライト・削除は addShape と
    // 同じ口（setShapeColor / setBoxHighlighted / removeShape）。頂点は
    // **ワールド座標**で渡す（姿勢行列は使わない - 変形する物に 1 個の
    // 姿勢は無い）。indices は 3 個で 1 三角形、外から見て反時計回り。
    // 位置と法線の配列は 3 個で 1 頂点・vertexCount ぶん。バウンディング
    // ボックスは頂点から毎回作り直す（影とカリングのため）。RENDER スレッド。
    std::size_t addSoftShape(std::size_t vertexCount,
                             const std::vector<uint32_t>& indices);
    void setSoftShapeVertices(std::size_t id, const float* positions,
                              const float* normals, std::size_t vertexCount);
    // ---- Lights ----------------------------------------------------------
    // ライトの設計値はシーン側（Scene のライト一覧、保存文書に入る）。ここが
    // 持つのは Filament のエンティティだけ。addLight / removeLight / updateLight
    // とも RENDER スレッドから呼ぶ（build 中の初期分は単一スレッドなので同じ）。
    // 種類・影・減衰・円錐角は生成時に決まる - 変えるときは作り直す
    // （removeLight + addLight。Scene::syncLights の rebuild がそれ）。
    // removeLight で空いた番号は次の addLight が再利用する（形状スロットと
    // 同じ流儀。番号を詰めると Scene の対応付けがずれるため）。
    std::size_t addLight(const LightDesc& desc);
    void removeLight(std::size_t index);
    void updateLight(std::size_t index, const filament::math::float3& color,
                     float intensity, const filament::math::float3& direction,
                     const filament::math::float3& position);
    // One highlight style per camera, configured once by the scene, so each
    // viewer's selection has its own colour and several can be lit at once.
    void configureHighlightColors(
        const std::vector<filament::math::float3>& colors);
    // Draw a box with the given camera's highlight colour; -1 restores it.
    void setBoxHighlighted(std::size_t id, int styleIndex);
    // ---- 描画設定（フォトリアル）------------------------------------------
    // 全ビューへ一度に効く。ColorGrading は指定が変わったときだけ作り直す
    // （LUT を焼くので毎フレーム呼んでよい作りではない）。影のテクスチャ
    // サイズなどライト側の設定も、既にあるライトへ流し込む。RENDER スレッド。
    void setRenderSettings(const RenderSettings& settings);
    const RenderSettings& renderSettings() const { return settings_; }

    // Vertical field of view in degrees - the picking code needs it to build
    // a ray through a screen position. Same value as kFovDegrees.
    float verticalFovDegrees() const;
    double aspect() const { return double(width_) / double(height_); }
    // tileMeters: how many metres one full repeat of the texture covers.
    // texturePath: image file (PNG etc) for the ground; if empty or missing,
    // a checkerboard is generated instead. 2 回目以降の呼び出しは前の地面を
    // 壊して作り直す（シーン文書の <ground> の実行時反映）。RENDER スレッド。
    void addGround(float halfSize, const filament::math::float3& color,
                   float tileMeters, const std::string& texturePath,
                   const ShapeMaterial& material = ShapeMaterial{0.9f});
    // 見える地面を外す（地面を削除したシーン = <ground enabled="false">）。
    // 無ければ何もしない。次の addGround で作り直せる。RENDER スレッド。
    void removeGround();
    // 地面の置き場所（位置 + 回転。地面のメッシュはローカルの y = 0 の面）。
    // 値を覚えていて、後から addGround で作り直した地面にも当てる。RENDER スレッド。
    void setGroundTransform(const filament::math::mat4f& transform);

    // ---- テレイン（チャンク分割の高さメッシュ）---------------------------
    // シーンに 1 つ。chunksX × chunksZ 枚のチャンクを作り、各チャンクは
    // quadsPerChunk 四方のクアッド（頂点・インデックス数は固定、
    // TerrainChunkMesh を参照）。2 回目以降の呼び出しは前のテレインを壊して
    // 作り直す。作った直後のチャンクは何も描かない（全インデックス 0）ので、
    // 続けて setTerrainChunk で中身を入れる（最初の 1 回は indices も渡す
    // こと - 空だと「前回のまま」= 全部 0 で描かれない）。影を落とし受け、全ビューに映る
    // 通常レイヤ。材質は setTerrainSurface（呼ぶまでは白・艶消し）。
    // 失敗（引数が不正）は false。すべて RENDER スレッド。
    bool addTerrain(int chunksX, int chunksZ, int quadsPerChunk);
    // addTerrain の引数の上限。quadsPerChunk は頂点バッファ 1 本が大きく
    // なりすぎない程度（256 で 66049 頂点、想定は 64）、チャンク数（1 辺）は
    // 誤った引数で数万個のレンダラブルを作らないための安全弁。
    // terrain::kMaxChunkQuads（src/terrain/HeightField.h）と同じ値に保つこと
    // （Renderer は src/terrain を知らないので、両方を見る Scene が static_assert
    // で照合する）。
    static constexpr int kMaxTerrainQuads = 256;
    static constexpr int kMaxTerrainChunks = 64;
    // チャンク (cx, cz) の頂点（と、空でなければインデックス）を送り直し、
    // バウンディングボックスも頂点から作り直す。配列の長さが合わなければ
    // 警告して何もしない（false）。
    bool setTerrainChunk(int cx, int cz, const TerrainChunkMesh& mesh);
    // 表面の材質（層の画像 3 枚とタイル・粗さ・金属・法線の強さ、色）。層の画像は
    // 前回とパスが違う画像だけ読み直す（読めなければ警告して既定の単色 = 色は白・
    // 法線は平ら・マスクは無し = 数値を使う）。どの画像も kTerrainLayerTexSize
    // 四方へ揃えて（縦横比が違えば引き伸ばす）その種類のテクスチャ配列の 1 枚に
    // 入れる。色・タイル・数値だけの変更は画像を読まない。
    void setTerrainSurface(const TerrainSurface& surface);
    // 層のテクスチャ配列の 1 辺（全層この大きさに揃える。ミップ込みで 1 層
    // 約 5.6 MB、配列は色・法線・マスクの 3 本）。
    static constexpr int kTerrainLayerTexSize = 1024;
    // 塗り分けの重み（スプラット: resolution × resolution のテクセル、1 テクセル
    // RGBA8 = 層 0〜3 の重み。テクセル (i, j) がセル (i, j) に対応）の矩形
    // (x0, z0, w, h) を rgba（行優先、1 行 w × 4 バイト、行 = +Z）で書き換える。
    // sizeMeters はテレインの全幅（UV のメートル → テクセルの換算）。今の
    // テクスチャと解像度が違う・未作成なら、既定のテクセル（層 0 だけ）で
    // 作り直してから矩形を書く（矩形が全体なら既定で埋めずにそのまま入れる）。
    // 矩形が範囲外・空なら警告して何もしない（false）。
    bool setTerrainSplat(int resolution, float sizeMeters, int x0, int z0, int w,
                         int h, const uint8_t* rgba);
    // テレインを外す（無ければ何もしない）。材質の設定は覚えたまま。
    void removeTerrain();
    // テレインの置き場所（格子のローカル → ワールド。チャンクの頂点は原点
    // 基準のまま）。値を覚えていて、後から addTerrain で作ったチャンクにも
    // 当てる。動かす・回すのにメッシュを作り直さない。RENDER スレッド。
    void setTerrainTransform(const filament::math::mat4f& transform);

    // ---- glTF モデル（シーン文書の <asset><mesh/> に対応）----------------
    // loadModel が原型（同じパスは 1 回だけ読む）、addModelInstance が実体。
    // 実体の番号は組み込み形状（addShape）とは別の空間で、姿勢は
    // setModelInstanceTransform で入れる。gltfio は実体を 1 個だけ壊せない
    // ので、releaseModelInstance はスケール 0 で隠して同じモデルの次の実体に
    // 番号を再利用させる。すべて RENDER スレッド。
    static constexpr std::size_t kInvalidModel = static_cast<std::size_t>(-1);
    // THROWS AssetError（見つからない・壊れている・glTF 非対応ビルド）。
    std::size_t loadModel(const std::string& path);
    // モデルのバウンディングボックスの最長辺（モデル自身の単位、0 = 無い）。
    // <mesh scale> を決める目安として起動ログに出す。
    float modelSize(std::size_t modelId) const;
    std::size_t addModelInstance(std::size_t modelId);
    void releaseModelInstance(std::size_t instanceId);
    void setModelInstanceTransform(std::size_t index,
                                   const filament::math::mat4f& transform);

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
    // 環境マップを背景としても出すか（シーン文書の <environment skybox>）。
    // 光の設定ではないので HDR を読み直さずに切り替えられる。環境マップが
    // 無いとき（一様アンビエント）は無地の背景のまま。RENDER スレッド。
    void setSkyboxEnabled(bool enabled);
    // 環境マップを外して、起動時と同じ一様な弱いアンビエントへ戻す
    // （シーン文書の <environment hdr=""> に対応）。RENDER スレッド。
    void clearEnvironment();

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

    // ---- 細線セット（グリッド用の軽量パス）-------------------------------
    // 1 ピクセルの LINES をひとつのレンダラブルに詰める。太線バッチは 1 本を
    // 板 2 枚（8 頂点・4 三角形）に展開するが、こちらは 1 本 2 頂点で塗りも
    // 無い。グリッドのように本数が多く「見えれば十分」な作業目安はこちら。
    // エディタ専用レイヤに乗るので、映るビューは太線バッチと同じ扱い。
    //
    // addLineSet は使う側が 1 回だけ呼ぶ（グリッドは起動時の Scene::build、
    // 筆の輪は TerrainComponent の最初の onRender）。setLineSet は中身が変わったとき
    // だけ呼ぶ約束（グリッドなら間隔の変更時のみ）。本数が変わったら
    // レンダラブルごと作り直す - 実行中にプリミティブ数を変える API は版に
    // よって名前が違うので使わない。RENDER スレッド。
    std::size_t addLineSet(const filament::math::float3& color);
    // points は 2 個で 1 本。空にすると非表示。
    void setLineSet(std::size_t id,
                    const std::vector<filament::math::float3>& points);

    // ---- オーバーレイ（エディタ専用レイヤの半透明メッシュ）----------------
    // 頂点色つき・unlit の三角形メッシュ。筆のカーソルの円盤用。映るビューは
    // 細線セットと同じ（エディタ専用レイヤ）。setOverlayMesh は中身が
    // 変わったときだけ呼ぶ約束（細線セットと同じ。Renderer は同一判定を
    // しない）。頂点数・インデックス数が変わったらレンダラブルごと作り直す。
    // RENDER スレッド。
    std::size_t addOverlayMesh();
    // colors は rgba（リニア、0〜1、乗算前の生の値）で positions と同じ数。
    // indices は 3 個で 1 三角形。positions か indices が空なら非表示。
    // 次の場合も非表示にして警告を 1 回だけ出す: 色と頂点の数が違う・
    // 頂点が 65536 個を超える（16 ビットのインデックスで指せない）・
    // indices の数が 3 の倍数でない・頂点数以上のインデックスがある。
    void setOverlayMesh(std::size_t id,
                        const std::vector<filament::math::float3>& positions,
                        const std::vector<filament::math::float4>& colors,
                        const std::vector<uint16_t>& indices);

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
    // 全ビューのカメラの投影（垂直画角・near・far）。型は Camera::setProjection
    // の引数（double）に合わせる。アスペクト比だけがビューの大きさで決まる。
    static constexpr double kFovDegrees = 45.0;
    static constexpr double kNearPlane = 0.1;
    static constexpr double kFarPlane = 2000.0;
    // 形状の既定色（リニア）。document/EditorTypes.h の Color3 の既定と同じ値
    // （層が違うので別定義。片方を変えたらもう片方も合わせる）。値は
    // Renderer.cpp に置く - constexpr で書くと MSVC は通るが IntelliSense が
    // float3 のコンストラクタを定数と見なせず E0028 を出すため。
    static const filament::math::float3 kDefaultShapeColor;

    // 起動時の一様アンビエントを（作り直して）張る。clearEnvironment の実体。
    void installFlatAmbient();
    // 描画設定の適用。ビュー 1 つぶん（addView と setRenderSettings が呼ぶ）、
    // 色作り（トーンマップ + グレーディング）の作り直し、ライトの影設定。
    // ShadowOptions は Filament の入れ子型なので、ここ（前方宣言しか無い
    // ヘッダ）には出さない - 組み立ては RendererInternal.h の
    // shadowOptionsFrom が持つ。
    struct ViewSlot;   // 下で定義（宣言だけ先に要る）
    struct ShapeSlot;  // 同上
    void applyViewSettings(ViewSlot& slot);
    // そのビューのカメラへ投影を入れる（kFovDegrees / kNearPlane / kFarPlane、
    // アスペクト比は slot.width / slot.height）。addView とリサイズが呼ぶ。
    void applyProjection(ViewSlot& slot);
    void rebuildColorGrading();
    void applyShadowSettings();
    // マテリアルインスタンスへ材質を入れる（共有インスタンス・ハイライト
    // 用インスタンス・スロットごとのインスタンスで同じ既定値を使うため）。
    void applyMaterialParams(filament::MaterialInstance* mi,
                             const ShapeMaterial& material) const;
    // そのスロット専用のマテリアルインスタンス（無ければ作って色と材質を
    // 入れる）。共有インスタンスを書き換えると全部の形状の色が変わるため。
    filament::MaterialInstance* ensureShapeInstance(ShapeSlot& slot);
    // いまの環境マップと設定からスカイボックスを作り直す（無効なら外す）。
    void refreshSkybox();

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
    // 円柱（タイヤ）。球と同じく最初に使うときに作る。
    filament::VertexBuffer* cylinderVb_ = nullptr;
    filament::IndexBuffer* cylinderIb_ = nullptr;
    uint32_t cylinderIndexCount_ = 0;
    void ensureCylinderMesh();
    uint32_t sphereIndexCount_ = 0;
    void ensureSphereMesh();
    // 箱の共有立方体メッシュ（vb_ / ib_）。起動時に 1 回。全部の箱が使うので
    // 遅延しない。
    void buildCubeMesh();

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
        // スロットが自分のインスタンスを持つとき、その中身。色と材質は
        // 別々のタイミングで来る（色はイベントでも変わる）ので、
        // インスタンスを作り直すときに両方を入れ直せるよう覚えておく。
        filament::math::float3 color = kDefaultShapeColor;
        ShapeMaterial material;
        // ソフトボディだけが持つ自前のバッファ（組み込み形状は共有メッシュ
        // なので null）。removeShape が一緒に壊す。
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
        uint32_t vertexCount = 0;
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

    // 細線セット（グリッド用）。バッファは今の本数ぶんだけ確保し、本数が
    // 変わったら作り直す（変わるのは設定変更のときだけ）。
    struct LineSet {
        utils::Entity entity;
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
        filament::MaterialInstance* mi = nullptr;
        std::size_t lineCount = 0;  // 確保済みの本数（0 = 実体なし）
        bool inScene = false;
    };
    std::vector<LineSet> lineSets_;

    // オーバーレイのメッシュ。細線セットと同じく、バッファは今の数ぶんだけ
    // 確保し、数が変わったら作り直す。
    struct OverlayMesh {
        utils::Entity entity;
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
        filament::MaterialInstance* mi = nullptr;
        std::size_t vertexCount = 0;  // 確保済みの頂点数（0 = 実体なし）
        std::size_t indexCount = 0;
        bool inScene = false;
    };
    std::vector<OverlayMesh> overlayMeshes_;
    filament::Material* overlayMaterial_ = nullptr;
    bool overlayWarned_ = false;  // 形の壊れたメッシュの警告を出したか（1 回だけ）
    // overlay.filamat の読み込み（コンストラクタが 1 回だけ呼ぶ）。無い・空・壊れている場合は
    // AssetError を投げる（loadLineMaterial と同じ扱い）。
    void loadOverlayMaterial();

    filament::Material* lineMaterial_ = nullptr;
    // line.filamat の読み込み（コンストラクタが 1 回だけ呼ぶ）。無い・空・壊れている場合は
    // AssetError を投げる（線が黙って消えるのではなく、原因を示して止める）。
    void loadLineMaterial();
    LineEntity createLine(const filament::math::float3& color);
    void updateLine(LineEntity& line, const filament::math::float3& from,
                    const filament::math::float3& to, bool visible);
    void destroyLine(LineEntity& line);
    filament::Material* groundMaterial_ = nullptr;
    filament::MaterialInstance* groundMatInstance_ = nullptr;
    filament::IndirectLight* ibl_ = nullptr;
    filament::Texture* iblTexture_ = nullptr;  // reflections, when an IBL is loaded
    // 環境マップを背景としても出すとき（<environment skybox="true">）の
    // 実体。iblTexture_ を共有するので、環境を張り替えるたびに作り直す。
    filament::Skybox* skybox_ = nullptr;
    bool skyboxWanted_ = false;
    float envIntensity_ = 30000.0f;  // 背景の明るさを光と揃えるため覚える

    // いまの描画設定（シーン文書の <visual>）と、そこから焼いた色作り。
    RenderSettings settings_;
    filament::ColorGrading* colorGrading_ = nullptr;
    std::unique_ptr<GltfLoader> gltf_;
    filament::Texture* groundTexture_ = nullptr;

    utils::Entity groundEntity_;
    std::vector<utils::Entity> lightEntities_;  // one per addLight(), in order

    // テレイン（RendererTerrain.cpp）。チャンクごとに自前の頂点 /
    // インデックスバッファとエンティティを持ち、マテリアルインスタンスは
    // 全チャンクで 1 個を共有する（層ごとの画像・粗さ・金属・法線の強さは
    // テクスチャ配列と float4 のパラメータで持つので、インスタンスは 1 個で足りる）。
    // 表面（層のテクスチャ配列・スプラット・材質）は RendererTerrainSurface.cpp。
    struct TerrainChunk {
        utils::Entity entity;
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
    };
    std::vector<TerrainChunk> terrainChunks_;
    filament::math::mat4f terrainTransform_;  // 単位で始まる（setTerrainTransform）
    filament::math::mat4f groundTransform_;   // 同じく地面（setGroundTransform）
    int terrainChunksX_ = 0;
    int terrainChunksZ_ = 0;
    int terrainQuads_ = 0;  // 1 チャンクの 1 辺のクアッド数
    filament::Material* terrainMaterial_ = nullptr;
    filament::MaterialInstance* terrainMatInstance_ = nullptr;
    // 層のテクスチャ配列（どれも kTerrainLayerTexSize 四方 × kLayers 層）。色は
    // SRGB8_A8、法線とマスクは RGBA8（= リニア。sRGB で読むと法線が曲がる）。
    // 作るのはコンストラクタの 1 回だけ（全層を既定の単色で埋める）で、以後は
    // 層ごとに中身を入れ替える。
    filament::Texture* terrainLayerTextures_ = nullptr;
    filament::Texture* terrainNormalTextures_ = nullptr;
    filament::Texture* terrainMaskTextures_ = nullptr;
    // 層に今入っている画像のパス（同じなら読み直さない。初期の単色は空文字列と
    // 同じ = 「画像無し」）。hasNormal = 法線マップがファイルから読めたか（無ければ
    // 平ら）。hasMask = マスクがファイルから読めたか（無ければ粗さ・金属は数値、
    // 遮蔽は無し）。
    struct TerrainLayerImages {
        std::string texture;
        std::string normal;
        std::string mask;
        bool hasNormal = false;
        bool hasMask = false;
    };
    TerrainLayerImages terrainLayerImages_[TerrainSurface::kLayers];
    // 塗り分けの重み（resolution 四方、リニア RGBA8、ミップ無し）。コンストラクタで
    // 1×1 の既定（層 0 だけ）を作り、setTerrainSplat が解像度に合わせて作り直す。
    filament::Texture* terrainSplat_ = nullptr;
    int terrainSplatRes_ = 0;
    // コンストラクタから: 層の配列 3 本（既定の単色）と 1×1 の既定のスプラットを作って
    // マテリアルへ付ける（RendererTerrainSurface.cpp）。
    void createTerrainTextures();
    // 解像度 resolution の既定のスプラットを作って付け替える（fill = 既定の
    // テクセルで埋める。直後に全体を書くなら省く）。
    void recreateTerrainSplat(int resolution, bool fill);

    // Kept alive for the vertex buffers (Filament references, not copies).
    filament::math::quatf cubeTangents_[24];
    filament::math::quatf groundTangents_[4];
    filament::math::float3 groundPos_[4];
    filament::math::float2 groundUv_[4];

};

}  // namespace wizengine
