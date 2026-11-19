#pragma once

#include <string>
#include <vector>

#include "document/SceneDocument.h"

// 今のシーンの「要約」（LLM に XML と一緒に渡す英文）と、依頼文の @名前。
//
// XML には名前と数値しか無く、LLM は「左の赤い箱」「テーブルの上のコップ」
// 「これ」がどれかを座標から自分で推理するしかない（そこで外す）。そこで
// エンジンが文書から決まった計算で出せることを文章にして添える:
//   * 物ごとに 形・実寸（回転込みの外接箱）・中心・底と上面の高さ・色の名前・
//     固定 / 質量・プレハブ / 車両 / ソフトの印
//   * 位置関係: 何の上に載っているか（底が相手の上面に 3 cm 以内で接し、
//     水平に重なる）・何とめり込んでいるか・宙に浮いているか（固定でも
//     ジョイントでもなく、何にも載っていない = シミュレートで落ちる）
//   * エディタカメラ（ユーザーが見ている絵）から見て 左 / 中央 / 右・
//     上 / 下・カメラからの距離（画面外ならそう書く）。ユーザーは画面の
//     左右で話すので、ワールドの ±X では答え合わせができない
//   * ジョイントの相手、ライト・カメラの一覧
//   * ユーザーが選んでいる物（focus）と @ で名指しした物
//
// Chrono も Filament も知らない（文書の値だけ）。テストは単体で回せる。
namespace wizengine {
namespace autogen {

struct DigestOptions {
    // 文書の cameras の何番がエディタカメラか（ユーザーが見ている絵）。
    int editorCamera = 0;
    // エディタで選んでいる物の名前（「これ」「それ」が指す物）。
    std::vector<std::string> focus;
    // 依頼文で @ 名指しされた名前（resolveMentions の結果）。
    std::vector<std::string> mentioned;
    // 一覧に載せる物の上限（超えた分は件数だけ）。
    std::size_t maxBodies = 150;
};

// 要約の本文（英語、"SCENE SUMMARY" で始まる数十行）。
std::string describeScene(const editor::SceneDocument& doc, const DigestOptions& opt);

// 名前の無い body に "body<番号>" の名前を付ける（番号 = worldbody の中の順。
// 既にある名前とぶつかれば "_2" … を足す）。LLM が名前で指せるように、
// 送る前の写しに付ける（編集モードでは返ってきた文書でそのまま残る）。
// 付けた数を返す。
int nameUnnamedBodies(editor::SceneDocument& doc);

// 文書の中で @ で指せる名前（body・ジョイント・ライト・カメラ・ケーブル）。
std::vector<std::string> mentionableNames(const editor::SceneDocument& doc);

// 依頼文の @名前 を拾う。@ の直後から、names のうち最も長く一致する名前を採る
// （日本語の名前の後ろに助詞が続いても切れる: "@テーブルの上に" → テーブル）。
// 一致しない @ は unknown に、@ に続く語（空白・句読点まで）を入れる。メール
// アドレスのような「英数字の直後の @」は名指しと見なさない。
struct Mentions {
    std::vector<std::string> found;    // 重複なし、出てきた順
    std::vector<std::string> unknown;  // 同上
};
Mentions resolveMentions(const std::string& prompt, const std::vector<std::string>& names);

// 色の名前（"red" / "dark gray" など。rgba はリニア値）。
std::string colorName(float r, float g, float b);

}  // namespace autogen
}  // namespace wizengine
