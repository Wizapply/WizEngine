#include "app/AppConfig.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>

#include <nlohmann/json.hpp>

namespace wizengine {

namespace {

using nlohmann::json;

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string typeName(const json& v) {
    if (v.is_null()) return "null";
    if (v.is_boolean()) return "boolean";
    if (v.is_number()) return "number";
    if (v.is_string()) return "string";
    if (v.is_array()) return "array";
    return "object";
}

// 点区切りのパス（"ai.llm.model"）で値を読む係。読んだパスと節を覚えて
// おき、最後に「知らないキー」を警告する（打ち間違いを黙って捨てない）。
class Reader {
public:
    Reader(const json& root, std::vector<std::string>& warnings)
        : root_(root), warnings_(warnings) {}

    void str(const char* path, std::string& out,
             std::initializer_list<const char*> allowed = {}) {
        const json* v = find(path);
        if (!v) return;
        if (!v->is_string()) {
            typeWarning(path, "a string", *v, "\"" + out + "\"");
            return;
        }
        std::string s = v->get<std::string>();
        if (allowed.size() > 0) {
            const std::string l = lower(s);
            bool ok = false;
            std::string list;
            for (const char* a : allowed) {
                ok = ok || l == a;
                list += (list.empty() ? "" : " / ") + std::string(a);
            }
            if (!ok) {
                warn(std::string(path) + ": '" + s + "' is not one of " + list +
                     " - using \"" + out + "\"");
                return;
            }
            s = l;
        }
        out = std::move(s);
    }

    void integer(const char* path, int& out, int lo, int hi) {
        const json* v = find(path);
        if (!v) return;
        double d = 0.0;
        if (v->is_number_integer() || v->is_number_unsigned()) {
            d = v->get<double>();
        } else if (v->is_number_float() && std::floor(v->get<double>()) == v->get<double>()) {
            d = v->get<double>();
        } else {
            typeWarning(path, "an integer", *v, std::to_string(out));
            return;
        }
        if (d < lo || d > hi) {
            const int c = int(std::clamp(d, double(lo), double(hi)));
            warn(std::string(path) + ": " + v->dump() + " is out of range (" +
                 std::to_string(lo) + ".." + std::to_string(hi) + ") - using " +
                 std::to_string(c));
            out = c;
            return;
        }
        out = int(d);
    }

    void number(const char* path, double& out, double lo, double hi) {
        const json* v = find(path);
        if (!v) return;
        if (!v->is_number()) {
            typeWarning(path, "a number", *v, json(out).dump());
            return;
        }
        const double d = v->get<double>();
        if (d < lo || d > hi) {
            const double c = std::clamp(d, lo, hi);
            warn(std::string(path) + ": " + v->dump() + " is out of range - using " +
                 json(c).dump());
            out = c;
            return;
        }
        out = d;
    }

    void boolean(const char* path, bool& out) {
        const json* v = find(path);
        if (!v) return;
        if (!v->is_boolean()) {
            typeWarning(path, "true or false", *v, out ? "true" : "false");
            return;
        }
        out = v->get<bool>();
    }

    // 値として null を許すキー（「送らない」を null で書けるように）。
    bool isNull(const char* path) {
        const json* v = find(path);
        return v && v->is_null();
    }

    // 読み終えたら呼ぶ。読んでいないキーを全部警告する。
    void reportUnknown() {
        if (!root_.is_object()) {
            warn("the top level must be an object { ... } - ignoring the file");
            return;
        }
        walk(root_, "");
    }

private:
    const json* find(const char* path) {
        known_.insert(path);
        const json* cur = &root_;
        std::string p = path;
        std::string sofar;
        std::size_t start = 0;
        while (true) {
            const std::size_t dot = p.find('.', start);
            const std::string key = p.substr(start, dot - start);
            if (!cur->is_object()) return nullptr;
            const auto it = cur->find(key);
            if (it == cur->end()) return nullptr;
            sofar += (sofar.empty() ? "" : ".") + key;
            if (dot == std::string::npos) return &*it;
            sections_.insert(sofar);
            known_.insert(sofar);
            if (!it->is_object()) {
                if (badSections_.insert(sofar).second) {
                    warn(sofar + ": must be an object { ... } - its settings are ignored");
                }
                return nullptr;
            }
            cur = &*it;
            start = dot + 1;
        }
    }

    void walk(const json& obj, const std::string& prefix) {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const std::string& key = it.key();
            // "$schema" や "_comment" のような注記は読まずに通す。
            if (!key.empty() && (key[0] == '$' || key[0] == '_')) continue;
            const std::string full = prefix.empty() ? key : prefix + "." + key;
            if (!known_.count(full)) {
                warn("unknown key '" + full + "' (ignored)");
            } else if (it->is_object() && sections_.count(full)) {
                walk(*it, full);
            }
        }
    }

    void typeWarning(const char* path, const char* want, const json& got,
                     const std::string& keeping) {
        warn(std::string(path) + ": expected " + want + ", got " + typeName(got) +
             " - using " + keeping);
    }

    void warn(std::string s) { warnings_.push_back(std::move(s)); }

    const json& root_;
    std::vector<std::string>& warnings_;
    std::set<std::string> known_;
    std::set<std::string> sections_;
    std::set<std::string> badSections_;
};

}  // namespace

bool parseAppConfig(const std::string& text, AppConfig& cfg,
                    std::vector<std::string>& warnings, std::string& error) {
    json root;
    try {
        // 第 4 引数 = コメント（// と /* */）を読み飛ばす。
        root = json::parse(text, nullptr, true, true);
    } catch (const json::parse_error& e) {
        // e.what() に行と列が入っている（"parse error at line 3, column 20: ..."）。
        error = std::string("config is not valid JSON - ") + e.what();
        return false;
    }
    Reader r(root, warnings);

    // ---- server ----
    auto& sv = cfg.server;
    r.str("server.mode", sv.mode, {"web", "window", "stream", "rtsp"});
    r.integer("server.httpPort", sv.httpPort, 1, 65535);
    r.str("server.streamHost", sv.streamHost);
    r.integer("server.streamPort", sv.streamPort, 1, 65535);
    r.str("server.rtspUrl", sv.rtspUrl);

    // ---- video ----
    auto& vd = cfg.video;
    r.integer("video.width", vd.width, 16, 7680);
    r.integer("video.height", vd.height, 16, 4320);
    r.integer("video.fps", vd.fps, 1, 240);
    r.str("video.codec", vd.codec, {"h264", "h265", "hevc", "av1", "vp9"});
    if (vd.codec == "hevc") vd.codec = "h265";
    r.str("video.encoder", vd.encoder);
    r.integer("video.bitrate", vd.bitrate, 100000, 200000000);
    r.boolean("video.gpuConvert", vd.gpuConvert);
    // 4:2:0 の色差は 2x2 画素で 1 つなので、奇数の寸法はエンコーダが嫌う。
    if (vd.width % 2 || vd.height % 2) {
        warnings.push_back("video: width and height must be even - rounded down to " +
                           std::to_string(vd.width & ~1) + "x" +
                           std::to_string(vd.height & ~1));
        vd.width &= ~1;
        vd.height &= ~1;
    }

    // ---- cpu ----
    r.str("cpu.physicsCores", cfg.cpu.physicsCores);
    r.str("cpu.renderCores", cfg.cpu.renderCores);
    r.integer("cpu.physicsThreads", cfg.cpu.physicsThreads, 0, 256);

    // ---- log ----
    r.str("log.level", cfg.logLevel, {"debug", "info", "warn", "error"});

    // ---- scene ----
    auto& sc = cfg.scene;
    int maxCameras = int(sc.maxCameras);
    int editorCamera = int(sc.editorCamera);
    r.integer("scene.maxCameras", maxCameras, 1, 16);
    r.integer("scene.editorCamera", editorCamera, 0, 15);
    if (editorCamera >= maxCameras) {
        warnings.push_back("scene.editorCamera: " + std::to_string(editorCamera) +
                           " is not below scene.maxCameras (" +
                           std::to_string(maxCameras) + ") - using 0");
        editorCamera = 0;
    }
    sc.maxCameras = std::size_t(maxCameras);
    sc.editorCamera = std::size_t(editorCamera);
    r.str("scene.startupScene", sc.startupScene);
    std::string startMode = editor::modeName(sc.startMode);
    r.str("scene.startMode", startMode, {"editor", "simulate"});
    sc.startMode = editor::modeFromName(startMode, sc.startMode);
    r.boolean("scene.idleWhenUnwatched", sc.idleWhenUnwatched);

    // ---- physics ----
    std::string backend = sc.backend == PhysicsBackend::Multicore ? "multicore" : "core";
    r.str("physics.backend", backend, {"core", "multicore"});
    sc.backend = backend == "multicore" ? PhysicsBackend::Multicore : PhysicsBackend::Core;
    r.boolean("physics.multicoreForSoftBodies", sc.multicoreForSoftBodies);
    r.integer("physics.multicoreMinBodies", sc.multicoreMinBodies, 0, 1000000);
    r.boolean("physics.terrainForcesCore", sc.terrainForcesCore);

    // ---- ai ----
    auto& ai = cfg.ai;
    r.str("ai.llm.url", ai.llmUrl);
    r.str("ai.llm.model", ai.llmModel);
    r.str("ai.llm.apiKey", ai.llmKey);
    if (r.isNull("ai.llm.temperature")) {
        ai.llmTemperature = -1.0;  // null = 送らない
    } else {
        r.number("ai.llm.temperature", ai.llmTemperature, -1.0, 2.0);
    }
    r.integer("ai.llm.timeoutSec", ai.llmTimeoutSec, 5, 3600);
    r.integer("ai.llm.repairs", ai.llmRepairs, 0, 5);
    int sceneKB = int(ai.llmMaxSceneBytes / 1024);
    r.integer("ai.llm.maxSceneKB", sceneKB, 1, 1024);
    ai.llmMaxSceneBytes = std::size_t(sceneKB) * 1024;

    r.str("ai.meshy.url", ai.meshyUrl);
    r.str("ai.meshy.apiKey", ai.meshyKey);
    r.boolean("ai.meshy.refine", ai.meshyRefine);
    r.str("ai.meshy.artStyle", ai.meshyArtStyle);
    r.str("ai.meshy.aiModel", ai.meshyAiModel);
    r.integer("ai.meshy.targetPolycount", ai.meshyTargetPolycount, 0, 300000);
    r.integer("ai.meshy.pollSec", ai.meshyPollSec, 1, 60);
    r.integer("ai.meshy.timeoutSec", ai.meshyTimeoutSec, 30, 7200);
    int downloadMB = int(ai.meshyMaxDownloadBytes / (1024 * 1024));
    r.integer("ai.meshy.maxDownloadMB", downloadMB, 1, 2048);
    ai.meshyMaxDownloadBytes = std::size_t(downloadMB) * 1024 * 1024;

    r.integer("ai.maxMeshes", ai.maxMeshes, 0, 16);
    r.str("ai.caFile", ai.caFile);
    r.str("ai.generatedDir", ai.generatedDir);
    // 生成物の置き場は assets/ の中だけ（シーン文書の <mesh file> の関所と同じ）。
    if (ai.generatedDir.empty() || ai.generatedDir[0] == '/' || ai.generatedDir[0] == '\\' ||
        (ai.generatedDir.size() > 1 && ai.generatedDir[1] == ':') ||
        ai.generatedDir.find("..") != std::string::npos) {
        warnings.push_back("ai.generatedDir: '" + ai.generatedDir +
                           "' must be a folder under assets/ - using \"generated\"");
        ai.generatedDir = "generated";
    }

    r.reportUnknown();
    return true;
}

ConfigLoad loadAppConfig(const std::string& path, AppConfig& cfg) {
    ConfigLoad res;
    res.path = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) return res;
    res.found = true;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // UTF-8 の BOM（Windows のメモ帳が付ける）は JSON の外なので落とす。
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    parseAppConfig(text, cfg, res.warnings, res.error);
    return res;
}

}  // namespace wizengine
