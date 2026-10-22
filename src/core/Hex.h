#pragma once

namespace wizengine {

// hex の 1 文字の値（'0'〜'9'、'a'〜'f'、'A'〜'F' → 0〜15）。それ以外は -1。
// 背番号（core/Guid.h）と色（editor::colorFromHex）が使う。
inline int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace wizengine
