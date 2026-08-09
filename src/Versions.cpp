#include "Versions.h"

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

const char* engineVersion() { return "1.0"; }

nlohmann::json versionsJson() {
    guint maj = 0, min = 0, mic = 0, nano = 0;
    gst_version(&maj, &min, &mic, &nano);  // runtime, not the build headers

    nlohmann::json j;
    j["WizEngine"] = engineVersion();
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
