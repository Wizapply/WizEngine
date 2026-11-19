#pragma once

#include <string>
#include <vector>

// LLM に渡す指示文と、返ってきた文章から XML を取り出す処理。
//
// 指示文は**英語**（モデルが最も素直に従う。ユーザーの依頼文は日本語のままで
// よい）。構成は「エンジンが文書をどう使うか（エディタ → シミュレート、掴み、
// カメラ）」→ 座標と単位の約束 → 見本の文書（systemPromptExample）→ 要素ごとの
// 注意（読込が警告にする組み合わせを先に言う）→ 使えるファイルの一覧 →
// 書かない物 → 出す前の点検表、の順。教えるのはシーン文書（SceneDocument.h 冒頭）の**部分集合**だけ:
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

// assets/ に今あるファイルのうち、LLM が文書から参照してよいもの。
// 一覧に無いファイル名を LLM が作り上げると、文書としては正しいのに
// 読込で「見つからない」になる（環境光は黙って前のまま・モデルは球で描く）
// ので、実在するものだけを見せる。集めるのは SceneGenerator（ファイルを
// 歩く側）、ここは文章にするだけ。
struct InventoryModel {
    std::string file;          // assets/ 相対
    double size[3] = {0, 0, 0};  // 倍率 1 での外接箱 (x y z、モデル単位)
};
struct AssetInventory {
    std::vector<std::string> hdrs;      // <environment hdr>（assets/ 相対）
    std::vector<std::string> textures;  // <ground texture>（assets/ 相対）
    std::vector<InventoryModel> models; // <asset><mesh file scale>
};

struct PromptOptions {
    bool meshGeneration = false;  // <mesh generate> を使ってよいか
    int maxMeshes = 4;
    std::vector<LibraryEntry> library;  // 最近のものから最大 40 件
    AssetInventory assets;
};

std::string buildSystemPrompt(const PromptOptions& opt);

// 指示文に載せる見本の文書。**この文書は警告 0 件で読めなければならない**
// （見本が警告を出すと、LLM はその書き方を真似て毎回修正の往復になる）。
// 文書の書式を変えたら、テスト（tests/document）でここも読ませて確かめる。
const char* systemPromptExample();

// mode と依頼文と（Edit / Append のとき）今のシーン XML から、最初の
// user メッセージを作る。digest はエンジンが作ったシーンの要約（SceneDigest.h。
// 空なら付けない）で、XML の後ろに置く。
std::string buildUserPrompt(GenMode mode, const std::string& request,
                            const std::string& currentXml, const std::string& digest = {});

// 見た目の見直し（適用したシーンの画像を添える user メッセージ）。currentXml は
// 適用後のシーン全体、views は添えた画像の順に「どこから見た絵か」の説明。
// 直す所が無ければ kNoChanges だけを返させる。
extern const char* const kNoChanges;  // "NO_CHANGES"
std::string buildReviewPrompt(const std::string& request, const std::string& currentXml,
                              const std::vector<std::string>& views,
                              const std::string& digest = {});
// 応答が「直す所なし」か（文書を含まず kNoChanges を含む）。
bool replySaysNoChanges(const std::string& reply);

// 応答から <wizengine ...>...</wizengine> を取り出す。``` で囲まれていても、
// 推論系モデルの <think> の中に下書きがあっても、**最後の**文書を採る。
// 見つからなければ false（error に英語の理由）。
bool extractSceneXml(const std::string& reply, std::string& xml, std::string& error);

// 直させるときの user メッセージ（壊れた XML / 警告の一覧）。
std::string repairPrompt(const std::string& problem, const std::vector<std::string>& items);

}  // namespace autogen
}  // namespace wizengine
