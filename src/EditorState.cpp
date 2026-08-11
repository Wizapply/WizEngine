#include "EditorState.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "AssetError.h"
#include "Log.h"

namespace {

// 保存名に使ってよい文字だけ残す。".." や "/" を弾くのが目的（保存先は
// assets/scenes に固定したい）。
std::string sanitizeName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) out.push_back(c);
        if (out.size() >= 64) break;
    }
    return out;
}

}  // namespace

void EditorState::push(Op op) {
    std::lock_guard<std::mutex> lk(mutex_);
    // 暴走したクライアントがキューを膨らませても、物理スレッドが 1 パスで
    // 捌ける量を大きく超えないように上限を置く。
    if (pending_.size() >= 512) return;
    pending_.push_back(std::move(op));
    pendingCount_.store(int(pending_.size()));
}

std::vector<EditorState::Op> EditorState::drain() {
    std::vector<Op> out;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        out.swap(pending_);
        pendingCount_.store(0);
    }
    return out;
}

std::vector<wizengine::editor::JointDesc> EditorState::joints() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return joints_;
}

void EditorState::setJoints(std::vector<wizengine::editor::JointDesc> joints) {
    std::lock_guard<std::mutex> lk(mutex_);
    joints_ = std::move(joints);
}

int EditorState::addJoint(const wizengine::editor::JointDesc& joint) {
    std::lock_guard<std::mutex> lk(mutex_);
    joints_.push_back(joint);
    return int(joints_.size()) - 1;
}

bool EditorState::removeJoint(int index) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (index < 0 || std::size_t(index) >= joints_.size()) return false;
    joints_.erase(joints_.begin() + index);
    return true;
}

std::size_t EditorState::jointCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return joints_.size();
}

wizengine::editor::SimSettings EditorState::sim() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return sim_;
}

void EditorState::setSim(const wizengine::editor::SimSettings& s) {
    std::lock_guard<std::mutex> lk(mutex_);
    sim_ = s;
}

wizengine::editor::GizmoSettings EditorState::gizmo() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return gizmo_;
}

void EditorState::setGizmo(const wizengine::editor::GizmoSettings& g) {
    std::lock_guard<std::mutex> lk(mutex_);
    gizmo_ = g;
}

void EditorState::setStatus(std::string text) {
    std::lock_guard<std::mutex> lk(mutex_);
    status_ = std::move(text);
}

std::string EditorState::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

void EditorState::setSceneFile(std::string name) {
    std::lock_guard<std::mutex> lk(mutex_);
    sceneFile_ = std::move(name);
}

std::string EditorState::sceneFile() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return sceneFile_;
}

std::vector<std::string> EditorState::sceneFiles() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return files_;
}

void EditorState::refreshSceneFiles() {
    std::vector<std::string> found;
    std::error_code ec;
    const std::filesystem::path dir(scenesDir());
    if (std::filesystem::exists(dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".json") continue;
            found.push_back(entry.path().stem().string());
        }
    }
    std::sort(found.begin(), found.end());
    std::lock_guard<std::mutex> lk(mutex_);
    files_.swap(found);
}

std::string EditorState::scenesDir() {
    // 実行時に読むものは全部 assets/ 以下、という既存の約束に合わせる。
    return wizengine::assetPath("scenes");
}

std::string EditorState::scenePath(const std::string& name) {
    const std::string safe = sanitizeName(name);
    if (safe.empty()) return {};
    return scenesDir() + "/" + safe + ".json";
}

bool EditorState::writeJson(const std::string& path, const nlohmann::json& doc,
                            std::string& reason) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        reason = "書き込めません: " + path;
        return false;
    }
    f << doc.dump(2) << "\n";
    if (!f) {
        reason = "書き込み中にエラー: " + path;
        return false;
    }
    LOGI("editor", "saved %s", path.c_str());
    return true;
}

bool EditorState::readJson(const std::string& path, nlohmann::json& doc,
                           std::string& reason) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        reason = "見つかりません: " + path;
        return false;
    }
    doc = nlohmann::json::parse(f, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        reason = "JSON として読めません: " + path;
        return false;
    }
    LOGI("editor", "loaded %s", path.c_str());
    return true;
}

