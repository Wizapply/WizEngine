#include "autogen/SceneGenerator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>

#include "autogen/AiClients.h"
#include "autogen/GlbBounds.h"
#include "autogen/HttpClient.h"
#include "autogen/SceneLayout.h"
#include "core/Log.h"
#include "core/SafeName.h"
#include "document/SceneDocument.h"
#include "document/SceneXml.h"

namespace wizengine {
namespace autogen {

namespace ed = wizengine::editor;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// ---- 小物 --------------------------------------------------------------------

std::string joinPath(const std::string& root, const std::string& rel) {
    if (root.empty()) return rel;
    return (fs::path(root) / fs::path(rel)).generic_string();
}

bool cancelled(const GenHooks& h) { return h.cancel && h.cancel->load(); }

void stage(const GenHooks& h, const char* name, double p) {
    if (h.stage) h.stage(name, std::clamp(p, 0.0, 1.0));
}

void note(const GenHooks& h, const std::string& line) {
    if (h.note) h.note(line);
}

// 待ち（cancel を 0.2 秒ごとに見る）。
void sleepFor(int seconds, const GenHooks& h) {
    const auto until = Clock::now() + std::chrono::seconds(std::max(1, seconds));
    while (Clock::now() < until && !cancelled(h)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

std::string fmt(double v, const char* f = "%.3g") {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// ---- 生成モデルのキャッシュ（assets/<generatedDir>/index.json）---------------
// {"models":[{"key","prompt","file","created"}]}。file は assets/ 相対。
// 同じ依頼（prompt + 画風 + refine + モデル）は同じキー = 再生成しない。

struct Library {
    std::string path;  // index.json の実パス
    nlohmann::json models = nlohmann::json::array();
};

Library loadLibrary(const AiConfig& cfg) {
    Library lib;
    lib.path = joinPath(cfg.assetRoot, cfg.generatedDir + "/index.json");
    std::ifstream f(lib.path, std::ios::binary);
    if (!f) return lib;
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_object() && j.contains("models") && j["models"].is_array()) {
        lib.models = j["models"];
    } else if (!text.empty()) {
        LOGW("ai", "%s is not a model index - starting a new one", lib.path.c_str());
    }
    return lib;
}

void saveLibrary(const Library& lib) {
    std::error_code ec;
    fs::create_directories(fs::path(lib.path).parent_path(), ec);
    const std::string tmp = lib.path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            LOGW("ai", "cannot write %s", tmp.c_str());
            return;
        }
        nlohmann::json j;
        j["models"] = lib.models;
        f << j.dump(2);
    }
    fs::remove(lib.path, ec);
    fs::rename(tmp, lib.path, ec);
    if (ec) LOGW("ai", "cannot update %s: %s", lib.path.c_str(), ec.message().c_str());
}

// キャッシュにあって、ファイルも残っているもの。
std::string libraryFile(const Library& lib, const AiConfig& cfg, const std::string& key) {
    for (const auto& m : lib.models) {
        if (!m.is_object() || m.value("key", std::string()) != key) continue;
        const std::string file = m.value("file", std::string());
        std::error_code ec;
        if (!file.empty() && fs::exists(joinPath(cfg.assetRoot, file), ec)) return file;
    }
    return {};
}

std::vector<LibraryEntry> libraryForPrompt(const Library& lib, const AiConfig& cfg) {
    std::vector<LibraryEntry> out;
    std::set<std::string> seen;
    for (auto it = lib.models.rbegin(); it != lib.models.rend() && out.size() < 40; ++it) {
        if (!it->is_object()) continue;
        const std::string prompt = it->value("prompt", std::string());
        const std::string file = it->value("file", std::string());
        std::error_code ec;
        if (prompt.empty() || seen.count(prompt) ||
            !fs::exists(joinPath(cfg.assetRoot, file), ec)) {
            continue;
        }
        // 指示文の中で引用符を壊さない。
        if (prompt.find('"') != std::string::npos) continue;
        seen.insert(prompt);
        out.push_back({prompt, file});
    }
    return out;
}

std::string cacheKey(const AiConfig& cfg, const std::string& prompt) {
    return hashHex(prompt + "|" + cfg.meshyArtStyle + "|" + cfg.meshyAiModel + "|" +
                   (cfg.meshyRefine ? "refine" : "preview") + "|" +
                   std::to_string(cfg.meshyTargetPolycount));
}

// ファイル名: 依頼文の頭を英数字に寄せたもの + キーの先頭 8 桁。
std::string cacheFileName(const AiConfig& cfg, const std::string& prompt,
                          const std::string& key) {
    std::string slug;
    for (const char c : prompt) {
        if (slug.size() >= 32) break;
        slug.push_back(c == ' ' ? '_' : c);
    }
    slug = sanitizeSafeName(slug);
    if (slug.empty()) slug = "model";
    return cfg.generatedDir + "/" + slug + "_" + key.substr(0, 8) + ".glb";
}

// ---- 文書（DOM）の手入れ -----------------------------------------------------

struct MeshReq {
    std::size_t assetIndex = 0;  // root.children() の <asset>
    std::size_t meshIndex = 0;   // その中の <mesh>
    std::string name;
    std::string prompt;
    double height = 1.0;
    std::string key;
    std::string file;  // assets/ 相対（成功したら）
    enum class State { Pending, Preview, Refine, Done, Failed } state = State::Pending;
    std::string taskId;
    std::string error;
    int progress = 0;
    bool cached = false;
    Clock::time_point started{};
};

std::vector<MeshReq> collectMeshRequests(xml::Element& root) {
    std::vector<MeshReq> out;
    auto& kids = root.children();
    for (std::size_t a = 0; a < kids.size(); ++a) {
        if (kids[a].name() != "asset") continue;
        auto& meshes = kids[a].children();
        for (std::size_t m = 0; m < meshes.size(); ++m) {
            if (meshes[m].name() != "mesh" || !meshes[m].has("generate")) continue;
            MeshReq r;
            r.assetIndex = a;
            r.meshIndex = m;
            r.name = meshes[m].attr("name");
            r.prompt = meshes[m].attr("generate");
            r.height = meshes[m].number("height", 1.0);
            if (!(r.height > 0.01) || r.height > 200.0) r.height = 1.0;
            out.push_back(std::move(r));
        }
    }
    return out;
}

xml::Element& meshElement(xml::Element& root, const MeshReq& r) {
    return root.children()[r.assetIndex].children()[r.meshIndex];
}

// <worldbody> 直下の <body> の <geom type="mesh" mesh=name> 全部に f を当てる。
template <typename Fn>
void forEachMeshGeom(xml::Element& root, const std::string& name, Fn&& f) {
    for (auto& wb : root.children()) {
        if (wb.name() != "worldbody") continue;
        for (auto& body : wb.children()) {
            if (body.name() != "body") continue;
            for (auto& g : body.children()) {
                if (g.name() == "geom" && g.attr("type") == "mesh" &&
                    g.attr("mesh") == name) {
                    f(g);
                }
            }
        }
    }
}

xml::Element& assetElement(xml::Element& root) {
    for (auto& c : root.children()) {
        if (c.name() == "asset") return c;
    }
    // <asset> は先頭近くに置く（<option> / <visual> の後）。
    auto& kids = root.children();
    std::size_t at = 0;
    while (at < kids.size() && (kids[at].name() == "option" || kids[at].name() == "visual"))
        ++at;
    kids.insert(kids.begin() + std::ptrdiff_t(at), xml::Element("asset"));
    return kids[at];
}

// 追加モード: 新しい文書が宣言せずに参照している既存のメッシュを宣言に
// 足す（足さないと文書の検査が「宣言が無い」と警告し、LLM に直させる羽目に
// なる。appendDocument は同じ名前のメッシュを二重に足さない）。
void injectExistingMeshes(xml::Element& root, const std::vector<ed::MeshAssetDesc>& have) {
    std::set<std::string> declared, used;
    for (const auto& c : root.children()) {
        if (c.name() != "asset") continue;
        for (const auto& m : c.children())
            if (m.name() == "mesh") declared.insert(m.attr("name"));
    }
    for (const auto& wb : root.children()) {
        if (wb.name() != "worldbody") continue;
        for (const auto& body : wb.children()) {
            for (const auto& g : body.children()) {
                if (g.name() == "geom" && g.attr("type") == "mesh") used.insert(g.attr("mesh"));
            }
        }
    }
    for (const auto& m : have) {
        if (!used.count(m.name) || declared.count(m.name)) continue;
        xml::Element e("mesh");
        e.set("name", m.name);
        e.set("file", m.file);
        e.setNumber("scale", m.scale, 6);
        assetElement(root).append(std::move(e));
    }
}

// 追加モード: 既存と同じ名前の新しいメッシュを改名する（appendDocument は
// 同名を足さないので、新しい物が古いモデルで描かれてしまう）。
void renameCollidingMeshes(xml::Element& root, std::vector<MeshReq>& reqs,
                           const std::vector<ed::MeshAssetDesc>& have) {
    std::set<std::string> taken;
    for (const auto& m : have) taken.insert(m.name);
    for (auto& r : reqs) {
        if (!taken.count(r.name)) {
            taken.insert(r.name);
            continue;
        }
        std::string fresh;
        for (int n = 2; n < 1000; ++n) {
            fresh = r.name + "_" + std::to_string(n);
            if (!taken.count(fresh)) break;
        }
        forEachMeshGeom(root, r.name, [&](xml::Element& g) { g.set("mesh", fresh); });
        meshElement(root, r).set("name", fresh);
        r.name = fresh;
        taken.insert(fresh);
    }
}

// 検査（LLM に直させる材料）。生成待ちのメッシュに仮のファイル名を入れた
// 写しを文書として読み、警告と自前の決まりを集める（英語 = LLM 向け）。
std::vector<std::string> validate(const xml::Element& root, bool meshAllowed, int maxMeshes) {
    std::vector<std::string> problems;
    if (root.name() != "wizengine") {
        problems.push_back("the root element must be <wizengine>, not <" + root.name() + ">");
        return problems;
    }
    xml::Element copy = root;
    int generated = 0;
    for (auto& c : copy.children()) {
        if (c.name() != "asset") continue;
        for (auto& m : c.children()) {
            if (m.name() != "mesh" || !m.has("generate")) continue;
            const std::string name = m.attr("name");
            if (!meshAllowed) {
                problems.push_back("mesh '" + name +
                                   "' uses generate= but model generation is not available; "
                                   "build it from boxes and spheres instead");
            } else if (m.attr("generate").empty()) {
                problems.push_back("mesh '" + name + "' has an empty generate= description");
            }
            ++generated;
            m.erase("generate");
            m.erase("height");
            m.set("file", "generated/pending.glb");
        }
    }
    if (meshAllowed && generated > maxMeshes) {
        problems.push_back("the document asks for " + std::to_string(generated) +
                           " generated meshes; the limit is " + std::to_string(maxMeshes));
    }
    std::vector<std::string> warnings;
    (void)ed::fromXml(copy, &warnings);
    problems.insert(problems.end(), warnings.begin(), warnings.end());
    return problems;
}

// ---- Meshy の待ち合わせ -------------------------------------------------------

// 全部の作成を先に投げ、まとめて待つ（1 個ずつ待つと N 倍かかる）。
void runMeshJobs(const AiConfig& cfg, std::vector<MeshReq*>& jobs, Library& lib,
                 const GenHooks& hooks) {
    using S = MeshReq::State;
    for (auto* r : jobs) {
        if (cancelled(hooks)) return;
        std::string err;
        r->taskId = meshyCreatePreview(cfg, r->prompt, err);
        r->started = Clock::now();
        if (r->taskId.empty()) {
            r->state = S::Failed;
            r->error = err;
            LOGW("ai", "mesh '%s': cannot start text-to-3d: %s", r->name.c_str(), err.c_str());
            note(hooks, "モデル「" + r->name + "」: 生成を開始できません（" + err + "）");
        } else {
            r->state = S::Preview;
            LOGI("ai", "mesh '%s': text-to-3d task %s", r->name.c_str(), r->taskId.c_str());
            note(hooks, "モデル「" + r->name + "」: 生成を開始しました");
        }
    }

    const double weightPreview = cfg.meshyRefine ? 0.5 : 1.0;
    for (;;) {
        bool pending = false;
        double sum = 0.0;
        for (auto* r : jobs) {
            if (r->state == S::Preview || r->state == S::Refine) pending = true;
            if (r->state == S::Done || r->state == S::Failed) {
                sum += 1.0;
            } else if (r->state == S::Preview) {
                sum += weightPreview * r->progress / 100.0;
            } else if (r->state == S::Refine) {
                sum += weightPreview + (1.0 - weightPreview) * r->progress / 100.0;
            }
        }
        stage(hooks, "meshes", jobs.empty() ? 1.0 : sum / double(jobs.size()));
        if (!pending || cancelled(hooks)) return;
        sleepFor(cfg.meshyPollSec, hooks);
        if (cancelled(hooks)) return;

        for (auto* r : jobs) {
            if (r->state != S::Preview && r->state != S::Refine) continue;
            if (Clock::now() - r->started > std::chrono::seconds(cfg.meshyTimeoutSec)) {
                r->state = S::Failed;
                r->error = "timed out after " + std::to_string(cfg.meshyTimeoutSec) + " s";
                note(hooks, "モデル「" + r->name + "」: 時間切れ");
                continue;
            }
            const MeshyTask t = meshyGetTask(cfg, r->taskId);
            if (t.status.empty()) {
                // 一時的な通信の失敗はもう一度待つ（時間切れが上限）。
                LOGW("ai", "mesh '%s': poll failed: %s", r->name.c_str(), t.error.c_str());
                continue;
            }
            r->progress = std::clamp(t.progress, 0, 100);
            if (!t.done()) continue;
            if (t.status != "SUCCEEDED") {
                r->state = S::Failed;
                r->error = t.status + (t.error.empty() ? "" : ": " + t.error);
                LOGW("ai", "mesh '%s': %s", r->name.c_str(), r->error.c_str());
                note(hooks, "モデル「" + r->name + "」: 失敗（" + r->error + "）");
                continue;
            }
            if (r->state == S::Preview && cfg.meshyRefine) {
                std::string err;
                const std::string refineId = meshyCreateRefine(cfg, r->taskId, err);
                if (!refineId.empty()) {
                    r->taskId = refineId;
                    r->state = S::Refine;
                    r->progress = 0;
                    note(hooks, "モデル「" + r->name + "」: 形ができました。テクスチャ付けへ");
                    continue;
                }
                // テクスチャ付けに入れなければ、形だけ（preview）で使う。
                LOGW("ai", "mesh '%s': refine failed (%s) - using the preview mesh",
                     r->name.c_str(), err.c_str());
                note(hooks, "モデル「" + r->name + "」: テクスチャ付けに入れないので形だけ使います");
            }
            if (t.glbUrl.empty()) {
                r->state = S::Failed;
                r->error = "the task has no glb url";
                continue;
            }
            net::HttpOptions opt;
            opt.readTimeoutSec = 120;
            opt.caFile = cfg.caFile;
            const std::string file = cacheFileName(cfg, r->prompt, r->key);
            const auto dl = net::download(t.glbUrl, joinPath(cfg.assetRoot, file),
                                          cfg.meshyMaxDownloadBytes, opt, hooks.cancel);
            if (!dl.ok()) {
                r->state = S::Failed;
                r->error = dl.error.empty() ? "HTTP " + std::to_string(dl.status) : dl.error;
                LOGW("ai", "mesh '%s': download failed: %s", r->name.c_str(), r->error.c_str());
                note(hooks, "モデル「" + r->name + "」: ダウンロード失敗（" + r->error + "）");
                continue;
            }
            r->file = file;
            r->state = S::Done;
            // できた順にキャッシュへ（後で全体が失敗しても、払った分は残す）。
            nlohmann::json entry;
            entry["key"] = r->key;
            entry["prompt"] = r->prompt;
            entry["file"] = file;
            entry["created"] = (long long)std::time(nullptr);
            lib.models.push_back(entry);
            while (lib.models.size() > 500) lib.models.erase(lib.models.begin());
            saveLibrary(lib);
            LOGI("ai", "mesh '%s': saved %s", r->name.c_str(), file.c_str());
            note(hooks, "モデル「" + r->name + "」: できました（" + file + "）");
        }
    }
}

// 失敗したモデルの代役: その geom を箱にする（頼んだ高さ × geom の半径）。
void replaceWithBox(xml::Element& root, const MeshReq& r) {
    forEachMeshGeom(root, r.name, [&](xml::Element& g) {
        const double radius = std::max(0.02, g.number("size", r.height * 0.3));
        const double half[3] = {radius, r.height * 0.5, radius};
        g.set("type", "box");
        g.erase("mesh");
        g.erase("collision");
        g.setNumbers("size", half, 3, 6);
        if (!g.has("rgba")) g.set("rgba", "0.6 0.6 0.62 1");
    });
}

MeshExtent extentOf(const Bounds& b, double scale) {
    MeshExtent e;
    for (int a = 0; a < 3; ++a) e.half[a] = std::max(1e-3, b.size(a) * 0.5 * scale);
    return e;
}

}  // namespace

std::string hashHex(const std::string& text) {
    std::uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

GenOutcome generateScene(const AiConfig& cfg, const GenRequest& req, const GenHooks& hooks) {
    GenOutcome out;
    out.mode = req.mode;
    auto fail = [&](std::string why) {
        out.ok = false;
        out.error = std::move(why);
        return out;
    };

    if (!cfg.llmConfigured()) {
        return fail("LLM が設定されていません（config.json の ai.llm.model と ai.llm.url）");
    }
    if (req.prompt.empty()) return fail("依頼文が空です");
    if (req.mode != GenMode::New && req.currentXml.size() > cfg.llmMaxSceneBytes) {
        return fail("今のシーンが大きすぎて LLM に渡せません（" +
                    std::to_string(req.currentXml.size() / 1024) + " KB > " +
                    std::to_string(cfg.llmMaxSceneBytes / 1024) +
                    " KB）。「新規」で作ってください");
    }

    // 今のシーン（編集 / 追加のとき）。
    ed::SceneDocument current;
    if (req.mode != GenMode::New) {
        std::string err;
        if (!ed::parseXml(req.currentXml, current, err)) {
            return fail("今のシーンの XML が読めません: " + err);
        }
    }

    const bool meshAllowed = req.allowMeshes && cfg.meshyConfigured() && cfg.maxMeshes > 0;
    Library lib = loadLibrary(cfg);

    PromptOptions po;
    po.meshGeneration = meshAllowed;
    po.maxMeshes = cfg.maxMeshes;
    if (meshAllowed) po.library = libraryForPrompt(lib, cfg);

    std::vector<ChatMessage> messages;
    messages.push_back({"system", buildSystemPrompt(po)});
    messages.push_back({"user", buildUserPrompt(req.mode, req.prompt, req.currentXml)});

    // ---- 1. LLM（壊れていれば直させる）----------------------------------
    xml::Element root;
    std::vector<std::string> lastProblems;
    bool accepted = false;
    long long tokensIn = 0, tokensOut = 0;
    for (int attempt = 0; attempt <= cfg.llmRepairs && !accepted; ++attempt) {
        if (cancelled(hooks)) return fail("キャンセルしました");
        stage(hooks, "llm", 0.1 + 0.25 * attempt);
        note(hooks, attempt == 0 ? "LLM に問い合わせています（" + cfg.llmModel + "）..."
                                 : "LLM に修正を依頼しています（" +
                                       std::to_string(attempt) + " 回目）...");
        LOGI("ai", "chat/completions model=%s attempt=%d", cfg.llmModel.c_str(), attempt + 1);
        const auto t0 = Clock::now();
        const ChatResult res = chatComplete(cfg, messages);
        if (cancelled(hooks)) return fail("キャンセルしました");
        if (!res.ok) {
            LOGW("ai", "chat/completions failed: %s", res.error.c_str());
            return fail("LLM の呼び出しに失敗しました: " + res.error);
        }
        tokensIn += res.promptTokens;
        tokensOut += res.completionTokens;
        const double secs =
            std::chrono::duration<double>(Clock::now() - t0).count();
        LOGI("ai", "reply %zu chars in %.1f s (tokens in %lld / out %lld)",
             res.content.size(), secs, res.promptTokens, res.completionTokens);
        note(hooks, "LLM の応答: " + std::to_string(res.content.size()) + " 文字 / " +
                        fmt(secs, "%.1f") + " 秒");

        std::string xmlText, err;
        lastProblems.clear();
        if (!extractSceneXml(res.content, xmlText, err)) {
            lastProblems.push_back(err);
        } else if (!xml::parse(xmlText, root, err)) {
            lastProblems.push_back("XML syntax error: " + err);
        } else {
            if (req.mode == GenMode::Append) injectExistingMeshes(root, current.meshes);
            lastProblems = validate(root, meshAllowed, cfg.maxMeshes);
            if (lastProblems.empty()) accepted = true;
        }
        for (const auto& p : lastProblems) LOGW("ai", "reply problem: %s", p.c_str());
        if (accepted) break;
        if (attempt == cfg.llmRepairs) {
            // 直しきれなかった。XML として読めていれば警告付きで使う
            // （文書層は既定値で補って必ず開ける）。読めなければ失敗。
            if (root.name() == "wizengine" && !xmlText.empty() &&
                xml::parse(xmlText, root, err)) {
                if (req.mode == GenMode::Append) injectExistingMeshes(root, current.meshes);
                note(hooks, "警告が残ったまま使います（" +
                                std::to_string(lastProblems.size()) + " 件）");
                out.warnings += int(lastProblems.size());
                accepted = true;
                break;
            }
            return fail("LLM が読める XML を返しませんでした: " +
                        (lastProblems.empty() ? std::string("?") : lastProblems.front()));
        }
        messages.push_back({"assistant", res.content});
        messages.push_back(
            {"user", repairPrompt("Your document has these problems:", lastProblems)});
        note(hooks, "応答に問題があります（" + std::to_string(lastProblems.size()) +
                        " 件）。直させます");
    }
    LOGI("ai", "LLM total tokens in %lld / out %lld", tokensIn, tokensOut);

    // ---- 2. モデルの生成 -------------------------------------------------
    std::vector<MeshReq> reqs = collectMeshRequests(root);
    if (req.mode == GenMode::Append) renameCollidingMeshes(root, reqs, current.meshes);
    if (!meshAllowed) {
        // 検査で直させたが残っていた（直しきれずに受け入れた）ぶん。
        for (auto& r : reqs) {
            r.state = MeshReq::State::Failed;
            r.error = "model generation is disabled";
        }
    }
    std::vector<MeshReq*> jobs;
    int newCount = 0;
    for (auto& r : reqs) {
        if (r.state == MeshReq::State::Failed) continue;
        r.key = cacheKey(cfg, r.prompt);
        const std::string hit = libraryFile(lib, cfg, r.key);
        if (!hit.empty()) {
            r.file = hit;
            r.cached = true;
            r.state = MeshReq::State::Done;
            note(hooks, "モデル「" + r.name + "」: キャッシュを使います（" + hit + "）");
            continue;
        }
        if (newCount >= cfg.maxMeshes) {
            r.state = MeshReq::State::Failed;
            r.error = "over the per-run limit";
            continue;
        }
        ++newCount;
        jobs.push_back(&r);
    }
    if (!jobs.empty()) {
        stage(hooks, "meshes", 0.0);
        note(hooks, std::to_string(jobs.size()) +
                        " 個のモデルを生成します（数分かかります）");
        runMeshJobs(cfg, jobs, lib, hooks);
        if (cancelled(hooks)) {
            return fail("キャンセルしました（Meshy 側で走り始めたタスクは止まりません）");
        }
    }

    // 結果を文書へ: 成功 = file と倍率、失敗 = 宣言を消して geom を箱に。
    MeshExtents extents;
    std::vector<const MeshReq*> failed;
    for (auto& r : reqs) {
        if (r.state != MeshReq::State::Done) {
            failed.push_back(&r);
            if (!r.error.empty()) {
                note(hooks, "モデル「" + r.name + "」は箱で代用します（" + r.error + "）");
            }
            continue;
        }
        std::string err;
        const Bounds b = gltfBounds(joinPath(cfg.assetRoot, r.file), &err);
        double scale = 1.0;
        if (b.valid) {
            const double h = b.size(1) > 1e-6
                                 ? b.size(1)
                                 : std::max({b.size(0), b.size(1), b.size(2), 1e-6});
            scale = r.height / h;
            extents[r.name] = extentOf(b, scale);
        } else {
            LOGW("ai", "mesh '%s': cannot measure %s (%s) - scale 1", r.name.c_str(),
                 r.file.c_str(), err.c_str());
        }
        xml::Element& m = meshElement(root, r);
        m.erase("generate");
        m.erase("height");
        m.set("file", r.file);
        m.setNumber("scale", scale, 6);
        if (r.cached) ++out.meshesCached; else ++out.meshesGenerated;
    }
    for (const auto* r : failed) replaceWithBox(root, *r);
    // 宣言の削除は番号がずれるので後ろから。
    std::sort(failed.begin(), failed.end(), [](const MeshReq* a, const MeshReq* b) {
        return a->assetIndex != b->assetIndex ? a->assetIndex > b->assetIndex
                                              : a->meshIndex > b->meshIndex;
    });
    for (const auto* r : failed) {
        auto& kids = root.children()[r->assetIndex].children();
        kids.erase(kids.begin() + std::ptrdiff_t(r->meshIndex));
    }
    out.meshesFailed = int(failed.size());

    // ---- 3. 文書に直して配置の後始末 -------------------------------------
    stage(hooks, "layout", 0.5);
    std::vector<std::string> warnings;
    ed::SceneDocument doc = ed::fromXml(root, &warnings);
    for (const auto& w : warnings) LOGW("ai", "document: %s", w.c_str());

    // 既存のモデル（生成したもの以外）も、ファイルが読めれば大きさを測る。
    auto measure = [&](const std::vector<ed::MeshAssetDesc>& list) {
        for (const auto& m : list) {
            if (extents.count(m.name) || m.file.empty()) continue;
            const Bounds b = gltfBounds(joinPath(cfg.assetRoot, m.file));
            if (b.valid) extents[m.name] = extentOf(b, m.scale);
        }
    };
    measure(doc.meshes);
    measure(current.meshes);

    std::vector<bool> movable(doc.bodies.size(), true);
    std::vector<ed::BodyDesc> obstacles;
    if (req.mode == GenMode::Append) {
        obstacles = current.bodies;
    } else if (req.mode == GenMode::Edit) {
        // 編集モードは「動かした / 新しい物」だけを直す（触っていない物は
        // ユーザーが置いた場所のまま）。
        for (std::size_t i = 0; i < doc.bodies.size(); ++i) {
            const auto& b = doc.bodies[i];
            for (const auto& c : current.bodies) {
                if (c.name != b.name) continue;
                const bool same =
                    std::fabs(c.position.x - b.position.x) < 1e-3 &&
                    std::fabs(c.position.y - b.position.y) < 1e-3 &&
                    std::fabs(c.position.z - b.position.z) < 1e-3 &&
                    std::fabs(c.size.x - b.size.x) < 1e-3 &&
                    std::fabs(c.size.y - b.size.y) < 1e-3 &&
                    std::fabs(c.size.z - b.size.z) < 1e-3 && c.mesh == b.mesh;
                if (same) movable[i] = false;
                break;
            }
        }
    }
    if (req.mode == GenMode::Append) {
        // 追加モードの床と設定は今のシーンのもの（appendDocument は文書の
        // 設定を使わない）。
        doc.ground = current.ground;
        doc.hasTerrain = current.hasTerrain;
    }
    const LayoutReport lay = resolveOverlaps(doc, movable, obstacles, extents);
    for (const auto& n : lay.notes) LOGI("ai", "layout: %s", n.c_str());
    if (lay.lifted + lay.raisedToFloor > 0) {
        note(hooks, "重なっていた " + std::to_string(lay.lifted + lay.raisedToFloor) +
                        " 個を上へ載せ直しました");
    }

    out.xml = ed::toXmlText(doc);
    out.bodies = int(doc.bodies.size());
    out.warnings += int(warnings.size());
    out.ok = true;
    stage(hooks, "layout", 1.0);
    return out;
}

// ---- 実行係 ------------------------------------------------------------------

struct SceneGenerator::Shared {
    mutable std::mutex m;
    bool alive = true;   // false = shutdown 済み（apply を呼ばない）
    bool busy = false;
    std::atomic<bool> cancel{false};
    std::string stage;
    double progress = 0.0;
    std::deque<std::string> log;
    int seq = 0;         // ログが増えるたびに進む（ブラウザの描き直しの目印）
    bool lastOk = false;
    std::string lastMessage;
    std::string mode;

    void addLog(const std::string& line) {
        std::lock_guard<std::mutex> lk(m);
        log.push_back(line);
        while (log.size() > 60) log.pop_front();
        ++seq;
    }
};

SceneGenerator::SceneGenerator(AiConfig cfg)
    : cfg_(std::move(cfg)), st_(std::make_shared<Shared>()) {}

SceneGenerator::~SceneGenerator() { shutdown(); }

void SceneGenerator::shutdown() {
    std::lock_guard<std::mutex> lk(st_->m);
    st_->alive = false;
    st_->cancel.store(true);
}

void SceneGenerator::cancel() {
    std::lock_guard<std::mutex> lk(st_->m);
    if (!st_->busy) return;
    st_->cancel.store(true);
    st_->stage = "cancelling";
    st_->log.push_back("キャンセルしています（進行中の通信が終わるまで待ちます）...");
    ++st_->seq;
}

bool SceneGenerator::start(GenRequest req, ApplyFn apply, std::string& whyNot) {
    {
        std::lock_guard<std::mutex> lk(st_->m);
        if (!st_->alive) {
            whyNot = "終了処理中です";
            return false;
        }
        if (st_->busy) {
            whyNot = "前の生成がまだ終わっていません";
            return false;
        }
        st_->busy = true;
        st_->cancel.store(false);
        st_->stage = "llm";
        st_->progress = 0.0;
        st_->log.clear();
        st_->log.push_back("開始: " + std::string(genModeName(req.mode)) + " - " +
                           (req.prompt.size() > 120 ? req.prompt.substr(0, 120) + "..."
                                                    : req.prompt));
        st_->mode = genModeName(req.mode);
        ++st_->seq;
    }
    LOGI("ai", "scene generation started (mode %s)", genModeName(req.mode));
    // 切り離したスレッドが共有状態を持つ（shared_ptr）。通信の途中で
    // SceneGenerator が消えても、状態はスレッドが終わるまで生きている。
    std::shared_ptr<Shared> st = st_;
    AiConfig cfg = cfg_;
    std::thread([st, cfg, req = std::move(req), apply = std::move(apply)]() {
        GenHooks hooks;
        hooks.cancel = &st->cancel;
        hooks.stage = [st](const std::string& s, double p) {
            std::lock_guard<std::mutex> lk(st->m);
            if (st->stage != "cancelling") st->stage = s;
            // 段ごとの区間を全体の進みへ（LLM 0〜30%、モデル 30〜95%、後始末 95〜100%）。
            if (s == "llm") st->progress = 0.3 * p;
            else if (s == "meshes") st->progress = 0.3 + 0.65 * p;
            else st->progress = 0.95 + 0.05 * p;
        };
        hooks.note = [st](const std::string& line) { st->addLog(line); };

        GenOutcome res;
        try {
            res = generateScene(cfg, req, hooks);
        } catch (const std::exception& e) {
            res.ok = false;
            res.error = std::string("内部エラー: ") + e.what();
            LOGE("ai", "scene generation threw: %s", e.what());
        }

        std::lock_guard<std::mutex> lk(st->m);
        if (st->alive && res.ok && !st->cancel.load()) {
            apply(res);  // ロックの中 = shutdown と入れ違わない
            st->lastOk = true;
            st->lastMessage = "AI シーンを適用しました（物体 " + std::to_string(res.bodies) +
                              "、生成 " + std::to_string(res.meshesGenerated) +
                              "、キャッシュ " + std::to_string(res.meshesCached) +
                              "、代役 " + std::to_string(res.meshesFailed) +
                              (res.warnings ? "、警告 " + std::to_string(res.warnings) : "") +
                              "）";
        } else {
            st->lastOk = false;
            st->lastMessage = res.ok ? std::string("キャンセルしました") : res.error;
        }
        st->log.push_back(st->lastMessage);
        ++st->seq;
        st->busy = false;
        st->stage = st->lastOk ? "done" : "failed";
        st->progress = st->lastOk ? 1.0 : st->progress;
        if (st->alive) {
            LOGI("ai", "scene generation %s", st->lastOk ? "applied" : "ended without result");
        }
    }).detach();
    return true;
}

nlohmann::json SceneGenerator::statusJson() const {
    nlohmann::json j;
    const auto llm = net::parseUrl(cfg_.llmUrl);
    const auto meshy = net::parseUrl(cfg_.meshyUrl);
    const bool https = net::httpsSupported();
    const bool llmOk = cfg_.llmConfigured() && llm.valid && (llm.scheme == "http" || https);
    const bool meshyOk =
        cfg_.meshyConfigured() && meshy.valid && (meshy.scheme == "http" || https);
    j["llm"] = llmOk;
    j["llmModel"] = cfg_.llmModel;
    j["llmHost"] = llm.valid ? llm.host : std::string();
    j["meshy"] = meshyOk;
    j["meshyHost"] = meshy.valid ? meshy.host : std::string();
    j["meshyRefine"] = cfg_.meshyRefine;
    j["maxMeshes"] = cfg_.maxMeshes;
    j["https"] = https;
    std::lock_guard<std::mutex> lk(st_->m);
    j["busy"] = st_->busy;
    j["stage"] = st_->stage;
    j["progress"] = st_->progress;
    j["seq"] = st_->seq;
    j["log"] = nlohmann::json(std::vector<std::string>(st_->log.begin(), st_->log.end()));
    j["lastOk"] = st_->lastOk;
    j["message"] = st_->lastMessage;
    return j;
}

}  // namespace autogen
}  // namespace wizengine
