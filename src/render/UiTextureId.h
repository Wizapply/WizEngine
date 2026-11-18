#pragma once

// UI のテクスチャ番号（Renderer の addUiTextureR8 / loadUiImage が返す int）の作り方。
// 番号 = 世代 × kUiTextureSlots + 枠の番号。枠は返した後に再利用されるが、世代が
// 進むので、返した番号を持ったままのメッシュが別のテクスチャを指すことはない
// （Renderer は世代の合わない番号を「無いテクスチャ」= 白として扱う）。
// 世代は 1 から（番号は 0 以上 = ui::kUiNoTexture の -1 と重ならない）。純粋な計算
// （Filament を知らない。tests/ui が検査する）。
namespace render_detail {

inline constexpr int kUiTextureSlots = 1 << 16;        // 同時に持てるテクスチャの数
inline constexpr int kUiTextureGenerations = 1 << 14;  // 世代は 1..この値-1 を回る（int に収まる）

inline int uiTextureId(int slot, int generation) { return generation * kUiTextureSlots + slot; }
inline int uiTextureSlot(int id) { return id % kUiTextureSlots; }
inline int uiTextureGeneration(int id) { return id / kUiTextureSlots; }
// 枠を使い直すときの次の世代（0 は使わない = 一度も使っていない枠の印）。同じ枠を
// kUiTextureGenerations − 1 回使い直すと一周するが、テクスチャを返すときにその番号を
// 使うメッシュは Renderer が壊すので、一周した番号を持つメッシュは残らない。
inline int nextUiTextureGeneration(int generation) {
    return generation + 1 < kUiTextureGenerations ? generation + 1 : 1;
}

}  // namespace render_detail
