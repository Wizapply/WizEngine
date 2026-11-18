#include "components/AutoGenComponent.h"

#include <string>
#include <utility>

#include "core/Log.h"
#include "scene/EditorState.h"
#include "scene/Scene.h"

namespace ag = wizengine::autogen;

namespace {

// 依頼文の上限（文字数ではなくバイト。日本語で 1300 字ほど）。LLM への
// 1 往復で済む長さに抑える。
constexpr std::size_t kMaxPromptBytes = 4000;

}  // namespace

AutoGenComponent::AutoGenComponent(ag::AiConfig cfg) : generator_(std::move(cfg)) {
    const auto& c = generator_.config();
    if (c.llmConfigured()) {
        LOGI("ai", "scene generation: LLM %s (%s), text-to-3d %s",
             c.llmModel.c_str(), c.llmUrl.c_str(),
             c.meshyConfigured() ? c.meshyUrl.c_str() : "off (no MESHY key)");
    } else {
        LOGI("ai", "scene generation: off (set ai.llm.model in config.json to enable)");
    }
}

bool AutoGenComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    const std::string cmd = msg.value("cmd", "");
    if (cmd != "ai.generate" && cmd != "ai.cancel") return false;
    EditorState& state = scene.editor();
    // シーンを書き換える操作 = エディタモード × エディタカメラのページだけ。
    if (camIndex != scene.editorCamera() || !state.isEditor()) {
        state.setStatus("AI シーン生成はエディタモードの Editor Camera のページで使えます");
        return true;
    }

    if (cmd == "ai.cancel") {
        generator_.cancel();
        return true;
    }

    ag::GenRequest req;
    req.prompt = (msg.contains("prompt") && msg["prompt"].is_string())
                     ? msg["prompt"].get<std::string>()
                     : std::string();
    if (req.prompt.size() > kMaxPromptBytes) req.prompt.resize(kMaxPromptBytes);
    req.mode = ag::genModeFromName(
        (msg.contains("mode") && msg["mode"].is_string()) ? msg["mode"].get<std::string>()
                                                          : std::string(),
        ag::GenMode::New);
    req.allowMeshes = !(msg.contains("meshes") && msg["meshes"].is_boolean() &&
                        !msg["meshes"].get<bool>());
    if (req.prompt.empty()) {
        state.setStatus("AI: 依頼文が空です");
        return true;
    }
    // 今のシーン（INPUT スレッドで写す。documentXml は自分でロックを取る）。
    if (req.mode != ag::GenMode::New) req.currentXml = scene.documentXml();

    const std::size_t editorCam = scene.editorCamera();
    std::string whyNot;
    const bool started = generator_.start(
        std::move(req),
        // ワーカースレッドから。物理スレッドへ Op を積むだけ（EditorState の
        // キューはスレッド安全）。適用の結果（警告の件数）は物理スレッドが
        // ステータスに出す。
        [&state, editorCam](const ag::GenOutcome& res) {
            EditorState::Op op;
            op.kind = res.mode == ag::GenMode::Append ? "xml.append" : "xml";
            op.args = {{"text", res.xml}};
            op.camera = editorCam;
            state.push(std::move(op));
        },
        whyNot);
    state.setStatus(started ? "AI: シーンを生成しています..." : "AI: " + whyNot);
    return true;
}
