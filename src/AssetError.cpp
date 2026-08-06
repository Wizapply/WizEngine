#include "AssetError.h"

#include <fstream>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace wizengine {

namespace {

std::string currentDirectory() {
    char buf[1024] = {0};
#ifdef _WIN32
    if (!_getcwd(buf, sizeof(buf))) return "(unknown)";
#else
    if (!getcwd(buf, sizeof(buf))) return "(unknown)";
#endif
    return buf;
}

bool isAbsolute(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() > 1 && p[1] == ':';  // C:\... on Windows
}

}  // namespace

std::string assetPath(const std::string& name) {
    if (name.empty() || isAbsolute(name)) return name;
    const std::string root = std::string(kAssetRoot) + "/";
    if (name.compare(0, root.size(), root) == 0) return name;  // already rooted
    return root + name;
}

void requireFile(const std::string& path, const std::string& what) {
    const std::string full = assetPath(path);
    std::ifstream f(full, std::ios::binary);
    if (f) return;
    // Relative paths are resolved against the working directory, so naming it
    // turns "file not found" into something the user can act on.
    throw AssetError(full, what + " not found (looked in " +
                               currentDirectory() + ")");
}

}  // namespace wizengine
