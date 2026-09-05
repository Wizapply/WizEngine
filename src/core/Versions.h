#pragma once

#include <nlohmann/json.hpp>

namespace wizengine {

// WizEngine's own version, shown in logs and the browser's About section.
const char* engineVersion();

// Every third-party library with its version, as a JSON object of
// name -> version string, served to the browser via /stats. Values come from
// each library's own version macro/API where one exists (Chrono, GStreamer,
// cpp-httplib, nlohmann/json, Eigen) and from build-time pins for the
// header-only downloads that have none (Filament prebuilt, cgltf, stb).
nlohmann::json versionsJson();

}  // namespace wizengine
