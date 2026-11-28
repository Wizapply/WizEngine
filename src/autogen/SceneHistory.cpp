#include "autogen/SceneHistory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "autogen/SceneDigest.h"
#include "core/Log.h"

namespace wizengine {
namespace autogen {

namespace ed = editor;
namespace fs = std::filesystem;

namespace {

// 履歴に残す依頼文の長さ（バイト）。長い依頼は先頭だけで流れは分かる。
constexpr std::size_t kMaxRequestBytes = 400;
// プロンプトに入れるとき、1 件あたりの変更の行数の上限。
constexpr std::size_t kMaxLinesPerEntry = 25;

std::string fmt(double v, int digits = 2) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, std::fabs(v) < 0.5e-2 ? 0.0 : v);
    return buf;
}
std::string fmtVec(const Vec3d& v) {
    return "(" + fmt(v.x) + ", " + fmt(v.y) + ", " + fmt(v.z) + ")";
}
std::string fmtSize(const Vec3d& v) { return fmt(v.x) + " x " + fmt(v.y) + " x " + fmt(v.z); }

double dist(const Vec3d& a, const Vec3d& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}
double maxDiff(const Vec3d& a, const Vec3d& b) {
    return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
}

// UTF-8 の文字の途中で切らずに n バイトまでに縮める。
std::string clip(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    std::size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

// 名前で突き合わせるためのキー（名前の無い物は種類ごとの並び順）。
template <class T>
std::vector<std::string> keysOf(const std::vector<T>& items, const char* unnamedPrefix) {
    std::vector<std::string> keys;
    int unnamed = 0;
    for (const auto& it : items) {
        keys.push_back(it.name.empty() ? std::string(unnamedPrefix) + std::to_string(unnamed++)
                                       : it.name);
    }
    return keys;
}
int findKey(const std::vector<std::string>& keys, const std::string& k) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == k) return int(i);
    }
    return -1;
}

std::string shapeWord(const ed::BodyDesc& b) {
    switch (b.shape) {
        case ed::ShapeKind::Box: return "box";
        case ed::ShapeKind::Sphere: return "sphere";
        case ed::ShapeKind::Model: return "model \"" + b.mesh + "\"";
        case ed::ShapeKind::Trimesh: return "mesh collider";
        case ed::ShapeKind::None: return "frame";
    }
    return "body";
}
std::string bodyLabel(const ed::BodyDesc& b) {
    return b.name.empty() ? "an unnamed " + shapeWord(b) : "\"" + b.name + "\"";
}
std::string bodyBrief(const ed::BodyDesc& b) {
    std::string s = shapeWord(b);
    if (b.shape == ed::ShapeKind::Box || b.shape == ed::ShapeKind::Trimesh) {
        s += " " + fmtSize(b.size) + " m";
    } else if (b.shape != ed::ShapeKind::None) {
        s += " " + fmt(b.size.x) + " m";
    }
    if (b.shape != ed::ShapeKind::None) s += ", " + colorName(b.color.r, b.color.g, b.color.b);
    s += " at " + fmtVec(b.position);
    if (b.fixed) s += ", fixed";
    return s;
}

std::string bodyNameAt(const ed::SceneDocument& d, int index) {
    if (index < 0 || std::size_t(index) >= d.bodies.size()) return "world";
    const auto& b = d.bodies[std::size_t(index)];
    return b.name.empty() ? "#" + std::to_string(index) : b.name;
}

void diffBodies(const ed::SceneDocument& A, const ed::SceneDocument& B,
                std::vector<std::string>& out) {
    const auto ka = keysOf(A.bodies, "#unnamed");
    const auto kb = keysOf(B.bodies, "#unnamed");
    for (std::size_t i = 0; i < A.bodies.size(); ++i) {
        if (findKey(kb, ka[i]) < 0) out.push_back("deleted " + bodyLabel(A.bodies[i]));
    }
    for (std::size_t j = 0; j < B.bodies.size(); ++j) {
        const ed::BodyDesc& b = B.bodies[j];
        const int i = findKey(ka, kb[j]);
        if (i < 0) {
            out.push_back("added " + bodyLabel(b) + ": " + bodyBrief(b));
            continue;
        }
        const ed::BodyDesc& a = A.bodies[std::size_t(i)];
        const std::string n = bodyLabel(b);
        bool any = false;
        auto line = [&](std::string s) {
            out.push_back(std::move(s));
            any = true;
        };
        if (a.shape != b.shape || a.mesh != b.mesh) {
            line("changed the shape of " + n + " from " + shapeWord(a) + " to " + shapeWord(b));
        }
        if (dist(a.position, b.position) > 0.005) {
            line("moved " + n + " from " + fmtVec(a.position) + " to " + fmtVec(b.position));
        }
        if (maxDiff(a.rotation, b.rotation) > 0.5) {
            line("rotated " + n + " from " + fmtVec(a.rotation) + " to " + fmtVec(b.rotation) +
                 " deg");
        }
        if (maxDiff(a.size, b.size) > 0.002) {
            line("resized " + n + " from " + fmtSize(a.size) + " to " + fmtSize(b.size) + " m");
        }
        if (std::max({std::fabs(a.color.r - b.color.r), std::fabs(a.color.g - b.color.g),
                      std::fabs(a.color.b - b.color.b)}) > 0.02) {
            const std::string ca = colorName(a.color.r, a.color.g, a.color.b);
            const std::string cb = colorName(b.color.r, b.color.g, b.color.b);
            line(ca == cb ? "changed the colour of " + n + " (still " + cb + ")"
                          : "recoloured " + n + " from " + ca + " to " + cb);
        }
        if (a.fixed != b.fixed) line(b.fixed ? "made " + n + " fixed" : "made " + n + " dynamic");
        if (std::fabs(a.mass - b.mass) > 0.01 * std::max(std::fabs(a.mass), 1e-6)) {
            line("changed the mass of " + n + " from " + fmt(a.mass) + " to " + fmt(b.mass) +
                 " kg");
        }
        if (a.prefab != b.prefab) {
            line(b.prefab.empty() ? "removed prefab \"" + a.prefab + "\" from " + n
                                  : "gave " + n + " prefab \"" + b.prefab + "\"");
        }
        if (a.hasVehicle != b.hasVehicle) {
            line(b.hasVehicle ? "made " + n + " a vehicle" : n + " is no longer a vehicle");
        }
        if (a.hasSoft != b.hasSoft) {
            line(b.hasSoft ? "made " + n + " a soft body" : n + " is no longer a soft body");
        }
        if (!any) {
            // 上で見ていない値（材質・物性・イベント・車両の中身など）。
            nlohmann::json ja = ed::toJson(a), jb = ed::toJson(b);
            if (ja != jb) out.push_back("changed other properties of " + n);
        }
    }
}

void diffJoints(const ed::SceneDocument& A, const ed::SceneDocument& B,
                std::vector<std::string>& out) {
    const auto ka = keysOf(A.joints, "#joint");
    const auto kb = keysOf(B.joints, "#joint");
    auto label = [](const ed::JointDesc& j) {
        return j.name.empty() ? std::string("an unnamed joint") : "joint \"" + j.name + "\"";
    };
    for (std::size_t i = 0; i < A.joints.size(); ++i) {
        if (findKey(kb, ka[i]) < 0) out.push_back("deleted " + label(A.joints[i]));
    }
    for (std::size_t j = 0; j < B.joints.size(); ++j) {
        const ed::JointDesc& b = B.joints[j];
        const int i = findKey(ka, kb[j]);
        const std::string ends = bodyNameAt(B, b.bodyA) + " - " + bodyNameAt(B, b.bodyB);
        if (i < 0) {
            out.push_back("added " + label(b) + " (" + ed::jointName(b.kind) + ", " + ends +
                          ", anchor " + fmtVec(b.anchor) + ")");
            continue;
        }
        const ed::JointDesc& a = A.joints[std::size_t(i)];
        const std::string endsA = bodyNameAt(A, a.bodyA) + " - " + bodyNameAt(A, a.bodyB);
        // 端は番号ではなく名前で比べる（文書ごとに番号が違うため）。
        nlohmann::json ja = ed::toJson(a), jb = ed::toJson(b);
        ja.erase("a"); ja.erase("b");
        jb.erase("a"); jb.erase("b");
        if (a.kind != b.kind || endsA != ends || dist(a.anchor, b.anchor) > 0.005 || ja != jb) {
            out.push_back("changed " + label(b) + " (now " + ed::jointName(b.kind) + ", " + ends +
                          ", anchor " + fmtVec(b.anchor) + ")");
        }
    }
}

void diffLights(const ed::SceneDocument& A, const ed::SceneDocument& B,
                std::vector<std::string>& out) {
    const auto ka = keysOf(A.lights, "#light");
    const auto kb = keysOf(B.lights, "#light");
    auto label = [](const ed::LightDesc& l) {
        return l.name.empty() ? std::string("an unnamed light") : "light \"" + l.name + "\"";
    };
    for (std::size_t i = 0; i < A.lights.size(); ++i) {
        if (findKey(kb, ka[i]) < 0) out.push_back("deleted " + label(A.lights[i]));
    }
    for (std::size_t j = 0; j < B.lights.size(); ++j) {
        const ed::LightDesc& b = B.lights[j];
        const int i = findKey(ka, kb[j]);
        if (i < 0) {
            out.push_back("added " + label(b) + " (" + ed::lightKindName(b.kind) + " at " +
                          fmtVec(b.position) + ")");
            continue;
        }
        const ed::LightDesc& a = A.lights[std::size_t(i)];
        if (dist(a.position, b.position) > 0.005) {
            out.push_back("moved " + label(b) + " to " + fmtVec(b.position));
        }
        if (maxDiff(a.rotation, b.rotation) > 0.5) {
            out.push_back("turned " + label(b) + " to euler " + fmtVec(b.rotation) + " deg");
        }
        if (std::fabs(a.intensity - b.intensity) > 0.01 * std::max(a.intensity, 1.0)) {
            out.push_back("changed the intensity of " + label(b) + " from " +
                          fmt(a.intensity, 0) + " to " + fmt(b.intensity, 0));
        }
        if (a.kind != b.kind) {
            out.push_back("changed " + label(b) + " to a " + ed::lightKindName(b.kind) + " light");
        }
    }
}

void diffNamed(const std::vector<std::string>& a, const std::vector<std::string>& b,
               const char* what, std::vector<std::string>& out) {
    for (const auto& n : a) {
        if (std::find(b.begin(), b.end(), n) == b.end()) out.push_back(std::string("removed ") + what + " \"" + n + "\"");
    }
    for (const auto& n : b) {
        if (std::find(a.begin(), a.end(), n) == a.end()) out.push_back(std::string("added ") + what + " \"" + n + "\"");
    }
}

}  // namespace

nlohmann::json toJson(const HistoryEntry& e) {
    nlohmann::json j;
    j["time"] = e.time;
    j["source"] = e.source;
    if (!e.mode.empty()) j["mode"] = e.mode;
    if (!e.request.empty()) j["request"] = e.request;
    if (!e.outcome.empty()) j["outcome"] = e.outcome;
    j["changes"] = e.changes;
    return j;
}

HistoryEntry historyEntryFromJson(const nlohmann::json& j) {
    HistoryEntry e;
    auto str = [&](const char* k) {
        return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string();
    };
    e.time = str("time");
    e.source = str("source");
    e.mode = str("mode");
    e.request = str("request");
    e.outcome = str("outcome");
    if (j.contains("changes") && j["changes"].is_array()) {
        for (const auto& c : j["changes"]) {
            if (c.is_string()) e.changes.push_back(c.get<std::string>());
        }
    }
    return e;
}

std::vector<std::string> diffScenes(const ed::SceneDocument& before,
                                    const ed::SceneDocument& after, std::size_t maxLines) {
    std::vector<std::string> out;
    diffBodies(before, after, out);
    diffJoints(before, after, out);
    diffLights(before, after, out);

    std::vector<std::string> ma, mb;
    for (const auto& m : before.meshes) ma.push_back(m.name);
    for (const auto& m : after.meshes) mb.push_back(m.name);
    diffNamed(ma, mb, "mesh asset", out);

    // 地面（あるか・大きさ・置き場所・見た目）。
    const auto& ga = before.ground;
    const auto& gb = after.ground;
    if (ga.enabled != gb.enabled) {
        out.push_back(gb.enabled ? "restored the ground" : "removed the ground");
    } else if (gb.enabled && ed::toJson(ga) != ed::toJson(gb)) {
        std::string s = "changed the ground";
        if (std::fabs(ga.half - gb.half) > 1e-3) s += " (size " + fmt(gb.half * 2) + " m)";
        if (dist(ga.position, gb.position) > 0.005) s += " (moved to " + fmtVec(gb.position) + ")";
        out.push_back(s);
    }
    if (ed::toJson(before.environment) != ed::toJson(after.environment)) {
        out.push_back("changed the environment light (hdr \"" + after.environment.hdr + "\")");
    }
    if (ed::toJson(before.render) != ed::toJson(after.render)) {
        out.push_back("changed the rendering settings (<visual>)");
    }
    if (ed::toJson(before.sim) != ed::toJson(after.sim)) {
        out.push_back("changed the simulation settings (<option>)");
    }
    if (before.cables.size() != after.cables.size()) {
        out.push_back("cables: " + std::to_string(before.cables.size()) + " -> " +
                      std::to_string(after.cables.size()));
    }
    if (before.emitters.size() != after.emitters.size()) {
        out.push_back("particle emitters: " + std::to_string(before.emitters.size()) + " -> " +
                      std::to_string(after.emitters.size()));
    }
    // オーディオ（音源の数と、名前ごとの中身。耳は有無と中身）。
    if (before.audioSources.size() != after.audioSources.size()) {
        out.push_back("audio sources: " + std::to_string(before.audioSources.size()) + " -> " +
                      std::to_string(after.audioSources.size()));
    } else {
        for (std::size_t i = 0; i < after.audioSources.size(); ++i) {
            if (ed::toJson(before.audioSources[i]) != ed::toJson(after.audioSources[i])) {
                out.push_back("changed the audio source \"" + after.audioSources[i].name + "\"");
            }
        }
    }
    if (before.hasAudioListener != after.hasAudioListener ||
        ed::toJson(before.audioListener) != ed::toJson(after.audioListener)) {
        out.push_back("changed the audio listener");
    }
    if (before.hasTerrain != after.hasTerrain) {
        out.push_back(after.hasTerrain ? "added a terrain" : "removed the terrain");
    }
    // 水面と流体粒子（単一要素。中身は JSON で比べる）。
    if (before.hasWater != after.hasWater) {
        out.push_back(after.hasWater ? "added water (<water>)" : "removed the water");
    } else if (after.hasWater && ed::toJson(before.water) != ed::toJson(after.water)) {
        out.push_back("changed the water (<water>)");
    }
    if (before.hasFluid != after.hasFluid) {
        out.push_back(after.hasFluid ? "added fluid particles (<fluid>)"
                                     : "removed the fluid particles");
    } else if (after.hasFluid && ed::toJson(before.fluid) != ed::toJson(after.fluid)) {
        out.push_back("changed the fluid particles (<fluid>)");
    }

    if (out.size() > maxLines) {
        const std::size_t more = out.size() - maxLines;
        out.resize(maxLines);
        out.push_back("... and " + std::to_string(more) + " more changes");
    }
    return out;
}

std::vector<std::string> describeNewScene(const ed::SceneDocument& doc, std::size_t maxLines) {
    std::vector<std::string> out;
    out.push_back("replaced the whole scene: " + std::to_string(doc.bodies.size()) + " bodies, " +
                  std::to_string(doc.joints.size()) + " joints, " +
                  std::to_string(doc.lights.size()) + " lights");
    for (const auto& b : doc.bodies) {
        if (out.size() >= maxLines) {
            out.push_back("...");
            break;
        }
        out.push_back(bodyLabel(b) + ": " + bodyBrief(b));
    }
    return out;
}

std::string formatHistory(const std::vector<HistoryEntry>& entries) {
    if (entries.empty()) return std::string();
    std::string s =
        "HISTORY OF THIS SCENE (oldest first). Context only: the CURRENT scene document is the "
        "truth. Use it to understand follow-up requests (\"the table you added\", \"undo that\", "
        "\"like before\") and do NOT undo changes the user made by hand unless asked.\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const HistoryEntry& e = entries[i];
        s += std::to_string(i + 1) + ". [" + e.time + "] ";
        if (e.source == "user") {
            s += "THE USER edited the scene by hand in the editor:\n";
        } else {
            s += "AI (" + (e.mode.empty() ? std::string("?") : e.mode) + " mode), request: \"" +
                 e.request + "\" -> " + (e.outcome.empty() ? std::string("applied") : e.outcome);
            s += e.changes.empty() ? "\n" : ". Changes:\n";
        }
        const std::size_t n = std::min(e.changes.size(), kMaxLinesPerEntry);
        for (std::size_t k = 0; k < n; ++k) s += "   - " + e.changes[k] + "\n";
        if (e.changes.size() > n) {
            s += "   - ... and " + std::to_string(e.changes.size() - n) + " more\n";
        }
    }
    if (entries.back().source == "user") {
        s += "The last entry shows what the user changed by hand since your previous answer; "
             "those changes are intentional - keep them.\n";
    }
    return s;
}

std::string historyTimestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string historyKey(const std::string& sceneFile) {
    // シーン名は EditorState::sanitizeSceneName を通った英数字と _ - だけだが、
    // ファイル名にするのでここでも念のため絞る。
    std::string out;
    for (const char c : sceneFile) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) out.push_back(c);
        if (out.size() >= 80) break;
    }
    return out.empty() ? std::string("_untitled") : out;
}

// ---- 保存 ---------------------------------------------------------------------

SceneHistoryStore::SceneHistoryStore(std::string dir) : dir_(std::move(dir)) {}

std::string SceneHistoryStore::pathFor(const std::string& key) const {
    return (fs::path(dir_) / (key + ".json")).string();
}

SceneHistoryStore::Data& SceneHistoryStore::load(const std::string& key) const {
    Data& d = cache_[key];
    if (d.loaded) return d;
    d.loaded = true;
    std::ifstream in(pathFor(key), std::ios::binary);
    if (!in) return d;  // まだ無い = 空の履歴
    std::stringstream ss;
    ss << in.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LOGW("ai", "history: cannot parse %s - starting a new history", pathFor(key).c_str());
        return d;
    }
    if (j.contains("baseline") && j["baseline"].is_string()) {
        d.baseline = j["baseline"].get<std::string>();
    }
    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& e : j["entries"]) {
            if (e.is_object()) d.entries.push_back(historyEntryFromJson(e));
        }
    }
    return d;
}

void SceneHistoryStore::save(const std::string& key, const Data& d) const {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    nlohmann::json j;
    j["scene"] = key;
    j["baseline"] = d.baseline;
    j["entries"] = nlohmann::json::array();
    for (const auto& e : d.entries) j["entries"].push_back(toJson(e));
    // 書きかけで落ちても前の履歴が壊れないよう、別名に書いてから置き換える。
    const std::string path = pathFor(key);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            LOGW("ai", "history: cannot write %s", tmp.c_str());
            return;
        }
        out << j.dump(1);
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // Windows では既にあるファイルへの rename が失敗する版がある。
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) LOGW("ai", "history: cannot replace %s (%s)", path.c_str(), ec.message().c_str());
    }
}

std::vector<HistoryEntry> SceneHistoryStore::entries(const std::string& scene,
                                                     std::size_t n) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const Data& d = load(historyKey(scene));
    if (n == 0 || n >= d.entries.size()) return d.entries;
    return std::vector<HistoryEntry>(d.entries.end() - std::ptrdiff_t(n), d.entries.end());
}

std::string SceneHistoryStore::baseline(const std::string& scene) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return load(historyKey(scene)).baseline;
}

void SceneHistoryStore::add(const std::string& scene, HistoryEntry e) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::string key = historyKey(scene);
    Data& d = load(key);
    e.request = clip(e.request, kMaxRequestBytes);
    d.entries.push_back(std::move(e));
    if (d.entries.size() > kKeep) {
        d.entries.erase(d.entries.begin(), d.entries.end() - std::ptrdiff_t(kKeep));
    }
    save(key, d);
}

void SceneHistoryStore::setBaseline(const std::string& scene, std::string xml) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::string key = historyKey(scene);
    Data& d = load(key);
    d.baseline = std::move(xml);
    save(key, d);
}

void SceneHistoryStore::addWithBaseline(const std::string& scene, HistoryEntry e,
                                        std::string xml) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::string key = historyKey(scene);
    Data& d = load(key);
    e.request = clip(e.request, kMaxRequestBytes);
    d.entries.push_back(std::move(e));
    if (d.entries.size() > kKeep) {
        d.entries.erase(d.entries.begin(), d.entries.end() - std::ptrdiff_t(kKeep));
    }
    d.baseline = std::move(xml);
    save(key, d);
}

void SceneHistoryStore::clear(const std::string& scene) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::string key = historyKey(scene);
    cache_[key] = Data{};
    cache_[key].loaded = true;
    std::error_code ec;
    fs::remove(pathFor(key), ec);
}

}  // namespace autogen
}  // namespace wizengine
