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
//     未知の tag は読み飛ばす（後の段階で SPLT / TREE / GRAS を足す）。
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

// decode / readFile の結果。field は原点中心・baseY 0（上の注意）。
struct TerrainFileData {
    HeightField field;
    float height = 0.0f;  // 高さの上限 (m)
    TerrainGuid guid;
};

// 失敗の理由（reason）はすべて英語（コンソールのログへ流れるため）。

// 見出しの 48 バイトだけを読んで検査する（マジック・版・resolution の範囲・
// size / height が正の有限値）。ファイル版の reason は「パス: 理由」。
bool readHeader(const uint8_t* data, std::size_t size, TerrainFileHeader& out,
                std::string& reason);
bool readHeader(const std::filesystem::path& path, TerrainFileHeader& out,
                std::string& reason);

// 格子 → バイト列。field が空・height が正の有限値でなければ false。
// 高さは [0, height] の外の値を端に丸めて量子化して保存する（呼び手が範囲内に
// 保つ前提。範囲外の値は読み戻すと端の値になる）。
bool encode(const HeightField& field, float height, const TerrainGuid& guid,
            std::vector<uint8_t>& out, std::string& reason);
// バイト列 → 格子。壊れた入力は理由付きで false（落ちない・見出しの数だけを
// 信じて大きな確保をしない）。
bool decode(const uint8_t* data, std::size_t size, TerrainFileData& out,
            std::string& reason);

// ファイルへ書く。同じフォルダの一時ファイルに書いてから置き換えるので、
// 途中で失敗しても元のファイルは壊れない。親フォルダが無ければ作る。
bool writeFile(const std::filesystem::path& path, const HeightField& field,
               float height, const TerrainGuid& guid, std::string& reason);
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
