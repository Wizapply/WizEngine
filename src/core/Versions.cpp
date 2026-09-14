#include "core/Versions.h"

#include <string>

#include <Eigen/Core>
#include <chrono/ChVersion.h>
#include <gst/gst.h>
#include <httplib.h>

// Pinned in CMakeLists.txt (wiz_download_header URLs / FILAMENT_VERSION);
// passed through as compile definitions so this file cannot drift from the
// build's reality. Fallbacks only cover building outside CMake.
#ifndef WIZ_FILAMENT_VERSION
#define WIZ_FILAMENT_VERSION "unknown"
#endif
#ifndef WIZ_CGLTF_VERSION
#define WIZ_CGLTF_VERSION "unknown"
#endif

namespace wizengine {

// 数字 3 つから文字列を組む（手で "1.0.0" と書くと kVersion* と食い違う）。
const char* engineVersion() {
    static const std::string text = std::to_string(kVersionMajor) + "." +
                                    std::to_string(kVersionMinor) + "." +
                                    std::to_string(kVersionPatch);
    return text.c_str();
}

const char* engineCodename() { return kCodename; }

const char* productName() { return kProductName; }

nlohmann::json versionsJson() {
    guint maj = 0, min = 0, mic = 0, nano = 0;
    gst_version(&maj, &min, &mic, &nano);  // runtime, not the build headers

    nlohmann::json j;
    j[productName()] = engineVersion();
    // コードネームは別キーで（製品名のキーは素の semver のまま = 機械的に
    // 比べられる）。ブラウザは About の見出しへ合成し、一覧には並べない。
    // 無い版（1.x）はキーごと省く。
    if (kCodename[0] != '\0') j["Codename"] = engineCodename();
    j["Filament"] = WIZ_FILAMENT_VERSION;
    j["Project Chrono"] = CHRONO_VERSION;
    j["GStreamer"] = std::to_string(maj) + "." + std::to_string(min) + "." +
                     std::to_string(mic);
    j["Eigen"] = std::to_string(EIGEN_WORLD_VERSION) + "." +
                 std::to_string(EIGEN_MAJOR_VERSION) + "." +
                 std::to_string(EIGEN_MINOR_VERSION);
    j["cpp-httplib"] = CPPHTTPLIB_VERSION;
    j["nlohmann/json"] = std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + "." +
                         std::to_string(NLOHMANN_JSON_VERSION_MINOR) + "." +
                         std::to_string(NLOHMANN_JSON_VERSION_PATCH);
    j["cgltf"] = WIZ_CGLTF_VERSION;
    j["stb_image"] = "master";  // upstream keeps no version macro
    return j;
}

}  // namespace wizengine
