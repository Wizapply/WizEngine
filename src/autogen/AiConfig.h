#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>

// AI によるシーン自動構築（src/autogen/）の接続設定。
//
// 置き場所の考え方は CPU コアの指定と同じ: どの API にどのモデルでつなぐかは
// シーンの中身ではなく**実行環境の設定**なので、SceneConfig.h にもシーン文書
// にも置かず、exe 引数と環境変数で渡す。
//
//   LLM（OpenAI 互換の Chat Completions。OpenAI / Azure 互換ゲートウェイ /
//   Ollama / LM Studio / vLLM / llama.cpp server など）
//     --llm-url URL      WIZ_LLM_URL      既定 https://api.openai.com/v1
//                                         （/chat/completions はこちらが足す）
//     --llm-model NAME   WIZ_LLM_MODEL    必須（既定なし - 提供元ごとに違うため）
//     （キー）           WIZ_LLM_API_KEY → OPENAI_API_KEY の順。ローカルの
//                                         サーバーなら無くてよい
//   3D モデル生成（Meshy 互換の Text to 3D）
//     --meshy-url URL    WIZ_MESHY_URL    既定 https://api.meshy.ai
//                                         （/openapi/v2/text-to-3d はこちらが足す）
//     --meshy-refine     WIZ_MESHY_REFINE=1  テクスチャ付け（refine）まで回す
//     （キー）           WIZ_MESHY_API_KEY → MESHY_API_KEY の順。無ければ
//                                         モデル生成は使わない（箱・球で組む）
//   共通
//     --ai-max-meshes N  WIZ_AI_MAX_MESHES  1 回の生成で作るモデルの上限（既定 4）
//     （CA）             WIZ_AI_CA_FILE   HTTPS の検証に使う CA 束（社内プロキシ等）
//
// **API キーは exe 引数では受けない**（プロセス一覧やシェルの履歴に残るため）。
// 環境変数だけ。ブラウザにも送らない（/scene に出すのは「設定済みか」だけ）。
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

// 環境変数から読む（exe 引数はこのあと applyAiArg で上書きする）。
AiConfig aiConfigFromEnvironment();

// exe 引数 1 個を解釈する。自分の引数なら true（値を取る引数は next() で
// 次の値をもらう）。知らない引数は false を返して main の解析に任せる。
template <typename NextFn>
bool applyAiArg(AiConfig& cfg, const std::string& arg, NextFn&& next) {
    if (arg == "--llm-url") {
        cfg.llmUrl = next("--llm-url");
    } else if (arg == "--llm-model") {
        cfg.llmModel = next("--llm-model");
    } else if (arg == "--meshy-url") {
        cfg.meshyUrl = next("--meshy-url");
    } else if (arg == "--meshy-refine") {
        cfg.meshyRefine = true;
    } else if (arg == "--ai-max-meshes") {
        const std::string v = next("--ai-max-meshes");
        if (!v.empty()) cfg.maxMeshes = std::atoi(v.c_str());
        if (cfg.maxMeshes < 0) cfg.maxMeshes = 0;
        if (cfg.maxMeshes > 16) cfg.maxMeshes = 16;
    } else {
        return false;
    }
    return true;
}

}  // namespace autogen
}  // namespace wizengine
