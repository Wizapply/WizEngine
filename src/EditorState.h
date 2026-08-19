#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "EditorTypes.h"

// エディタ／シミュレートの切り替えと、まだ適用されていない編集操作を持つ入れ物。
//
// 名前空間について: 値の型（BodyDesc / JointDesc / ...）は `wizengine::editor`
// にまとめてあるが、このクラス自体はグローバル。Scene・PhysicsWorld・
// SceneComponent・PhysicsTuning といったシーン層のクラスが全部グローバルに
// あり、`wizengine` は Renderer やローダーなど下回りに使われている、という
// 既存の分け方に合わせている。
//
// スレッドの約束（ここが一番大事）:
//   * モード（mode_）と切替要求は atomic。誰が読んでもよい。
//   * 編集操作（Op）は INPUT スレッド（ブラウザのコマンド）が push し、
//     PHYSICS スレッドが drain して実行する。PhysicsWorld を触ってよいのは
//     物理スレッドだけなので、実体の生成・削除は必ずこの経路を通る。
//   * ジョイント一覧と設定は mutex 付き。書くのは物理スレッド（Op 実行時）、
//     読むのは HTTP スレッド（サイドバー用 JSON）と物理スレッド。
//
// オブジェクト（BodyDesc）そのものはここではなく Scene が持つ。実体（Chrono の
// ボディと Filament のレンダラブル）と一対一で並べておかないと、番号がずれた
// ときに黙って別の物を動かしてしまうため。
class EditorState {
public:
    // ブラウザから来た 1 個の編集操作。kind はコマンド名（"add" / "remove" /
    // "set" / "joint.add" ...）、args はそのパラメータ。JSON のまま運ぶのは、
    // 操作を増やすときに配管（キュー・構造体・分岐）を触らずに済むから。
    struct Op {
        std::string kind;
        nlohmann::json args;
        std::size_t camera = 0;  // どのカメラのページから来たか
    };

    // ---- モード ---------------------------------------------------------
    wizengine::editor::AppMode mode() const { return mode_.load(); }
    bool isEditor() const { return mode_.load() == wizengine::editor::AppMode::Editor; }
    // 物理スレッドが遷移を終えてから確定させる。
    void setMode(wizengine::editor::AppMode m) { mode_.store(m); }
    // 切替要求（INPUT スレッド）。実際の切替は物理スレッドが行う。
    void requestMode(wizengine::editor::AppMode m) {
        request_.store(m == wizengine::editor::AppMode::Editor ? 1 : 2);
    }
    // 物理スレッド: 要求があれば取り出して true。
    bool takeModeRequest(wizengine::editor::AppMode& out) {
        const int r = request_.exchange(0);
        if (r == 0) return false;
        out = (r == 1) ? wizengine::editor::AppMode::Editor : wizengine::editor::AppMode::Simulate;
        return true;
    }

    // ---- 編集操作キュー -------------------------------------------------
    void push(Op op);
    std::vector<Op> drain();
    // 積み残しの有無（物理スレッドが毎パス見る）。
    bool hasPending() const { return pendingCount_.load() > 0; }

    // ---- エディタ選択（ライト / カメラ）-----------------------------------
    // オブジェクトの選択は従来どおり BoxController（カメラ毎）が持つ。ここに
    // 持つのは「エディタカメラがライトかカメラを選んでいる」状態だけ。
    // オブジェクト選択と同時には立たないよう、立てる側（EditorComponent /
    // Scene）が反対側を必ず消す。kind と index は別々の atomic なので瞬間的に
    // 食い違いうるが、読む側は必ず alive / active を確かめるので実害はない。
    enum class SelKind : int { None = 0, Light = 1, Camera = 2 };
    void setSel(SelKind kind, int index) {
        selIndex_.store(index);
        selKind_.store(int(kind));
    }
    void clearSel() { setSel(SelKind::None, -1); }
    SelKind selKind() const { return SelKind(selKind_.load()); }
    int selIndex() const { return selIndex_.load(); }

    // ---- ジョイント -----------------------------------------------------
    std::vector<wizengine::editor::JointDesc> joints() const;
    void setJoints(std::vector<wizengine::editor::JointDesc> joints);
    // 追加した番号を返す。
    int addJoint(const wizengine::editor::JointDesc& joint);
    bool removeJoint(int index);
    std::size_t jointCount() const;

    // ---- イベントグラフ ---------------------------------------------------
    // ノードとワイヤーの一覧。書くのは物理スレッド（Op 実行時）、読むのは
    // HTTP スレッド（サイドバー用 JSON）と物理スレッド（実行キャッシュの
    // 取り込み）。ジョイントと同じ mutex で守る。ノードは id で引く（番号を
    // 詰めるとワイヤーが別のノードを指すため、id は再利用しない）。
    std::vector<wizengine::editor::NodeDesc> graphNodes() const;
    std::vector<wizengine::editor::WireDesc> graphWires() const;
    // 丸ごと差し替え（読込・全消し用）。nextNodeId は id の最大 + 1 に直す。
    void setGraph(std::vector<wizengine::editor::NodeDesc> nodes,
                  std::vector<wizengine::editor::WireDesc> wires);
    // id を採番して追加し、その id を返す。
    int addGraphNode(wizengine::editor::NodeDesc node);
    // 送られてきたキーだけ上書き（クランプ込み）。id が無ければ false。
    bool updateGraphNode(int id, const nlohmann::json& patch);
    // ノードと、それに繋がるワイヤーを消す。
    bool removeGraphNode(int id);
    // from = トリガー / to = アクション の向きと存在を検証してから張る。
    // 重複や向き違いは false（理由は status に入れない - UI 側が防ぐ前提の
    // 二重チェックなので）。
    bool addGraphWire(int from, int to);
    bool removeGraphWire(int from, int to);
    // グラフが変わるたびに増える版番号。物理スレッドは毎パスこれだけを見て、
    // 変わったときだけ一覧をコピーし直す（毎ステップのロックを避ける）。
    std::uint64_t graphVersion() const { return graphVersion_.load(); }
    // ノードの発火回数（ノードエディタの ⚡ バッジ用）。書くのは物理スレッド、
    // 読むのは HTTP スレッド。シミュレート開始でクリアされる。
    void noteNodeFired(int id);
    void clearNodeFireCounts();
    std::vector<std::pair<int, int>> nodeFireCounts() const;

    // ---- シミュレート設定 -----------------------------------------------
    wizengine::editor::SimSettings sim() const;
    void setSim(const wizengine::editor::SimSettings& s);

    // ---- ギズモ設定 -------------------------------------------------------
    // 書くのは INPUT スレッド、読むのは RENDER（描画）と PHYSICS（ドラッグの
    // 適用）。1 回のドラッグの途中で切り替わっても破綻しない値しか無いので、
    // 他の設定と同じ mutex で足りる。
    wizengine::editor::GizmoSettings gizmo() const;
    void setGizmo(const wizengine::editor::GizmoSettings& g);

    // ---- UI 向けの状態 ---------------------------------------------------
    // 直近の操作結果（「保存しました」「ジョイントを作成」など）。
    void setStatus(std::string text);
    std::string status() const;

    void setSceneFile(std::string name);
    std::string sceneFile() const;

    // assets/scenes/ にあるシーンの一覧（拡張子を落とした名前。キャッシュ）。
    // *.xml（今の形式）と *.json（旧形式）の両方を拾い、同じ名前があれば
    // 1 つにまとめる。保存・読込のたびに refresh する。HTTP スレッドが毎回
    // ディレクトリを走査しないための配慮。
    std::vector<std::string> sceneFiles() const;
    void refreshSceneFiles();

    // ---- ファイル ---------------------------------------------------------
    // assets/scenes（無ければ作る）。
    static std::string scenesDir();
    // 保存名の正規化（英数字と _ - のみを残す・64 文字まで）。ファイル名と
    // 文書の model 名の両方にこれを使う - 一覧（sceneFiles）はファイル名の
    // 語幹なので、別々に正規化すると「保存したのにタイルが選択されない」に
    // なる。空になったら不正な名前（呼び出し側で弾く）。
    static std::string sanitizeSceneName(const std::string& name);
    // 受け取った名前から英数字・_ - のみを残し、".xml" を付けたフルパス。
    // 空になった場合は空文字列を返す（呼び出し側で弾く）。シーンの保存は
    // 常にこちら（XML が正）。
    static std::string scenePath(const std::string& name);
    // 同じ名前の旧形式（.json）のフルパス。読み込みの後方互換にだけ使う。
    static std::string legacyScenePath(const std::string& name);

    // 保存・読み込み（テキスト）。失敗時は false を返し、reason に理由を
    // 入れる。中身が XML か JSON かはここでは見ない。
    static bool writeText(const std::string& path, const std::string& text,
                          std::string& reason);
    static bool readText(const std::string& path, std::string& text,
                         std::string& reason);
    // 旧形式（*.json）の読み込み。新規の保存には使わない。
    static bool readJson(const std::string& path, nlohmann::json& doc,
                         std::string& reason);

private:
    std::atomic<wizengine::editor::AppMode> mode_{wizengine::editor::AppMode::Simulate};
    std::atomic<int> request_{0};  // 0=なし 1=Editor 2=Simulate

    std::atomic<int> selKind_{0};    // SelKind
    std::atomic<int> selIndex_{-1};

    mutable std::mutex mutex_;
    std::vector<Op> pending_;
    std::atomic<int> pendingCount_{0};
    std::vector<wizengine::editor::JointDesc> joints_;
    // イベントグラフ（mutex_ の下）。fireCounts_ は id -> 発火回数。
    std::vector<wizengine::editor::NodeDesc> nodes_;
    std::vector<wizengine::editor::WireDesc> wires_;
    int nextNodeId_ = 1;
    std::atomic<std::uint64_t> graphVersion_{0};
    std::vector<std::pair<int, int>> fireCounts_;
    wizengine::editor::SimSettings sim_;
    wizengine::editor::GizmoSettings gizmo_;
    std::string status_ = "ready";
    std::string sceneFile_;
    std::vector<std::string> files_;
};
