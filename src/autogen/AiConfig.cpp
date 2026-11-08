#include "autogen/AiConfig.h"

#include <cstdlib>

namespace wizengine {
namespace autogen {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

std::string envFirst(const char* a, const char* b) {
    std::string v = env(a);
    return v.empty() ? env(b) : v;
}

bool envTrue(const char* name) {
    const std::string v = env(name);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

}  // namespace

AiConfig aiConfigFromEnvironment() {
    AiConfig c;
    if (const auto v = env("WIZ_LLM_URL"); !v.empty()) c.llmUrl = v;
    c.llmModel = env("WIZ_LLM_MODEL");
    c.llmKey = envFirst("WIZ_LLM_API_KEY", "OPENAI_API_KEY");
    if (const auto v = env("WIZ_MESHY_URL"); !v.empty()) c.meshyUrl = v;
    c.meshyKey = envFirst("WIZ_MESHY_API_KEY", "MESHY_API_KEY");
    c.meshyRefine = envTrue("WIZ_MESHY_REFINE");
    if (const auto v = env("WIZ_AI_MAX_MESHES"); !v.empty()) {
        c.maxMeshes = std::atoi(v.c_str());
        if (c.maxMeshes < 0) c.maxMeshes = 0;
        if (c.maxMeshes > 16) c.maxMeshes = 16;
    }
    c.caFile = env("WIZ_AI_CA_FILE");
    return c;
}

}  // namespace autogen
}  // namespace wizengine
