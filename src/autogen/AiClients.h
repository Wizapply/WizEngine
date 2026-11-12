#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "autogen/AiConfig.h"

// 外部 API の薄い口（同期・ワーカースレッド専用）。
//
//   chatComplete … OpenAI 互換の POST {llmUrl}/chat/completions。
//                  送るのは model と messages だけ（+ 指定時の temperature）。
//                  max_tokens / response_format は送らない - 提供元とモデルの
//                  世代で名前や可否が違い（max_completion_tokens 等）、送ると
//                  400 で落ちる組み合わせがあるため。
//   Meshy        … Meshy 互換の Text to 3D（v2）。
//                  作成 POST {meshyUrl}/openapi/v2/text-to-3d
//                       {mode:"preview", prompt, ...} / {mode:"refine", preview_task_id}
//                  取得 GET  {meshyUrl}/openapi/v2/text-to-3d/{id}
//                       → status（PENDING / IN_PROGRESS / SUCCEEDED / FAILED /
//                         CANCELED / EXPIRED）, progress, model_urls.glb, task_error
namespace wizengine {
namespace autogen {

struct ChatMessage {
    std::string role;  // "system" / "user" / "assistant"
    std::string content;
};

struct ChatResult {
    bool ok = false;
    std::string content;
    std::string error;  // 英語（ログに出る）
    long long promptTokens = 0;
    long long completionTokens = 0;
};

ChatResult chatComplete(const AiConfig& cfg, const std::vector<ChatMessage>& messages);

struct MeshyTask {
    std::string id;
    std::string status;   // 上の一覧。取得に失敗したら空
    int progress = 0;     // 0..100
    std::string glbUrl;   // SUCCEEDED のときだけ
    std::string error;    // 通信の失敗か task_error.message
    bool done() const {
        return status == "SUCCEEDED" || status == "FAILED" || status == "CANCELED" ||
               status == "EXPIRED";
    }
};

// 作成。成功で task id、失敗で空（error に理由）。
std::string meshyCreatePreview(const AiConfig& cfg, const std::string& prompt,
                               std::string& error);
std::string meshyCreateRefine(const AiConfig& cfg, const std::string& previewTaskId,
                              std::string& error);
MeshyTask meshyGetTask(const AiConfig& cfg, const std::string& id);

// HTTP の応答本文からエラー文を拾う（{"error":{"message"}} / {"message"} /
// {"error":"..."}）。拾えなければ本文の先頭 200 文字。
std::string apiErrorText(int status, const std::string& body);

}  // namespace autogen
}  // namespace wizengine
