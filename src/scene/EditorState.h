#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "document/EditorTypes.h"

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
    // Part はプレハブ編集モード中の「部品」（index = 部品番号。どのプレハブ
    // かは prefabEditObject() のオブジェクトに付いているもの）。
    enum class SelKind : int { None = 0, Light = 1, Camera = 2, Part = 3 };
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

    // ---- イベントアセット（ノードベースのスクリプト）------------------------
    // ノードとワイヤーの束を「アセット」として名前で持つ（EditorTypes.h の
    // EventAssetDesc）。アセットは付けて初めて動く: オブジェクトに付いたぶんは
    // BodyDesc::events（Scene が持つ）、シーン全体に付いたぶんはここの
    // worldEvents。書くのは物理スレッド（Op 実行時）、読むのは HTTP スレッド
    // （サイドバー用 JSON）と物理スレッド（実行キャッシュの取り込み）。
    // ジョイントと同じ mutex で守る。ノード id はアセットの中で一意で、
    // 削除しても再利用しない（ワイヤーが別のノードを指してしまうため）。
    std::vector<wizengine::editor::EventAssetDesc> eventAssets() const;
    bool hasEventAsset(const std::string& name) const;
    // 丸ごと差し替え（読込・全消し用）。各アセットの採番は id の最大 + 1。
    void setEventAssets(std::vector<wizengine::editor::EventAssetDesc> assets,
                        std::vector<std::string> worldEvents);
    // 空のアセットを作る / 消す（消すときはワールドの付け先も外す。
    // オブジェクト側の付け先を外すのは Scene の仕事）。名前は正規化済みの
    // ものを渡すこと（wizengine::editor::sanitizeEventName）。
    bool addEventAsset(const std::string& name);
    bool removeEventAsset(const std::string& name);
    // シーン全体に付いているアセット名。
    std::vector<std::string> worldEvents() const;
    bool attachWorldEvent(const std::string& name);
    bool detachWorldEvent(const std::string& name);

    // ---- ノードとワイヤーの編集（必ずどのアセットへの操作かを渡す）--------
    // id を採番して追加し、その id を返す（アセットが無ければ -1）。
    int addGraphNode(const std::string& asset, wizengine::editor::NodeDesc node);
    // 送られてきたキーだけ上書き（クランプ込み）。id が無ければ false。
    bool updateGraphNode(const std::string& asset, int id,
                         const nlohmann::json& patch);
    // ノードと、それに繋がるワイヤーを消す。
    bool removeGraphNode(const std::string& asset, int id);
    // from = トリガー / to = アクション の向きと存在を検証してから張る。
    // 重複や向き違いは false（理由は status に入れない - UI 側が防ぐ前提の
    // 二重チェックなので）。
    bool addGraphWire(const std::string& asset, int from, int to);
    bool removeGraphWire(const std::string& asset, int from, int to);
    // グラフが変わるたびに増える版番号。物理スレッドは毎パスこれだけを見て、
    // 変わったときだけ一覧をコピーし直す（毎ステップのロックを避ける）。
    // オブジェクト側の付け先（BodyDesc::events）を書き換えた Scene も、
    // 実行側に取り込み直させるために bumpGraphVersion() を呼ぶ。
    std::uint64_t graphVersion() const { return graphVersion_.load(); }
    void bumpGraphVersion() { graphVersion_.fetch_add(1); }
    // ノードの発火回数（ノードエディタの ⚡ バッジ用）。キーは（アセット名,
    // ノード id）。書くのは物理スレッド、読むのは HTTP スレッド。
    // シミュレート開始でクリアされる。
    void noteNodeFired(const std::string& asset, int id);
    void clearNodeFireCounts();
    std::map<std::pair<std::string, int>, int> nodeFireCounts() const;

    // ---- 計算式アセット（ノード式。vehicle/Formula.h）---------------------
    // イベントアセットと同じ流儀: 名前で持ち、車両のタイヤが名前で参照する
    // （BodyDesc::vehicle の axles[].tire.formula）。書くのは物理スレッド
    // （Op 実行時）、読むのは HTTP スレッド（サイドバー用 JSON）と物理スレッド
    // （車両の生成時にコンパイル）。ノード id はアセットの中で一意で、削除
    // しても再利用しない。
    std::vector<wizengine::vehicle::FormulaGraphDesc> formulaAssets() const;
    bool hasFormulaAsset(const std::string& name) const;
    void setFormulaAssets(std::vector<wizengine::vehicle::FormulaGraphDesc> assets);
    // graph.name が空 / 重複なら false。中身ごと登録できる（テンプレート用）。
    bool addFormulaAsset(wizengine::vehicle::FormulaGraphDesc graph);
    bool removeFormulaAsset(const std::string& name);
    // id を採番して追加し、その id を返す（アセットが無い・種類が不正なら -1）。
    int addFormulaNode(const std::string& asset,
                       wizengine::vehicle::FormulaNodeDesc node);
    // 送られてきたキー（x y name params）だけ上書き。種類と id は変えない。
    bool updateFormulaNode(const std::string& asset, int id,
                           const nlohmann::json& patch);
    bool removeFormulaNode(const std::string& asset, int id);
    // 両端とポート番号を検証し、同じ入力ポートへの古い線は張り替える。
    // 循環になる線は張らない（式は前進評価なので）。
    bool addFormulaWire(const std::string& asset,
                        const wizengine::vehicle::FormulaWireDesc& wire);
    bool removeFormulaWire(const std::string& asset,
                           const wizengine::vehicle::FormulaWireDesc& wire);
    // 式（またはタイヤの参照）が変わるたびに増える版番号。VehicleComponent が
    // 見て、変わっていたら車両モデルを作り直す（式のコンパイルは生成時）。
    std::uint64_t formulaVersion() const { return formulaVersion_.load(); }
    void bumpFormulaVersion() { formulaVersion_.fetch_add(1); }

    // ---- プレハブ（見た目の部品の集合）------------------------------------
    // イベント・計算式と同じ流儀のアセット。部品は番号で参照する（軸と同じ）。
    // 書くのは物理スレッド（Op 実行時とギズモのドラッグ）、読むのは HTTP
    // スレッド（JSON）と RENDER スレッド（部品の描画。prefabVersion で
    // キャッシュ）と INPUT スレッド（ギズモのピック）。
    std::vector<wizengine::editor::PrefabDesc> prefabAssets() const;
    bool hasPrefabAsset(const std::string& name) const;
    bool prefabAsset(const std::string& name, wizengine::editor::PrefabDesc& out) const;
    void setPrefabAssets(std::vector<wizengine::editor::PrefabDesc> assets);
    bool addPrefabAsset(wizengine::editor::PrefabDesc prefab);  // 名前が空 / 重複で false
    bool removePrefabAsset(const std::string& name);
    // 部品。追加は番号を返す（-1 = プレハブが無い）。
    int addPart(const std::string& prefab, wizengine::editor::PartDesc part);
    // 送られてきたキーだけ上書き（partFromJson + clampPart）。
    bool updatePart(const std::string& prefab, int index, const nlohmann::json& patch);
    bool setPartTransform(const std::string& prefab, int index,
                          const wizengine::editor::Vec3d* pos,
                          const wizengine::editor::Vec3d* rot,
                          const wizengine::editor::Vec3d* size);
    bool removePart(const std::string& prefab, int index);
    bool partDesc(const std::string& prefab, int index,
                  wizengine::editor::PartDesc& out) const;
    std::uint64_t prefabVersion() const { return prefabVersion_.load(); }
    void bumpPrefabVersion() { prefabVersion_.fetch_add(1); }
    // プレハブ編集モード: 編集中のオブジェクト番号（-1 = 通常）。Unity の
    // プレハブモードに相当し、その間はそのオブジェクトの部品だけが選べる。
    int prefabEditObject() const { return prefabEdit_.load(); }
    void setPrefabEditObject(int index) { prefabEdit_.store(index); }

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
    // イベントアセット（mutex_ の下）。nextNodeId はアセットごとの採番、
    // fireCounts_ は（アセット名, ノード id）-> 発火回数。
    struct EventAssetState {
        wizengine::editor::EventAssetDesc desc;
        int nextNodeId = 1;
    };
    std::vector<EventAssetState> events_;
    std::vector<std::string> worldEvents_;
    // 計算式アセット（mutex_ の下）。
    struct FormulaAssetState {
        wizengine::vehicle::FormulaGraphDesc desc;
        int nextNodeId = 1;
    };
    std::vector<FormulaAssetState> formulas_;
    std::atomic<std::uint64_t> formulaVersion_{0};
    // プレハブ（mutex_ の下）。
    std::vector<wizengine::editor::PrefabDesc> prefabs_;
    std::atomic<std::uint64_t> prefabVersion_{0};
    std::atomic<int> prefabEdit_{-1};
    std::atomic<std::uint64_t> graphVersion_{0};
    std::map<std::pair<std::string, int>, int> fireCounts_;
    wizengine::editor::SimSettings sim_;
    wizengine::editor::GizmoSettings gizmo_;
    std::string status_ = "ready";
    std::string sceneFile_;
    std::vector<std::string> files_;
};
