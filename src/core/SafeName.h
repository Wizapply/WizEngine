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

// 正規化済みの名前か（空でなく、sanitizeSafeName で変わらない）。ファイル名に
// する前の関所（テレイン名の検査と TerrainLibrary のパスの組み立て）が使う。
inline bool isSafeName(const std::string& name) {
    return !name.empty() && sanitizeSafeName(name) == name;
}

// 採番の接尾辞の区切り（base_2 の "_"）。firstFreeName が付け、stripNumberSuffix が外す。
constexpr char kNumberSuffixSeparator = '_';

// 空いている名前の採番の規則（唯一の定義）: base が空いていれば base、
// ふさがっていれば base_2, base_3 … を順に taken(name) に聞いて最初の空き。
// 候補が maxLength を超えるときは接尾辞が入るよう base 側を削る。上限
// （_999）まで探しても空かなければ空文字列。TerrainLibrary::uniqueName と
// core/Trash（.trash/ の名前の衝突）が使う。
template <class Taken>
std::string firstFreeName(const std::string& base, std::size_t maxLength, Taken taken) {
    constexpr int kMaxSuffix = 999;
    const std::string first = base.substr(0, maxLength);
    if (!taken(first)) return first;
    for (int n = 2; n <= kMaxSuffix; ++n) {
        const std::string suffix = kNumberSuffixSeparator + std::to_string(n);
        if (suffix.size() > maxLength) break;
        const std::string candidate = base.substr(0, maxLength - suffix.size()) + suffix;
        if (!taken(candidate)) return candidate;
    }
    return {};
}

// firstFreeName が付ける形の接尾辞（末尾の区切り + 1 文字以上の数字だけ）を外した
// 名前。その形でなければ name のまま（"a_"・"_2"・"a_2b" は外さない）。外して
// よいか（元の名前が実在するか）は呼び手が決める。
inline std::string stripNumberSuffix(const std::string& name) {
    const std::size_t sep = name.rfind(kNumberSuffixSeparator);
    if (sep == std::string::npos || sep == 0 || sep + 1 == name.size()) return name;
    for (std::size_t k = sep + 1; k < name.size(); ++k)
        if (name[k] < '0' || name[k] > '9') return name;
    return name.substr(0, sep);
}

}  // namespace wizengine
