#include "core/FileBytes.h"

#include <algorithm>
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

namespace {

// path の先頭から count バイト（ファイルの大きさ以下）を読む。読めた長さが count に
// 足りなければ失敗（読む途中で縮んだファイル）。
bool readCount(const std::filesystem::path& path, std::uintmax_t count,
               std::vector<std::uint8_t>& out, std::string& reason) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        reason = "cannot open " + path.u8string();
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (std::size_t(in.gcount()) != bytes.size()) {
        reason = "failed to read " + path.u8string();
        return false;
    }
    out = std::move(bytes);
    return true;
}

// ファイルの大きさ。取れなければ false（理由は「開けない」）。
bool fileSizeOf(const std::filesystem::path& path, std::uintmax_t& size, std::string& reason) {
    std::error_code ec;
    size = std::filesystem::file_size(path, ec);
    if (ec) {
        reason = "cannot open " + path.u8string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

std::filesystem::path utf8Path(const std::string& path) {
    return std::filesystem::u8path(path);
}

bool readFileBytes(const std::filesystem::path& path, std::uintmax_t maxBytes,
                   std::vector<std::uint8_t>& out, std::string& reason) {
    std::uintmax_t fileSize = 0;
    if (!fileSizeOf(path, fileSize, reason)) return false;
    if (fileSize > maxBytes) {
        reason = path.u8string() + " is too large (" + std::to_string(fileSize) + " bytes)";
        return false;
    }
    return readCount(path, fileSize, out, reason);
}

bool readFileHead(const std::filesystem::path& path, std::uintmax_t maxBytes,
                  std::vector<std::uint8_t>& out, std::string& reason) {
    std::uintmax_t fileSize = 0;
    if (!fileSizeOf(path, fileSize, reason)) return false;
    return readCount(path, std::min(fileSize, maxBytes), out, reason);
}

}  // namespace wizengine
