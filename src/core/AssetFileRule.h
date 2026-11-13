#pragma once

#include <string>

namespace wizengine {

// 文書に書いてよいファイル参照（<mesh file> / <ground texture> /
// <environment hdr> / UI の画像など）は assets/ からの相対パスだけ。".." と
// 絶対パス（/ 始まり・ドライブレター）を弾いて、読み込み先を assets/ の下に
// 閉じ込める（保存名の正規化と同じ動機。文書もブラウザの入力欄も
// 手で書けるので、XML の読み込みと edit.* の両方がこれを通す）。
inline bool assetFileAllowed(const std::string& file) {
    if (file.empty()) return false;
    if (file[0] == '/' || file[0] == '\\') return false;
    if (file.size() > 1 && file[1] == ':') return false;  // C:\ ...
    if (file.find("..") != std::string::npos) return false;
    return true;
}

}  // namespace wizengine
