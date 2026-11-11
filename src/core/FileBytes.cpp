#include "core/FileBytes.h"

#include <fstream>
#include <system_error>
#include <utility>

namespace wizengine {

bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                    std::string& reason) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            reason = "cannot create directory " + parent.u8string() + ": " + ec.message();
            return false;
        }
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) {
            reason = "cannot open " + tmp.u8string() + " for writing";
            return false;
        }
        o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        o.close();
        if (!o) {
            std::filesystem::remove(tmp, ec);
            reason = "failed to write " + tmp.u8string();
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);  // 既にあれば置き換える
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        reason = "cannot replace " + path.u8string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool readFileBytes(const std::filesystem::path& path, std::uintmax_t maxBytes,
                   std::vector<std::uint8_t>& out, std::string& reason) {
    std::error_code ec;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        reason = "cannot open " + path.u8string() + ": " + ec.message();
        return false;
    }
    if (fileSize > maxBytes) {
        reason = path.u8string() + " is too large (" + std::to_string(fileSize) + " bytes)";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        reason = "cannot open " + path.u8string();
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (std::size_t(in.gcount()) != bytes.size()) {
        reason = "failed to read " + path.u8string();
        return false;
    }
    out = std::move(bytes);
    return true;
}

}  // namespace wizengine
