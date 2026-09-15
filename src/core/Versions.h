#pragma once

#include <nlohmann/json.hpp>

namespace wizengine {

// ---- Charon 自身の版 -------------------------------------------------------
// **ここが唯一の定義**（CMake の project(VERSION) には持たせない - 2 か所に
// 書くと必ずずれる）。起動ログの 1 行目、/stats の versions.Charon、
// ブラウザのサイドバー見出しと About 節がここから出る。
//
// 製品名は "Charon"（冥王星の衛星）。番号は semver（major.minor.patch）。
// コードネームは major 版ごとの呼び名で、1.x は "WizEngine"（開発時の名前。
// 名前空間・実行ファイル・シーン文書のルート要素 <wizengine> もこれ）。
// 上げるときは下の 3 つの数字とコードネームだけを直す。空なら表示しない。
constexpr const char* kProductName = "Charon";
constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;
constexpr int kVersionPatch = 0;
constexpr const char* kCodename = "WizEngine";

// "Charon" - 製品名。ログ・/stats のキー・ブラウザの見出しに出る。
const char* productName();
// "1.0.0" - ログとブラウザの About 節に出る文字列。
const char* engineVersion();
// major 版のコードネーム（無ければ空文字列）。
const char* engineCodename();

// Every third-party library with its version, as a JSON object of
// name -> version string, served to the browser via /stats. Values come from
// each library's own version macro/API where one exists (Chrono, GStreamer,
// cpp-httplib, nlohmann/json, Eigen) and from build-time pins for the
// header-only downloads that have none (Filament prebuilt, cgltf, stb).
nlohmann::json versionsJson();

}  // namespace wizengine
