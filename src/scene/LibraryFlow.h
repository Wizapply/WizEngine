#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// グローバル（ライブラリ）のアセット（計算式・プレハブ・UI）の、共用にする・ローカルにする・
// 読み込む・削除の判断の流れ（applyLibraryFlow、唯一の定義）と、そこへ渡す種類ごとの部品の
// 組（LibraryKind）、変更の目印（LibraryBaseline）。イベントの共通化は従来どおり別
// （SceneEvents.cpp）。直すきっかけは「ライブラリの操作の流れを変える」だけ（文は
// scene/LibraryStatus.h、置き場の規則は scene/SharedAssetFile、種類ごとの中身は使う側 =
// SceneLibrary.cpp と scene/UiLibraryOps）。Chrono も Filament も Scene も知らない
// （tests/document で 3 種類を回す）。すべて PHYSICS thread。
namespace wizengine {

// 変更の目印: 共用のファイルを読んだとき・書いたときの中身（正規化した XML のテキスト）。
// 保存で今の中身から作ったテキストと比べ、違うものだけ書く（触っていないものは書き戻さない）。
// 「ファイルが読めたか」の唯一の持ち主でもある: file のある物で覚えが無い = 読めなかった
// （保存で書かない = 空で上書きしない・ローカルにしない）。キーの規則はここだけ。
class LibraryBaseline {
public:
    // kind は種類の名前（"formula" / "prefab" / "ui" = LibraryKind::id）。
    void remember(const std::string& kind, const std::string& name, std::string text) {
        texts_[key(kind, name)] = std::move(text);
    }
    void forget(const std::string& kind, const std::string& name) { texts_.erase(key(kind, name)); }
    // 覚えた中身（無ければ nullptr）。
    const std::string* find(const std::string& kind, const std::string& name) const {
        const auto it = texts_.find(key(kind, name));
        return it == texts_.end() ? nullptr : &it->second;
    }
    void clear() { texts_.clear(); }

private:
    static std::string key(const std::string& kind, const std::string& name) {
        return kind + ":" + name;
    }
    std::map<std::string, std::string> texts_;
};

// 種類ごとに違うところ（計算式・プレハブ・UI）。判断の流れは applyLibraryFlow の 1 本で、
// ここに入れた関数だけが種類ごと。
struct LibraryKind {
    const char* id = "";     // "formula" / "prefab" / "ui"（ログと覚えのキー）
    std::string label;       // ステータスの呼び名（libstatus の label）
    std::string importHint;  // 読み込んだ後の案内（libstatus::imported の hint）
    // 名前 → ライブラリのファイル（assets/ 相対。名前が不正なら空）。
    std::function<std::string(const std::string& name)> libraryFile;
    // 今のシーンにあれば true と今の file（空 = ローカル）。
    std::function<bool(const std::string& name, std::string& file)> find;
    // 共用にする対象を決める（name を書き換えてよい）。断るなら false と refusal（黙って
    // 断るなら空）。
    std::function<bool(std::string& name, std::string& file, std::string& refusal)> findForShare;
    // 共用にしたときにファイルへ書く中身（file を付けた形）と、それを今のシーンへ入れる
    // （入れられなければ false = 共用にしなかったとして知らせる）。
    std::function<std::string(const std::string& name, const std::string& file)> sharedText;
    std::function<bool(const std::string& name, const std::string& file)> commitShare;
    // ローカルにする（参照をやめる。中身はシーンに残す）。できなければ false。
    std::function<bool(const std::string& name)> unshare;
    // ライブラリから読んで足す。読めたら覚えも入れる（種類ごとの「読んだ後に覚えを入れる」
    // 関数を通す = シーンの読込と同じ 1 か所）。
    std::function<bool(const std::string& name, const std::string& file, std::string& reason)>
        import;
};

// 結果（ステータスと、ライブラリの一覧を取り直すか）。
struct LibraryResult {
    std::string status;
    bool refresh = false;
};

// edit.lib.share / unshare / import / delete の判断の流れ（唯一の定義）。what は操作の名前
// （"share" など。知らなければ何もしない）、name は正規化済みの名前、editorMode = 今が
// エディタモードか（グローバルからの削除はエディタモードだけ）。
LibraryResult applyLibraryFlow(const LibraryKind& k, const std::string& what, std::string name,
                               bool overwrite, bool editorMode, LibraryBaseline& baseline);

// 「シーンの一覧（名前と file を持つ Desc の並び）」の形の種類（計算式・プレハブ）の部品を
// k に入れる（find / findForShare / sharedText / commitShare / unshare）。get = 今の一覧、
// set = 一覧を書き戻す、text = ライブラリのファイルの中身。一覧は 1 回の操作につき最初に
// 使うときに 1 回だけ取る（共用にする手順の途中で何度も写さない）。
template <class Desc>
void bindListKind(LibraryKind& k, std::function<std::vector<Desc>()> get,
                  std::function<void(std::vector<Desc>)> set,
                  std::function<std::string(const Desc&)> text) {
    auto cache = std::make_shared<std::optional<std::vector<Desc>>>();
    auto list = [get, cache]() -> std::vector<Desc>& {
        if (!*cache) *cache = get();
        return **cache;
    };
    auto findDesc = [list](const std::string& n) -> Desc* {
        for (auto& d : list()) {
            if (d.name == n) return &d;
        }
        return nullptr;
    };
    k.find = [findDesc](const std::string& n, std::string& file) {
        const Desc* d = findDesc(n);
        if (d) file = d->file;
        return d != nullptr;
    };
    k.findForShare = [find = k.find](std::string& n, std::string& file, std::string&) {
        return find(n, file);
    };
    k.sharedText = [findDesc, text](const std::string& n, const std::string& file) {
        const Desc* d = findDesc(n);
        if (!d) return std::string();
        Desc shared = *d;
        shared.file = file;
        return text(shared);
    };
    auto setFile = [findDesc, list, set](const std::string& n, const std::string& file) {
        Desc* d = findDesc(n);
        if (!d) return false;
        d->file = file;
        set(list());
        return true;
    };
    k.commitShare = setFile;
    k.unshare = [setFile](const std::string& n) { return setFile(n, ""); };
}

}  // namespace wizengine
