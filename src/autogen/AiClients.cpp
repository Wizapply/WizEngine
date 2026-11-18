#include "autogen/AiClients.h"

#include <nlohmann/json.hpp>

#include "autogen/HttpClient.h"

namespace wizengine {
namespace autogen {

namespace {

net::HttpOptions llmOptions(const AiConfig& cfg) {
    net::HttpOptions o;
    o.readTimeoutSec = cfg.llmTimeoutSec;
    o.caFile = cfg.caFile;
    return o;
}

net::HttpOptions meshyOptions(const AiConfig& cfg) {
    net::HttpOptions o;
    o.readTimeoutSec = 60;
    o.caFile = cfg.caFile;
    return o;
}

net::Headers bearer(const std::string& key) {
    net::Headers h;
    if (!key.empty()) h.emplace_back("Authorization", "Bearer " + key);
    h.emplace_back("Accept", "application/json");
    return h;
}

std::string meshyEndpoint(const AiConfig& cfg) {
    return net::joinUrl(cfg.meshyUrl, "/openapi/v2/text-to-3d");
}

// content は文字列か、部品の配列（[{type:"text", text}]）で返る実装がある。
std::string contentText(const nlohmann::json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string out;
    if (content.is_array()) {
        for (const auto& part : content) {
            if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                out += part["text"].get<std::string>();
            }
        }
    }
    return out;
}

std::string createTask(const AiConfig& cfg, const nlohmann::json& body,
                       std::string& error) {
    const auto res = net::request("POST", meshyEndpoint(cfg), bearer(cfg.meshyKey),
                                  body.dump(), "application/json", meshyOptions(cfg));
    if (!res.error.empty()) {
        error = res.error;
        return {};
    }
    if (!res.ok()) {
        error = apiErrorText(res.status, res.body);
        return {};
    }
    const auto j = nlohmann::json::parse(res.body, nullptr, false);
    // v2 は {"result": "<id>"}。互換サーバーが {"id"} を返しても受ける。
    for (const char* key : {"result", "id"}) {
        if (j.is_object() && j.contains(key) && j[key].is_string()) {
            return j[key].get<std::string>();
        }
    }
    error = "unexpected response from text-to-3d: " + res.body.substr(0, 200);
    return {};
}

}  // namespace

std::string apiErrorText(int status, const std::string& body) {
    std::string msg;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_object()) {
        if (j.contains("error")) {
            const auto& e = j["error"];
            if (e.is_object() && e.contains("message") && e["message"].is_string()) {
                msg = e["message"].get<std::string>();
            } else if (e.is_string()) {
                msg = e.get<std::string>();
            }
        }
        if (msg.empty() && j.contains("message") && j["message"].is_string()) {
            msg = j["message"].get<std::string>();
        }
    }
    if (msg.empty()) msg = body.substr(0, 200);
    return "HTTP " + std::to_string(status) + (msg.empty() ? "" : ": " + msg);
}

ChatResult chatComplete(const AiConfig& cfg, const std::vector<ChatMessage>& messages) {
    ChatResult out;
    nlohmann::json body;
    body["model"] = cfg.llmModel;
    body["stream"] = false;
    if (cfg.llmTemperature >= 0.0) body["temperature"] = cfg.llmTemperature;
    for (const auto& m : messages) {
        body["messages"].push_back({{"role", m.role}, {"content", m.content}});
    }
    const auto res = net::request("POST", net::joinUrl(cfg.llmUrl, "/chat/completions"),
                                  bearer(cfg.llmKey), body.dump(), "application/json",
                                  llmOptions(cfg));
    if (!res.error.empty()) {
        out.error = res.error;
        return out;
    }
    if (!res.ok()) {
        out.error = apiErrorText(res.status, res.body);
        return out;
    }
    const auto j = nlohmann::json::parse(res.body, nullptr, false);
    if (!j.is_object() || !j.contains("choices") || !j["choices"].is_array() ||
        j["choices"].empty()) {
        out.error = "unexpected response from chat/completions: " + res.body.substr(0, 200);
        return out;
    }
    const auto& choice = j["choices"][0];
    if (choice.contains("message") && choice["message"].is_object() &&
        choice["message"].contains("content")) {
        out.content = contentText(choice["message"]["content"]);
    }
    if (j.contains("usage") && j["usage"].is_object()) {
        const auto& u = j["usage"];
        if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number())
            out.promptTokens = u["prompt_tokens"].get<long long>();
        if (u.contains("completion_tokens") && u["completion_tokens"].is_number())
            out.completionTokens = u["completion_tokens"].get<long long>();
    }
    if (out.content.empty()) {
        const std::string reason =
            choice.contains("finish_reason") && choice["finish_reason"].is_string()
                ? choice["finish_reason"].get<std::string>()
                : std::string("unknown");
        out.error = "the model returned no text (finish_reason " + reason + ")";
        return out;
    }
    out.ok = true;
    return out;
}

std::string meshyCreatePreview(const AiConfig& cfg, const std::string& prompt,
                               std::string& error) {
    nlohmann::json body;
    body["mode"] = "preview";
    // Meshy の prompt は 600 文字まで。超えると 400 になるので切る。
    body["prompt"] = prompt.size() > 600 ? prompt.substr(0, 600) : prompt;
    if (!cfg.meshyArtStyle.empty()) body["art_style"] = cfg.meshyArtStyle;
    if (!cfg.meshyAiModel.empty()) body["ai_model"] = cfg.meshyAiModel;
    if (cfg.meshyTargetPolycount > 0) {
        body["should_remesh"] = true;
        body["target_polycount"] = cfg.meshyTargetPolycount;
    }
    return createTask(cfg, body, error);
}

std::string meshyCreateRefine(const AiConfig& cfg, const std::string& previewTaskId,
                              std::string& error) {
    nlohmann::json body;
    body["mode"] = "refine";
    body["preview_task_id"] = previewTaskId;
    body["enable_pbr"] = true;
    return createTask(cfg, body, error);
}

MeshyTask meshyGetTask(const AiConfig& cfg, const std::string& id) {
    MeshyTask t;
    t.id = id;
    const auto res = net::request("GET", meshyEndpoint(cfg) + "/" + id,
                                  bearer(cfg.meshyKey), {}, {}, meshyOptions(cfg));
    if (!res.error.empty()) {
        t.error = res.error;
        return t;
    }
    if (!res.ok()) {
        t.error = apiErrorText(res.status, res.body);
        return t;
    }
    const auto j = nlohmann::json::parse(res.body, nullptr, false);
    if (!j.is_object()) {
        t.error = "unexpected task response: " + res.body.substr(0, 200);
        return t;
    }
    if (j.contains("status") && j["status"].is_string()) t.status = j["status"].get<std::string>();
    if (j.contains("progress") && j["progress"].is_number())
        t.progress = int(j["progress"].get<double>());
    if (j.contains("model_urls") && j["model_urls"].is_object()) {
        const auto& u = j["model_urls"];
        if (u.contains("glb") && u["glb"].is_string()) t.glbUrl = u["glb"].get<std::string>();
    }
    if (j.contains("task_error") && j["task_error"].is_object()) {
        const auto& e = j["task_error"];
        if (e.contains("message") && e["message"].is_string())
            t.error = e["message"].get<std::string>();
    }
    if (t.status.empty()) t.error = "task response has no status";
    return t;
}

}  // namespace autogen
}  // namespace wizengine
