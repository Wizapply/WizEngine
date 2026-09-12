#pragma once

#include <nlohmann/json.hpp>

namespace wizengine {

// ---- WizEngine 自身の版 -----------------------------------------------------
// **ここが唯一の定義**（CMake の project(VERSION) には持たせない - 2 か所に
// 書くと必ずずれる）。起動ログの 1 行目、/stats の versions.WizEngine、
// ブラウザのサイドバー見出しと About 節がここから出る。
//
// 番号は semver（major.minor.patch）。コードネームは major 版ごとの呼び名で、
// 1.x は "Charon"（冥王星の衛星。以後の major 版も外側の衛星・準惑星から）。
// 上げるときは下の 3 つの数字とコードネームだけを直す。
constexpr int kVersionMajor = 1;
constexpr int kVersionMinor = 0;
constexpr int kVersionPatch = 0;
constexpr const char* kCodename = "Charon";

// "1.0.0" - ログとブラウザの About 節に出る文字列。
const char* engineVersion();
// "Charon" - major 版のコードネーム。
const char* engineCodename();

// Every third-party library with its version, as a JSON object of
// name -> version string, served to the browser via /stats. Values come from
// each library's own version macro/API where one exists (Chrono, GStreamer,
// cpp-httplib, nlohmann/json, Eigen) and from build-time pins for the
// header-only downloads that have none (Filament prebuilt, cgltf, stb).
nlohmann::json versionsJson();

}  // namespace wizengine
