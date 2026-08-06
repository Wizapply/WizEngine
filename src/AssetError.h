#pragma once

#include <stdexcept>
#include <string>

namespace wizengine {

// Thrown when an asset named by the scene cannot be loaded.
//
// Silently carrying on with a missing model or texture is worse than stopping:
// the scene then differs from what was configured, and the reason is a single
// line lost in the log. main() catches this, prints it and exits, so a typo in
// scene.cpp fails immediately and visibly.
class AssetError : public std::runtime_error {
public:
    AssetError(const std::string& path, const std::string& what)
        : std::runtime_error("asset '" + path + "': " + what) {}
};

// Everything loaded at runtime lives under this folder, next to the
// executable: compiled materials, textures, models and the browser UI. Keeping
// one root means a missing file has one place to look and one thing to fix.
inline constexpr const char* kAssetRoot = "assets";

// Resolves a scene-relative name to a path under kAssetRoot. An absolute path
// (or one that already starts with the asset root) is returned unchanged, so a
// model can still be loaded from anywhere on disk.
std::string assetPath(const std::string& name);

// Throws unless the file exists and can be opened for reading. `what`
// describes the role of the file, so the message says what was being loaded.
// The path is resolved with assetPath() first.
void requireFile(const std::string& path, const std::string& what);

}  // namespace wizengine
