#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "autogen/AiConfig.h"
#include "autogen/ScenePrompt.h"

// AI によるシーンの自動構築（パイプライン + ワーカースレッド）。
//
//   依頼文 ──► LLM（OpenAI 互換）── シーン文書の XML
//                │  壊れた XML・文書の警告は LLM に直させる（cfg.llmRepairs 回）
//                ▼
//   <mesh generate="..." height="..."> ──► Meshy 互換 Text to 3D ── .glb
//                │  作成を全部先に投げてから並行して待つ / 同じ依頼は
//                │  assets/generated/index.json のキャッシュから（再課金しない）
//                │  失敗したモデルは箱の代役に差し替える（シーンは必ず完成させる）
//                ▼
//   倍率（頼んだ高さ ÷ モデルの高さ）── 重なりの後始末（SceneLayout）
//                ▼
//   正規化した XML ──► apply（エンジン側: EditorState の Op で物理スレッドへ）
//
// **Chrono も Filament も知らない**（文書層と HTTP だけ）。エンジンとの接続は
// components/AutoGenComponent。ネットワークの待ちは分単位なので、どの
// エンジンのスレッドでも回さず、1 本の専用スレッドで 1 件ずつ回す。
namespace wizengine {
namespace autogen {

struct GenRequest {
    GenMode mode = GenMode::New;
    std::string prompt;
    bool allowMeshes = true;   // ブラウザのチェック（Meshy を使ってよいか）
    std::string currentXml;    // Edit / Append のとき今のシーン
};

struct GenOutcome {
    bool ok = false;
    GenMode mode = GenMode::New;
    std::string xml;            // 適用する文書（正規化済み）
    std::string error;          // 失敗の理由（日本語。ブラウザに出る）
    int bodies = 0;
    int meshesGenerated = 0;
    int meshesCached = 0;
    int meshesFailed = 0;
    int warnings = 0;
};

// パイプライン本体（同期）。テストはこれを直接呼ぶ。
struct GenHooks {
    // 段階と進み（0..1）。stage は "llm" / "meshes" / "layout"。
    std::function<void(const std::string& stage, double progress)> stage;
    // ブラウザのログ欄に出す 1 行（日本語）。コンソールには別に英語で出す。
    std::function<void(const std::string& line)> note;
    const std::atomic<bool>* cancel = nullptr;
};
GenOutcome generateScene(const AiConfig& cfg, const GenRequest& req, const GenHooks& hooks);

// FNV-1a 64bit の 16 進（キャッシュのキーとファイル名）。
std::string hashHex(const std::string& text);

// 1 件ずつ回す実行係。apply は**ワーカースレッドから**呼ばれる（成功時だけ）。
class SceneGenerator {
public:
    using ApplyFn = std::function<void(const GenOutcome&)>;

    explicit SceneGenerator(AiConfig cfg);
    ~SceneGenerator();  // shutdown() と同じ

    SceneGenerator(const SceneGenerator&) = delete;
    SceneGenerator& operator=(const SceneGenerator&) = delete;

    // 走っていれば false（whyNot に日本語の理由）。
    bool start(GenRequest req, ApplyFn apply, std::string& whyNot);
    void cancel();
    // 以後 apply を呼ばない。走っている通信は待たない（スレッドは切り離し
    // てあり、終わり次第自分で消える）- 終了時に LLM の応答を最大数分
    // 待たされないため。Scene より先に呼ぶ（apply が Scene を触るので）。
    void shutdown();

    const AiConfig& config() const { return cfg_; }
    // /scene の "ai"。キーは返さない（設定済みかどうかと接続先のホストだけ）。
    nlohmann::json statusJson() const;

private:
    struct Shared;
    AiConfig cfg_;
    std::shared_ptr<Shared> st_;
};

}  // namespace autogen
}  // namespace wizengine
