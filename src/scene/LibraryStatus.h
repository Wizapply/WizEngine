#pragma once

#include <string>

#include "scene/StatusCaveat.h"  // unusableNameCaveat

// グローバル（ライブラリ）のアセットの操作（edit.lib.share / unshare / import / delete）で
// ブラウザに出すステータスの文（日本語。唯一の定義）。判断の流れ（scene/LibraryFlow の
// applyLibraryFlow）と、種類ごとに断るとき（UI = scene/UiLibraryOps）が同じ文を使う。
// label は種類の呼び名（"計算式" / "プレハブ" / "UI"）。イベントの共通化の文は
// SceneEvents.cpp のまま。文を変えるきっかけは「ライブラリの操作の知らせ方を変える」だけ
// （操作の流れは使う側）。Chrono も Filament も読まない。
namespace wizengine {
namespace libstatus {

inline std::string alreadyGlobal(const std::string& label, const std::string& name) {
    return label + " " + name + " は既にグローバルです";
}
inline std::string globalNameTaken(const std::string& label, const std::string& name) {
    return "グローバルに同じ名前の" + label + " " + name +
           " があります（もう一度選ぶと上書きを確認します）";
}
inline std::string shareFailed(const std::string& label) {
    return label + "をグローバルにできません（コンソール参照）";
}
inline std::string shared(const std::string& label, const std::string& name,
                          const std::string& file) {
    return label + " " + name + " をグローバルにしました（" + file + "。他のシーンからも使えます）";
}
// 名前が正規化で全部落ちたので、グローバルにしなかった。
inline std::string unusableName(const std::string& label) {
    return label + "をグローバルにできません" + unusableNameCaveat();
}
inline std::string unshared(const std::string& label, const std::string& name) {
    return label + " " + name +
           " をこのシーン専用（ローカル）にしました"
           "（保存でシーンに埋め込みます。グローバルのファイルは残ります）";
}
// ファイルが読めなかった（中身が空の）物は、ローカルにしない（空の中身が正になるため）。
inline std::string unshareUnreadable(const std::string& label, const std::string& name) {
    return label + " " + name +
           " はファイルが読めないので、ローカルにできません（ファイルを戻して読み直してください）";
}
inline std::string alreadyInScene(const std::string& label, const std::string& name) {
    return "このシーンには既に" + label + " " + name + " があります";
}
inline std::string importFailed(const std::string& label, const std::string& name) {
    return label + " " + name + " を読み込めません（コンソール参照）";
}
// hint = 次に何をすれば使えるか（"制御器にすると回ります" など）。
inline std::string imported(const std::string& label, const std::string& name,
                            const std::string& hint) {
    return label + " " + name + " をこのシーンで使えるようにしました（" + hint + "）";
}
inline std::string deleteOnlyInEditor() {
    return "グローバルからの削除はエディタモードでだけできます";
}
inline std::string deleteInUse(const std::string& label) {
    return "このシーンが使っている" + label + "は削除できません（ローカルにするか消してから）";
}
inline std::string trashFailed(const std::string& label, const std::string& name) {
    return label + " " + name + " をごみ箱へ移せません（コンソール参照）";
}
inline std::string trashed(const std::string& label, const std::string& name) {
    return "グローバルの" + label + " " + name + " をごみ箱へ移しました";
}

}  // namespace libstatus
}  // namespace wizengine
