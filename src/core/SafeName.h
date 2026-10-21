#pragma once

#include <cstddef>
#include <string>

namespace wizengine {

// 正規化した名前の最大の長さ（文字数）。
constexpr std::size_t kSafeNameMaxLength = 64;

// 名前の正規化（英数字と _ - だけ・64 文字まで）。ファイル名にも文書内の
// 参照名にも使える文字だけを残す（".." や "/" を弾き、保存先のフォルダの
// 外へ出られないようにするのが目的）。空になったら不正な名前で、呼び出し側が
// 弾く。シーン名（EditorState::sanitizeSceneName）とアセット名
// （editor::sanitizeEventName）はどちらもこれを呼ぶ = 規則は 1 か所。
inline std::string sanitizeSafeName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) out.push_back(c);
        if (out.size() >= kSafeNameMaxLength) break;
    }
    return out;
}

}  // namespace wizengine
