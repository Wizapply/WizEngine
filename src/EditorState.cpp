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

// ---- イベントアセット -------------------------------------------------------
// 変更は必ず graphVersion_ を進める（物理スレッドが実行キャッシュを取り直す
// 合図）。mutex_ の下で書き、読みはコピーで返す - ジョイントと同じ流儀。
// ノードとワイヤーは「どのアセットのものか」を必ず伴う: 同じ id が別の
// アセットにも居るので、名前を落とすと黙って別のノードを触ってしまう。

namespace {
// name のアセットを探す（見つからなければ nullptr）。呼び出し側は mutex_ を
// 取っていること。
template <typename T>
T* findAsset(std::vector<T>& list, const std::string& name) {
    for (auto& a : list) {
        if (a.desc.name == name) return &a;
    }
    return nullptr;
}
}  // namespace

std::vector<wizengine::editor::EventAssetDesc> EditorState::eventAssets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<wizengine::editor::EventAssetDesc> out;
    out.reserve(events_.size());
    for (const auto& a : events_) out.push_back(a.desc);
    return out;
}

bool EditorState::hasEventAsset(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& a : events_) {
        if (a.desc.name == name) return true;
    }
    return false;
}

void EditorState::setEventAssets(
    std::vector<wizengine::editor::EventAssetDesc> assets,
    std::vector<std::string> worldEvents) {
    std::lock_guard<std::mutex> lk(mutex_);
    events_.clear();
    for (auto& d : assets) {
        EventAssetState st;
        // id はアセット内で一意なら何でもよい（読み込んだ文書の番号をその
        // まま使う）。次の採番だけ最大値の先へ動かす。
        for (const auto& n : d.nodes) {
            if (n.id >= st.nextNodeId) st.nextNodeId = n.id + 1;
        }
        st.desc = std::move(d);
        events_.push_back(std::move(st));
    }
    worldEvents_ = std::move(worldEvents);
    fireCounts_.clear();
    graphVersion_.fetch_add(1);
}

bool EditorState::addEventAsset(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (name.empty() || findAsset(events_, name) != nullptr) return false;
    EventAssetState st;
    st.desc.name = name;
    events_.push_back(std::move(st));
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::removeEventAsset(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = events_.size();
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [&name](const EventAssetState& a) {
                                     return a.desc.name == name;
                                 }),
                  events_.end());
    if (events_.size() == before) return false;
    // ワールドの付け先も外す（オブジェクト側は Scene が外す - BodyDesc を
    // 持っているのは向こうなので）。
    worldEvents_.erase(
        std::remove(worldEvents_.begin(), worldEvents_.end(), name),
        worldEvents_.end());
    graphVersion_.fetch_add(1);
    return true;
}

std::vector<std::string> EditorState::worldEvents() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return worldEvents_;
}

bool EditorState::attachWorldEvent(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (findAsset(events_, name) == nullptr) return false;  // 無いものは付けない
    if (std::find(worldEvents_.begin(), worldEvents_.end(), name) !=
        worldEvents_.end()) {
        return false;  // 二重付けは意味が無い（同じことを 2 回する）
    }
    worldEvents_.push_back(name);
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::detachWorldEvent(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::size_t before = worldEvents_.size();
    worldEvents_.erase(
        std::remove(worldEvents_.begin(), worldEvents_.end(), name),
        worldEvents_.end());
    if (worldEvents_.size() == before) return false;
    graphVersion_.fetch_add(1);
    return true;
}

int EditorState::addGraphNode(const std::string& asset,
                              wizengine::editor::NodeDesc node) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* a = findAsset(events_, asset);
    if (a == nullptr) return -1;
    node.id = a->nextNodeId++;
    const int id = node.id;
    a->desc.nodes.push_back(std::move(node));
    graphVersion_.fetch_add(1);
    return id;
}

bool EditorState::updateGraphNode(const std::string& asset, int id,
                                  const nlohmann::json& patch) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* a = findAsset(events_, asset);
    if (a == nullptr) return false;
    for (auto& n : a->desc.nodes) {
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

bool EditorState::removeGraphNode(const std::string& asset, int id) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* a = findAsset(events_, asset);
    if (a == nullptr) return false;
    auto& nodes = a->desc.nodes;
    const std::size_t before = nodes.size();
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [id](const wizengine::editor::NodeDesc& n) {
                                   return n.id == id;
                               }),
                nodes.end());
    if (nodes.size() == before) return false;
    auto& wires = a->desc.wires;
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [id](const wizengine::editor::WireDesc& w) {
                                   return w.from == id || w.to == id;
                               }),
                wires.end());
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::addGraphWire(const std::string& asset, int from, int to) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* st = findAsset(events_, asset);
    if (st == nullptr) return false;
    // 両端が存在し、from がトリガー・to がアクションであること。UI も同じ
    // 制約で描くが、リクエストは誰でも作れるので判定はここが持つ。
    const wizengine::editor::NodeDesc* a = nullptr;
    const wizengine::editor::NodeDesc* b = nullptr;
    for (const auto& n : st->desc.nodes) {
        if (n.id == from) a = &n;
        if (n.id == to) b = &n;
    }
    if (!a || !b) return false;
    if (!wizengine::editor::nodeIsTrigger(a->kind)) return false;
    if (wizengine::editor::nodeIsTrigger(b->kind)) return false;
    for (const auto& w : st->desc.wires) {
        if (w.from == from && w.to == to) return false;  // 二重線は張らない
    }
    st->desc.wires.push_back({from, to});
    graphVersion_.fetch_add(1);
    return true;
}

bool EditorState::removeGraphWire(const std::string& asset, int from, int to) {
    std::lock_guard<std::mutex> lk(mutex_);
    EventAssetState* st = findAsset(events_, asset);
    if (st == nullptr) return false;
    auto& wires = st->desc.wires;
    const std::size_t before = wires.size();
    wires.erase(std::remove_if(wires.begin(), wires.end(),
                               [from, to](const wizengine::editor::WireDesc& w) {
                                   return w.from == from && w.to == to;
                               }),
                wires.end());
    if (wires.size() == before) return false;
    graphVersion_.fetch_add(1);
    return true;
}

void EditorState::noteNodeFired(const std::string& asset, int id) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++fireCounts_[{asset, id}];
}

void EditorState::clearNodeFireCounts() {
    std::lock_guard<std::mutex> lk(mutex_);
    fireCounts_.clear();
}

std::map<std::pair<std::string, int>, int> EditorState::nodeFireCounts() const {
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
        reason = "cannot write: " + path;
        return false;
    }
    f << text;
    if (!f) {
        reason = "write error: " + path;
        return false;
    }
    LOGI("editor", "saved %s", path.c_str());
    return true;
}

bool EditorState::readText(const std::string& path, std::string& text,
                           std::string& reason) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        reason = "not found: " + path;
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>());
    if (f.bad()) {
        reason = "read error: " + path;
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
        reason = "not readable as JSON: " + path;
        return false;
    }
    return true;
}

