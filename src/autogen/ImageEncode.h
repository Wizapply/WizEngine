#pragma once

#include <cstdint>
#include <string>
#include <vector>

// LLM へ送る画像（AI の見直し用のスクリーンショット）を作る小物。Chrono も
// Filament も知らない（RGBA の画素を受け取るだけ）。
namespace wizengine {
namespace autogen {

// RGBA8（上の行から、1 行 width * 4 バイト）を JPEG にする。アルファは捨てる。
// quality は 1..100。失敗なら空。
std::vector<uint8_t> encodeJpeg(const uint8_t* rgba, int width, int height, int quality);

// バイト列 → base64（改行なし）。
std::string base64(const std::vector<uint8_t>& bytes);

// "data:image/jpeg;base64,..."（OpenAI の image_url にそのまま渡せる形）。
std::string jpegDataUrl(const std::vector<uint8_t>& jpeg);

}  // namespace autogen
}  // namespace wizengine
