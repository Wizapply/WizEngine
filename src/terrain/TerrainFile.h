#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "terrain/HeightField.h"

// テレインの保存ファイル（assets/terrains/<名前>.terrain）とバイト列の往復、
// そしてテレインの背番号（TerrainGuid）。
//
// 1 ファイル / 1 バイト列だけを扱う（一覧・名前の解決・改名は
// TerrainLibrary の担当）。Chrono も Filament もシーン文書も知らない。
//
// 形式（リトルエンディアン）:
//   見出し（48 バイト）
//     char[4] "WZTR" / u32 version (1) / u32 resolution / f32 size /
//     f32 height（高さの上限 m）/ u8[16] guid / u32 sectionCount / u32[2] reserved
//     （4+4+4+4+4+16+4+8 = 48。節の先頭が 8 バイト境界になる）
//   節（sectionCount 個、順不同、各節の先頭は 8 バイト境界）
//     char[4] tag / u32 sectionVersion / u64 byteLength / payload（byteLength
//     バイト。次の節までは 0 で 8 バイト境界まで埋める）
//     HGHT（必須、版 1）: u16 × (res+1)²。値 = round(clamp(h / height, 0, 1) × 65535)。
//          並びは行 = +Z、行内 = +X（HeightField::samples() と同じ行優先）。
//          中身は RAW16 のハイトマップと同じ（量子化と変換は Heightmap.h）。
//     HOLE（版 1）: u8 × res²。0 = 穴、255 = 中身（メモリ上の HeightField とは
//          逆向き）。穴が 1 つも無ければ書かない。読むときは 0 だけを穴とみなす。
//     SPLT（版 1）: u8 × res² × kSplatChannels。セルごとの層の重み（合計 255。
//          読むときは検査しない - HeightField.h の注記）。並びは行 = +Z、行内 = +X。
//          既定でないセルが無ければ（HeightField::anySplat が false）書かない。
//          無ければ全セル層 0。
//     LAYR（版 1）: 層の定義。u32 count のあと、層ごとに u32 recordBytes /
//          u16 pathLen + UTF-8 のパス（色。assets/ 相対）/ f32 tile (m) /
//          u16 normalLen + 法線マップのパス / u16 maskLen + マスクのパス /
//          f32 roughness / f32 metallic / f32 normalScale。読むのは
//          先頭の min(count, kSplatChannels) 層で、残りの層とレコードの後ろの
//          知らない欄は recordBytes で飛ばす（後で欄を末尾へ足しても古い版が
//          読める）。tile でレコードが終わる古い形式は、法線から後ろを既定と
//          みなす（法線の欄からの途中で尽きたら壊れた節）。全部の層が既定なら
//          書かない。無ければ全層既定。
//     未知の tag は読み飛ばす（後の段階で TREE / GRAS を足す）。
//
// 位置（中心・baseY）はファイルに入れない（シーンの XML が持つ）。decode が
// 返す格子は**原点中心・baseY 0**で、呼び手が HeightField::withGeometry で
// 置き直す。
namespace wizengine {
namespace terrain {

// 背番号。作成時に 1 回だけ採番し、名前の変更・保存では変えない。全部 0 = 空
// （背番号を持たない）。
struct TerrainGuid {
    std::array<uint8_t, 16> bytes{};

    bool empty() const;
    bool operator==(const TerrainGuid& o) const { return bytes == o.bytes; }
    bool operator!=(const TerrainGuid& o) const { return bytes != o.bytes; }
};

// 32 桁の小文字 hex（XML と TerrainDesc の表記）。
std::string guidToHex(const TerrainGuid& guid);
// 32 桁の hex（大文字も可）から。長さ違い・hex 以外の文字は false で out は
// 触らない。
bool guidFromHex(const std::string& hex, TerrainGuid& out);
// 乱数の背番号（空にはならない）。std::random_device が使えなければ時刻と
// アドレスを混ぜて作る。
TerrainGuid makeRandomGuid();

constexpr uint32_t kTerrainFileVersion = 1;
constexpr std::size_t kTerrainHeaderBytes = 48;

// 見出しの中身（一覧の走査用。格子は読まない）。
struct TerrainFileHeader {
    uint32_t version = 0;
    int resolution = 0;
    float size = 0.0f;
    float height = 0.0f;
    TerrainGuid guid;
    uint32_t sectionCount = 0;
};

// 層の粗さ・金属の「テレイン全体の <terrain roughness metallic> に従う」を表す値。
// 判定は layerFollowsTerrain（負なら従う）。
constexpr float kLayerFollowsTerrain = -1.0f;
// 層の粗さ・金属の値が「テレイン全体の値に従う」を表しているか（負 = 従う）。
inline bool layerFollowsTerrain(float v) { return v < 0.0f; }

// 塗り分けの層 1 つの定義（.terrain の LAYR 節）。値の検査（パスの関所・
// tile の範囲）は呼び手（Scene）の担当で、ここは素の値を運ぶだけ。
// 色・法線・マスクの 3 枚は同じ tile で貼る。
struct TerrainLayer {
    std::string texture;  // 色の画像（assets/ 相対のパス）。空 = テクスチャ無し
    float tile = 2.0f;    // テクスチャ 1 枚の幅 (m)
    // 法線マップ（assets/ 相対。緑が画像の上を向く向き）。空 = 平ら
    std::string normal;
    // マスク（assets/ 相対。R = 遮蔽、G = 粗さ、B = 金属、A は使わない。
    // リニア）。あれば下の roughness / metallic は使わない（マスクが勝つ）
    std::string mask;
    // 粗さ・金属。負 = テレイン全体の <terrain roughness metallic> に従う
    // （layerFollowsTerrain）
    float roughness = kLayerFollowsTerrain;
    float metallic = kLayerFollowsTerrain;
    float normalScale = 1.0f;  // 法線マップの強さ（1 = 原寸、0 = 平ら）

    bool operator==(const TerrainLayer& o) const {
        return texture == o.texture && tile == o.tile && normal == o.normal &&
               mask == o.mask && roughness == o.roughness && metallic == o.metallic &&
               normalScale == o.normalScale;
    }
    bool operator!=(const TerrainLayer& o) const { return !(*this == o); }
};
// 層が持つ画像の欄（色・法線・マスク）の一覧。パスの検査（長さの上限・
// assets/ の関所）はこれを回す = 欄を足したらここに足せば全部の検査に効く。
enum class LayerImage { Color, Normal, Mask };
constexpr std::array<LayerImage, 3> kLayerImages = {LayerImage::Color, LayerImage::Normal,
                                                     LayerImage::Mask};
// 層の画像の欄のパス。
inline const std::string& layerImagePath(const TerrainLayer& l, LayerImage image) {
    switch (image) {
        case LayerImage::Normal: return l.normal;
        case LayerImage::Mask: return l.mask;
        case LayerImage::Color: break;
    }
    return l.texture;
}
inline std::string& layerImagePath(TerrainLayer& l, LayerImage image) {
    return const_cast<std::string&>(layerImagePath(static_cast<const TerrainLayer&>(l), image));
}
// 画像の欄の英語名（ログと reason 用）。
inline const char* layerImageName(LayerImage image) {
    switch (image) {
        case LayerImage::Normal: return "normal map";
        case LayerImage::Mask: return "mask";
        case LayerImage::Color: break;
    }
    return "texture";
}
using TerrainLayers = std::array<TerrainLayer, kSplatChannels>;
// LAYR のパスの長さの上限（バイト）。超える層は書かず、読みは壊れた節とみなす。
constexpr std::size_t kMaxLayerPathBytes = 1024;

// decode / readFile の結果。field は原点中心・baseY 0（上の注意）。
struct TerrainFileData {
    HeightField field;
    float height = 0.0f;  // 高さの上限 (m)
    TerrainGuid guid;
    TerrainLayers layers;  // LAYR が無ければ全層既定
};

// 失敗の理由（reason）はすべて英語（コンソールのログへ流れるため）。

// 見出しの 48 バイトだけを読んで検査する（マジック・版・resolution の範囲・
// size / height が正の有限値）。ファイル版の reason は「パス: 理由」。
bool readHeader(const uint8_t* data, std::size_t size, TerrainFileHeader& out,
                std::string& reason);
bool readHeader(const std::filesystem::path& path, TerrainFileHeader& out,
                std::string& reason);

// 格子 → バイト列。field が空・height が正の有限値でない・層のパス（色・
// 法線・マスクのどれか）が kMaxLayerPathBytes を超えるなら false。
// 高さは [0, height] の外の値を端に丸めて量子化して保存する（呼び手が範囲内に
// 保つ前提。範囲外の値は読み戻すと端の値になる）。layers は LAYR 節に書く
// （全層既定 = 書かない）。層を渡し忘れて黙って消さないよう、既定引数は持たない。
bool encode(const HeightField& field, float height, const TerrainGuid& guid,
            std::vector<uint8_t>& out, std::string& reason,
            const TerrainLayers& layers);
// バイト列 → 格子。壊れた入力は理由付きで false（落ちない・見出しの数だけを
// 信じて大きな確保をしない）。
bool decode(const uint8_t* data, std::size_t size, TerrainFileData& out,
            std::string& reason);

// ファイルへ書く。同じフォルダの一時ファイルに書いてから置き換えるので、
// 途中で失敗しても元のファイルは壊れない。親フォルダが無ければ作る。
// layers は encode と同じ（既定引数は持たない）。
bool writeFile(const std::filesystem::path& path, const HeightField& field,
               float height, const TerrainGuid& guid, std::string& reason,
               const TerrainLayers& layers);
bool readFile(const std::filesystem::path& path, TerrainFileData& out,
              std::string& reason);

// ファイルのバイト列の読み書き（.terrain・ハイトマップの RAW / PNG が共用）。
// readFileBytes は maxBytes を超えるファイルを読まずに断る（壊れた・別の
// 大きなファイルを丸ごと読まない安全弁）。writeFileBytes は一時ファイルに
// 書いてから置き換え（途中で失敗しても元のファイルは壊れない）、親フォルダが
// 無ければ作る。reason にはパスが付く。
bool readFileBytes(const std::filesystem::path& path, uintmax_t maxBytes,
                   std::vector<uint8_t>& out, std::string& reason);
bool writeFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
                    std::string& reason);

}  // namespace terrain
}  // namespace wizengine
