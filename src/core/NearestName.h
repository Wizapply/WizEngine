#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace wizengine {

// 大文字小文字を無視した編集距離（Levenshtein）。
inline std::size_t editDistanceIcase(const std::string& a, const std::string& b) {
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// names[0..count) のうち name に近いもの（大文字小文字を無視して一致、無ければ
// 編集距離 2 以内で最も近いもの。3 文字以下の名前は 1 以内）。無ければ nullptr。
// 文書の読み込みの「もしかして」（性質・属性・種類の名前）に使う。
inline const char* nearestName(const std::string& name, const char* const* names,
                               std::size_t count) {
    if (name.empty()) return nullptr;
    // 短い名前ほど打ち間違いの許し幅を狭める（"pos" に 2 文字違いを許すと
    // 無関係な名前まで候補になる）。
    const std::size_t maxDistance = name.size() <= 3 ? 1 : 2;
    const char* best = nullptr;
    std::size_t bestDistance = maxDistance + 1;
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t d = editDistanceIcase(name, names[k]);
        if (d < bestDistance) {
            best = names[k];
            bestDistance = d;
        }
    }
    return best;
}

}  // namespace wizengine
