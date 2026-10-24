#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

// src/terrain/ の中だけで使う、整数のバイト順の読み書き（.terrain の見出しと
// 節・RAW16 はリトルエンディアン、PNG と zlib の数値はビッグエンディアン）。
// T は符号なし整数（uint16_t / uint32_t / uint64_t）。
namespace wizengine {
namespace terrain {
namespace byteorder {

template <typename T>
void putLE(std::vector<uint8_t>& out, T v) {
    static_assert(std::is_unsigned<T>::value, "unsigned integers only");
    for (std::size_t k = 0; k < sizeof(T); ++k) out.push_back(uint8_t(v >> (8 * k)));
}

template <typename T>
void putBE(std::vector<uint8_t>& out, T v) {
    static_assert(std::is_unsigned<T>::value, "unsigned integers only");
    for (std::size_t k = sizeof(T); k-- > 0;) out.push_back(uint8_t(v >> (8 * k)));
}

template <typename T>
T getLE(const uint8_t* p) {
    static_assert(std::is_unsigned<T>::value, "unsigned integers only");
    T v = 0;
    for (std::size_t k = 0; k < sizeof(T); ++k) v = T(v | (T(p[k]) << (8 * k)));
    return v;
}

}  // namespace byteorder
}  // namespace terrain
}  // namespace wizengine
