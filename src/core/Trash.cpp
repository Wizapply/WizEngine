// ファイルをごみ箱へ移す。宣言と OS ごとの振る舞いは Trash.h。
#include "core/Trash.h"

#include <system_error>

#include "core/SafeName.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace wizengine {

bool moveToTrash(const std::filesystem::path& path, std::string& reason) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        reason = "cannot move " + path.u8string() + " to the trash: not a file";
        return false;
    }
    // シェルは相対パスをごみ箱へ送らずに消すことがあるので、絶対パスにする。
    const std::filesystem::path full = std::filesystem::absolute(path, ec);
    if (ec) {
        reason = "cannot move " + path.u8string() + " to the trash: " + ec.message();
        return false;
    }
#ifdef _WIN32
    // pFrom は NUL 区切りの一覧で、二重の NUL で終わる。
    std::wstring from = full.wstring();
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();  // c_str() の終端と合わせて二重の NUL
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) {
        reason = "cannot move " + full.u8string() + " to the recycle bin (code " +
                 std::to_string(rc) + ")";
        return false;
    }
    if (std::filesystem::exists(full, ec)) {
        reason = "cannot move " + full.u8string() + " to the recycle bin: still exists";
        return false;
    }
    return true;
#else
    const std::filesystem::path trash = full.parent_path() / ".trash";
    std::filesystem::create_directories(trash, ec);
    if (ec) {
        reason = "cannot create " + trash.u8string() + ": " + ec.message();
        return false;
    }
    // 同じ名前が既にごみ箱にあれば stem_2, stem_3 … の空いている名前（rename は
    // 上書きするので、空いていることを確かめてから移す。調べられない名前は
    // ふさがっている扱い）。拡張子は最後に付け直す。
    const std::string ext = full.extension().u8string();
    const std::string stem = firstFreeName(
        full.stem().u8string(), std::string::npos, [&](const std::string& name) {
            std::error_code existsEc;
            const bool exists =
                std::filesystem::exists(trash / std::filesystem::u8path(name + ext), existsEc);
            return exists || bool(existsEc);
        });
    if (stem.empty()) {
        reason = "cannot find a free name for " + full.filename().u8string() + " in " +
                 trash.u8string();
        return false;
    }
    const std::filesystem::path dst = trash / std::filesystem::u8path(stem + ext);
    std::filesystem::rename(full, dst, ec);
    if (ec) {
        reason = "cannot move " + full.u8string() + " to " + trash.u8string() + ": " +
                 ec.message();
        return false;
    }
    return true;
#endif
}

}  // namespace wizengine
