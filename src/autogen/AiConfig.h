#pragma once

#include <cstddef>
#include <string>

// AI によるシーン自動構築（src/autogen/）の接続設定。
//
// どの API にどのモデルでつなぐかはシーンの中身ではなく**実行環境の設定**
// なので、起動設定 config.json の "ai" 節が持つ（app/AppConfig が読んで
// ここへ詰める。キーの一覧と既定値は同梱の config.json）:
//
//   "ai": {
//     "llm":   { "url", "model", "apiKey", "temperature", "timeoutSec",
//                "repairs", "maxSceneKB" },      ... OpenAI 互換 Chat Completions
//                                                   （OpenAI / Ollama / LM Studio /
//                                                   vLLM / llama.cpp server など）
//     "meshy": { "url", "apiKey", "refine", "artStyle", "aiModel",
//                "targetPolycount", "pollSec", "timeoutSec", "maxDownloadMB" },
//     "maxMeshes", "caFile", "generatedDir"
//   }
//
// **API キー**は config.json の apiKey に書くか、空のままにして環境変数で
// 渡す（WIZ_LLM_API_KEY → OPENAI_API_KEY、WIZ_MESHY_API_KEY → MESHY_API_KEY の
// 順。config.json に書いた値が優先）。キーを書いた config.json はリポジトリに
// 入れないこと。ブラウザには送らない（/scene に出すのは「設定済みか」だけ）。
namespace wizengine {
namespace autogen {

struct AiConfig {
    // ---- LLM（OpenAI 互換）----
    std::string llmUrl = "https://api.openai.com/v1";
    std::string llmModel;
    std::string llmKey;
    // 負 = 送らない（推論系のモデルは temperature を 1 以外にすると 400 を返す
    // ため、既定は送らない）。
    double llmTemperature = -1.0;
    int llmTimeoutSec = 300;  // 推論系は 1 回の応答に数分かかる
    // XML が壊れていた・警告が出たときに LLM へ直させる回数（往復の上限）。
    int llmRepairs = 2;
    // 編集 / 追加モードで LLM に見せる今のシーン XML の上限（バイト）。超えたら
    // 「大きすぎる」と断る（途中で切った XML を渡すと、壊れた文書が返ってくる）。
    std::size_t llmMaxSceneBytes = 96 * 1024;

    // ---- Meshy 互換（Text to 3D）----
    std::string meshyUrl = "https://api.meshy.ai";
    std::string meshyKey;
    bool meshyRefine = false;       // true = テクスチャ付けまで（時間と費用が倍以上）
    std::string meshyArtStyle = "realistic";  // 空 = 送らない
    std::string meshyAiModel;        // 空 = 送らない（提供元の既定）
    int meshyTargetPolycount = 0;    // 0 = 送らない
    int meshyPollSec = 5;
    int meshyTimeoutSec = 900;       // 1 モデルの待ち時間の上限（preview + refine）
    std::size_t meshyMaxDownloadBytes = 128u * 1024u * 1024u;

    // ---- 共通 ----
    int maxMeshes = 4;               // 1 回の生成で新しく作るモデルの上限
    std::string caFile;              // 空 = OS / OpenSSL の既定
    // 実行時ファイルのルート（core/AssetError.h の kAssetRoot。テストが
    // 一時フォルダへ向ける）。
    std::string assetRoot = "assets";
    // 生成したモデルの置き場（assets/ 相対）。シーン文書の <mesh file> が
    // assets/ 相対しか受けないので、必ずその中。
    std::string generatedDir = "generated";

    bool llmConfigured() const { return !llmUrl.empty() && !llmModel.empty(); }
    bool meshyConfigured() const { return !meshyUrl.empty() && !meshyKey.empty(); }
};

// config.json に API キーが無いとき、環境変数から補う（上の順）。
void fillAiKeysFromEnvironment(AiConfig& cfg);

}  // namespace autogen
}  // namespace wizengine
