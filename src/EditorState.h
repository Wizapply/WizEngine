#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
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

    // ---- ジョイント -----------------------------------------------------
    std::vector<wizengine::editor::JointDesc> joints() const;
    void setJoints(std::vector<wizengine::editor::JointDesc> joints);
    // 追加した番号を返す。
    int addJoint(const wizengine::editor::JointDesc& joint);
    bool removeJoint(int index);
    std::size_t jointCount() const;

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

    // assets/scenes/ にある *.json の一覧（キャッシュ）。保存・読込のたびに
    // refresh する。HTTP スレッドが毎回ディレクトリを走査しないための配慮。
    std::vector<std::string> sceneFiles() const;
    void refreshSceneFiles();

    // ---- ファイル ---------------------------------------------------------
    // assets/scenes（無ければ作る）。
    static std::string scenesDir();
    // 受け取った名前から英数字・_ - のみを残し、".json" を付けたフルパス。
    // 空になった場合は空文字列を返す（呼び出し側で弾く）。
    static std::string scenePath(const std::string& name);

    // 保存・読み込み。失敗時は false を返し、reason に理由を入れる。
    static bool writeJson(const std::string& path, const nlohmann::json& doc,
                          std::string& reason);
    static bool readJson(const std::string& path, nlohmann::json& doc,
                         std::string& reason);

private:
    std::atomic<wizengine::editor::AppMode> mode_{wizengine::editor::AppMode::Simulate};
    std::atomic<int> request_{0};  // 0=なし 1=Editor 2=Simulate

    mutable std::mutex mutex_;
    std::vector<Op> pending_;
    std::atomic<int> pendingCount_{0};
    std::vector<wizengine::editor::JointDesc> joints_;
    wizengine::editor::SimSettings sim_;
    wizengine::editor::GizmoSettings gizmo_;
    std::string status_ = "ready";
    std::string sceneFile_;
    std::vector<std::string> files_;
};
