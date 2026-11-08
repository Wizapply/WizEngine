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

}  // namespace

void fillAiKeysFromEnvironment(AiConfig& cfg) {
    if (cfg.llmKey.empty()) cfg.llmKey = envFirst("WIZ_LLM_API_KEY", "OPENAI_API_KEY");
    if (cfg.meshyKey.empty()) cfg.meshyKey = envFirst("WIZ_MESHY_API_KEY", "MESHY_API_KEY");
}

}  // namespace autogen
}  // namespace wizengine
