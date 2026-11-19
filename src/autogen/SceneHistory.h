#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "document/SceneDocument.h"

// AI シーン生成の「これまでの流れ」（シーンごとの履歴）。
//
// LLM は 1 回の依頼ごとに今のシーンしか見ないので、「さっき足したテーブル」
// 「元に戻して」「前回の続きで」が通じず、ユーザーが手で直した所を次の生成で
// 黙って戻すこともある。そこで依頼と結果、その間にユーザーが手で変えた所を
// 1 件ずつ記録し、直近の数件（config.json の ai.history）を短い英文にして
// user メッセージに添える。XML の全文は持ち回らない（毎回送る今のシーンの
// XML が唯一の正）。履歴にあるのは「何が変わったか」の行だけ。
//
//   * AI の 1 件 = 依頼文・モード・結果・AI が変えた所（依頼の直前のシーンと、
//     適用し終えた直後のシーン = EditorState::aiBaseline の差）
//   * ユーザーの 1 件 = 前回の AI の適用（基準）から、次の依頼までにエディタで
//     変わった所（同じく 2 つの文書の差。依頼のときに記録して基準を今へ進める
//     = 同じ変更を 2 回書かない）
//
// 差は文書の値どうしを名前で突き合わせて出す（diffScenes）。カメラは見ない
// （エディタで視点を回すたびに「変わった」になるため）。
//
// 保存先は <assetRoot>/<generatedDir>/history/<シーン名>.json（無題のシーンは
// _untitled）。シーン名ごとに分ける = 別のシーンを開くと別の流れになる。
// エンジンを再起動しても続く（基準の XML もファイルに入る）。
// Chrono も Filament も知らない。
namespace wizengine {
namespace autogen {

struct HistoryEntry {
    std::string time;     // "2026-11-19 10:02"（ローカル時刻）
    std::string source;   // "ai" | "user"
    std::string mode;     // "new" | "edit" | "append"（ai のみ）
    std::string request;  // 依頼文（ai のみ。長ければ切る）
    std::string outcome;  // ai: "applied" / "applied; revised 1x after the visual check" / "failed: ..."
    std::vector<std::string> changes;  // 英語の行（diffScenes）
};

nlohmann::json toJson(const HistoryEntry& e);
HistoryEntry historyEntryFromJson(const nlohmann::json& j);

// before → after で変わった所（英語の行。最大 maxLines 行 + 「... and N more」）。
// body・ジョイント・ライトは名前で突き合わせる（名前の無い物は種類ごとの
// 並び順）。カメラは見ない。
std::vector<std::string> diffScenes(const editor::SceneDocument& before,
                                    const editor::SceneDocument& after,
                                    std::size_t maxLines = 40);

// 新しく作ったシーンの中身の一覧（"new" モードの AI の 1 件の変更の代わり）。
std::vector<std::string> describeNewScene(const editor::SceneDocument& doc,
                                          std::size_t maxLines = 20);

// user メッセージに入れる文章（古い順）。空なら空文字列。
std::string formatHistory(const std::vector<HistoryEntry>& entries);

// 今のローカル時刻（"YYYY-MM-DD HH:MM"）。
std::string historyTimestamp();

// シーン名 → 履歴のキー（ファイル名）。空 = "_untitled"。
std::string historyKey(const std::string& sceneFile);

// シーンごとの履歴（ファイルに保存）。どのスレッドから呼んでもよい（mutex）。
class SceneHistoryStore {
public:
    // dir = 保存先のフォルダ（無ければ書くときに作る）。
    explicit SceneHistoryStore(std::string dir);

    // 直近 n 件（古い順）。n = 0 なら全部。
    std::vector<HistoryEntry> entries(const std::string& scene, std::size_t n = 0) const;
    // 基準（最後に AI が適用し終えた / ユーザーの変更を記録した時点のシーン XML）。
    std::string baseline(const std::string& scene) const;

    void add(const std::string& scene, HistoryEntry e);
    void setBaseline(const std::string& scene, std::string xml);
    // 基準を置き換え、同時に 1 件足す（ファイルは 1 回だけ書く）。
    void addWithBaseline(const std::string& scene, HistoryEntry e, std::string xml);
    // そのシーンの履歴と基準を消す（ファイルも）。
    void clear(const std::string& scene);

    // 1 シーンに覚えておく件数の上限（古い物から捨てる）。プロンプトに入れるのは
    // このうち config の ai.history 件。
    static constexpr std::size_t kKeep = 50;

private:
    struct Data {
        bool loaded = false;
        std::string baseline;
        std::vector<HistoryEntry> entries;
    };
    Data& load(const std::string& key) const;  // mutex_ の下で呼ぶ
    void save(const std::string& key, const Data& d) const;
    std::string pathFor(const std::string& key) const;

    std::string dir_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, Data> cache_;
};

}  // namespace autogen
}  // namespace wizengine
