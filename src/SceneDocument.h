#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "EditorTypes.h"
#include "SceneXml.h"

// シーン文書（保存・読込・受け渡しの単位）と、その XML 表現。
//
// ---- なぜ XML か -----------------------------------------------------------
// シーンの「中身」は MuJoCo の MJCF に倣った XML を正とする。要素の入れ子で
// 世界の構造（worldbody の中に body、body の中に geom）がそのまま見え、値は
// 全部属性に入るので、手で書いても差分を見ても読める。SceneDocument はその
// XML と 1 対 1 に対応する値型で、Scene（実体）と XML（ファイル）の間に
// 立つ唯一の経路になる:
//
//   Scene（Chrono / Filament の実体） <-> SceneDocument <-> XML テキスト
//
// ---- 書式（MJCF に寄せた点・違う点）----------------------------------------
//   <wizengine model="名前" version="4">
//     <option .../>                     ... シミュレート設定（MuJoCo の option）
//     <asset> <mesh name="apple" file="apple2.glb" scale="1"/> </asset>
//     <worldbody>
//       <environment hdr="studio.hdr" intensity="30000"/>
//       <ground size="10" visual="8" texture="textures/ground.png" tile="2"/>
//       <light .../> <camera .../>
//       <body name pos euler fixed> <geom type size mass rgba/> </body>
//     </worldbody>
//     <equality> <joint type body1 body2 anchor axis/> </equality>
//     <events>  <node .../> <wire from to/> </events>   ... WizEngine 独自
//   </wizengine>
//
//   * 角度は全部「度」。<body euler> と <light euler> はエディタが持つ
//     オイラー角（R = Rz*Ry*Rx）そのもの。
//   * geom の size は MuJoCo と同じ「半分の寸法」: box は各辺の半分、
//     sphere / mesh は半径。BodyDesc は辺の長さで持つので、ここで 1/2 する。
//   * <geom type="mesh" mesh="apple"> は <asset> の <mesh> を名前で参照する。
//     file は assets/ からの相対パス（".." と絶対パスは弾く）。見た目の
//     大きさは <mesh scale>、当たり判定の大きさは geom の size / collision。
//   * <ground> と <environment> は worldbody 直下の単一要素（WizEngine の
//     語彙）。ground の size / visual は半分の広さ（物理の床と見える地面）、
//     texture / hdr は assets/ 相対パス。節を書かない文書は既定値で開く。
//   * 色は rgba="r g b a"（リニア値、a は今は常に 1）。
//   * body1 / body2 は名前でも番号でも書ける。"world" と -1 が地面。
//     保存側は名前が一意ならその名前、そうでなければ番号を書く。
//   * <events> は MuJoCo に無い WizEngine の拡張（イベントグラフ）。
//     ノードの target / other はオブジェクト・ライト・カメラの番号。
//
// ---- 拡張の手順（新しい値を足すとき）---------------------------------------
// この文書は今後も節・属性が増えていく前提。足すときの決まりはこれだけ:
//
//   属性を 1 個足す:  EditorTypes.h の構造体にメンバを足す →
//     SceneDocument.cpp の「書き出し関数」と「読み込み関数」の対
//     （bodyElement / bodyFromXml のように必ず隣り合わせ）を両方直す。
//     読み込みは必ず既定値付きで書く（無い属性 = 既定値）。これで
//     属性を足しても古い文書はそのまま読めるし、新しい文書を古い版が
//     読んでも知らない属性を無視するだけで壊れない。
//   節（要素）を 1 個足す:  SceneDocument にベクタ（と、空と無指定を
//     区別したいなら has フラグ）を足す → toXml / fromXml に節の読み書き →
//     fromXml の「知っている節の一覧」（未知の節を警告する箇所）へ名前を足す。
//   kSceneDocVersion を上げる:  読めなくなる変更（属性の意味を変える・
//     消す）をしたときだけ。足すだけなら上げない - 新旧の相互読みを保つ。
//
// ---- 読み取りの約束 ---------------------------------------------------------
// 読み取りは失敗しない（欠けた属性・型違いは既定値）。ただし「黙って別の
// 意味になる」内容 - 未知の節・種類名の打ち間違い・見つからない body 参照・
// 入れ子 body・零ベクトルの軸・不正なワイヤーなど - は warnings に文で集める。
// 呼び出し側（シーン読込）はそれをログとステータスに出す。手で書いた XML の
// 打ち間違いを、動かして首をかしげる前に気付けるようにするため。
namespace wizengine {
namespace editor {

// 文書バージョン。1〜3 は旧 JSON 形式（読み込みのみ対応）、4 から XML が正。
constexpr int kSceneDocVersion = 4;

struct SceneDocument {
    std::string model;  // <wizengine model="...">。ふつうは保存名
    SimSettings sim;
    std::vector<MeshAssetDesc> meshes;  // <asset> の <mesh> 宣言
    GroundDesc ground;
    EnvironmentDesc environment;
    std::vector<LightDesc> lights;
    std::vector<CameraPose> cameras;
    std::vector<BodyDesc> bodies;
    std::vector<JointDesc> joints;
    std::vector<NodeDesc> nodes;
    std::vector<WireDesc> wires;
    // 「節が無い」と「空の節」の区別。ライト / カメラの節を持たない文書
    // （旧 v1 の保存や手書きの最小 XML）は初期構成へ戻す意味になるので、
    // 空配列と同じにはできない。
    bool hasSim = false;
    bool hasGround = false;
    bool hasEnvironment = false;
    bool hasLights = false;
    bool hasCameras = false;
};

// ---- XML -------------------------------------------------------------------
xml::Element toXml(const SceneDocument& doc);
std::string toXmlText(const SceneDocument& doc);
// 読み取りは失敗しない（欠けた属性は既定値）。ルート要素名は問わないので、
// <wizengine> でも <mujoco> でも同じように読める。
// warnings を渡すと、既定値へ落とした「怪しい中身」（上の「読み取りの約束」
// 参照）が文で入る。
SceneDocument fromXml(const xml::Element& root,
                      std::vector<std::string>* warnings = nullptr);
// テキストから。XML として壊れている場合だけ false（error に行番号付きの理由）。
bool parseXml(const std::string& text, SceneDocument& out, std::string& error,
              std::vector<std::string>* warnings = nullptr);

// ---- 旧 JSON 文書（version 1〜3）--------------------------------------------
// 既に保存してある assets/scenes/*.json を読むための取り込み口。書き出しは
// もう JSON では行わない（保存は常に XML）。
SceneDocument fromLegacyJson(const nlohmann::json& doc);

}  // namespace editor
}  // namespace wizengine
