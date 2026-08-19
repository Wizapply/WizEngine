#include "EditorState.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "AssetError.h"
#include "Log.h"

// 保存名に使ってよい文字だけ残す。".." や "/" を弾くのが目的（保存先は
// assets/scenes に固定したい）。
std::string EditorState::sanitizeSceneName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) out.push_back(c);
        if (out.size() >= 64) break;
    }
    return out;
}

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

// ---- イベントグラフ ---------------------------------------------------------
// 変更は必ず graphVersion_ を進める（物理スレッドが実行キャッシュを取り直す
// 合図）。mutex_ の下で書き、読みはコピーで返す - ジョイントと同じ流儀。

std::vector<wizengine::editor::NodeDesc> EditorState::graphNodes() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return nodes_;
}

std::vector<wizengine::editor::WireDesc> EditorState::graphWires() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return wires_;
}

void EditorState::setGraph(std::vector<wizengine::editor::NodeDesc> nodes,
                           std::vector<wizengine::editor::WireDesc> wires) {
    std::lock_guard<std::mutex> lk(mutex_);
    nodes_ = std::move(nodes);
    wires_ = std::move(wires);
    // id はグラフ内で一意なら何でもよい（読み込んだ文書の番号をそのまま
    // 使う）。次の採番だけ最大値の先へ動かす。
    nextNodeId_ = 1;
    for (const auto& n : nodes_) {
        if (n.id >= nextNodeId_) nextNodeId_ = n.id + 1;
    }
    fireCounts_.clear();
    graphVersion_.fetch_add(1);
}

int EditorState::addGraphNode(wizengine::editor::NodeDesc node) {
    std::lock_guard<std::mutex> lk(mutex_);
    node.id = nextNodeId_++;
    const int id = node.id;
    nodes_.push_back(std::move(node));
    graphVersion_.fetch_add(1);
    return id;
}

bool EditorState::updateGraphNode(int id, const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& n : nodes_) {
        if (n.id != id) continue;
        // 種類と id は変えさせない（種類が変わると target の意味とワイヤーの
        // 向きが崩れる。作り直したほうが安全）。
        wizengine::editor::NodeDesc next =
            wizengine::editor::clampNode(wizengine::editor::nodeFromJson(patch, n));
        next.id = n.id;
        next.kind = n.kind;
        n = next;
        graphVersion_.fetch_add(1);
        return true;
    }
    return false;
}

bool EditorState::removeGraphNode(int id) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = nodes_.size();
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                [id](const wizengine::editor::NodeDesc& n) {
                                    return n.id == id;
                                }),
                 nodes_.end());
    if (nodes_.size() == before) return false;
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                [id](const wizengine::editor::WireDesc& w) {
                                    return w.from == id || w.to == id;
                                }),
                 wires_.end());
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::addGraphWire(int from, int to) {
    std::lock_guard<std::mutex> lk(mutex_);
    // 両端が存在し、from がトリガー・to がアクションであること。UI も同じ
    // 制約で描くが、リクエストは誰でも作れるので判定はここが持つ。
    const wizengine::editor::NodeDesc* a = nullptr;
    const wizengine::editor::NodeDesc* b = nullptr;
    for (const auto& n : nodes_) {
        if (n.id == from) a = &n;
        if (n.id == to) b = &n;
    }
    if (!a || !b) return false;
    if (!wizengine::editor::nodeIsTrigger(a->kind)) return false;
    if (wizengine::editor::nodeIsTrigger(b->kind)) return false;
    for (const auto& w : wires_) {
        if (w.from == from && w.to == to) return false;  // 二重線は張らない
    }
    wires_.push_back({from, to});
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::removeGraphWire(int from, int to) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = wires_.size();
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                [from, to](const wizengine::editor::WireDesc& w) {
                                    return w.from == from && w.to == to;
                                }),
                 wires_.end());
    if (wires_.size() == before) return false;
    graphVersion_.fetch_add(1);
    return true;
}

void EditorState::noteNodeFired(int id) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& fc : fireCounts_) {
        if (fc.first == id) {
            ++fc.second;
            return;
        }
    }
    fireCounts_.push_back({id, 1});
}

void EditorState::clearNodeFireCounts() {
    std::lock_guard<std::mutex> lk(mutex_);
    fireCounts_.clear();
}

std::vector<std::pair<int, int>> EditorState::nodeFireCounts() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return fireCounts_;
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
            const std::string ext = entry.path().extension().string();
            // .xml が今の形式、.json は旧形式（読み込みのみ）。一覧に出す
            // 名前は拡張子を落としたものなので、同名の新旧が両方あっても
            // タイルは 1 個になる（読み込みは .xml を先に見る）。
            if (ext != ".xml" && ext != ".json") continue;
            found.push_back(entry.path().stem().string());
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::lock_guard<std::mutex> lk(mutex_);
    files_.swap(found);
}

std::string EditorState::scenesDir() {
    // 実行時に読むものは全部 assets/ 以下、という既存の約束に合わせる。
    return wizengine::assetPath("scenes");
}

std::string EditorState::scenePath(const std::string& name) {
    const std::string safe = sanitizeSceneName(name);
    if (safe.empty()) return {};
    return scenesDir() + "/" + safe + ".xml";
}

std::string EditorState::legacyScenePath(const std::string& name) {
    const std::string safe = sanitizeSceneName(name);
    if (safe.empty()) return {};
    return scenesDir() + "/" + safe + ".json";
}

bool EditorState::writeText(const std::string& path, const std::string& text,
                            std::string& reason) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        reason = "書き込めません: " + path;
        return false;
    }
    f << text;
    if (!f) {
        reason = "書き込み中にエラー: " + path;
        return false;
    }
    LOGI("editor", "saved %s", path.c_str());
    return true;
}

bool EditorState::readText(const std::string& path, std::string& text,
                           std::string& reason) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        reason = "見つかりません: " + path;
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>());
    if (f.bad()) {
        reason = "読み込み中にエラー: " + path;
        return false;
    }
    LOGI("editor", "loaded %s", path.c_str());
    return true;
}

bool EditorState::readJson(const std::string& path, nlohmann::json& doc,
                           std::string& reason) {
    std::string text;
    if (!readText(path, text, reason)) return false;
    doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        reason = "JSON として読めません: " + path;
        return false;
    }
    return true;
}

