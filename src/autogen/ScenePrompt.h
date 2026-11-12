#pragma once

#include <string>
#include <vector>

// LLM に渡す指示文と、返ってきた文章から XML を取り出す処理。
//
// 指示文は**英語**（モデルが最も素直に従う。ユーザーの依頼文は日本語のままで
// よい）。教えるのはシーン文書（SceneDocument.h 冒頭）の**部分集合**だけ:
// 箱・球・生成モデルの剛体、ライト、カメラ、地面、環境光、描画、基本の
// ジョイント。車両・テレイン・ソフトボディ・FEA・イベントのノードは
// 教えない - 書かせても数値の整合（パワートレイン・格子ファイル・ノード id）
// を LLM が取れず、黙って動かないシーンになりやすいため（編集モードでは
// 既存のものを「そのまま残せ」とだけ言う）。
namespace wizengine {
namespace autogen {

enum class GenMode {
    New,     // 新しいシーンを作って置き換える
    Edit,    // 今のシーンを渡して、直した全文で置き換える
    Append,  // 今のシーンを見せて、足す物だけを書かせる（appendDocument）
};

const char* genModeName(GenMode m);
GenMode genModeFromName(const std::string& s, GenMode fallback);

// 以前に生成したモデル（キャッシュ）。同じ prompt を書けば再生成しない。
struct LibraryEntry {
    std::string prompt;
    std::string file;
};

struct PromptOptions {
    bool meshGeneration = false;  // <mesh generate> を使ってよいか
    int maxMeshes = 4;
    std::vector<LibraryEntry> library;  // 最近のものから最大 40 件
};

std::string buildSystemPrompt(const PromptOptions& opt);

// mode と依頼文と（Edit / Append のとき）今のシーン XML から、最初の
// user メッセージを作る。
std::string buildUserPrompt(GenMode mode, const std::string& request,
                            const std::string& currentXml);

// 応答から <wizengine ...>...</wizengine> を取り出す。``` で囲まれていても、
// 推論系モデルの <think> の中に下書きがあっても、**最後の**文書を採る。
// 見つからなければ false（error に英語の理由）。
bool extractSceneXml(const std::string& reply, std::string& xml, std::string& error);

// 直させるときの user メッセージ（壊れた XML / 警告の一覧）。
std::string repairPrompt(const std::string& problem, const std::vector<std::string>& items);

}  // namespace autogen
}  // namespace wizengine
