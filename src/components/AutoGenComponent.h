#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "autogen/SceneGenerator.h"
#include "autogen/SceneHistory.h"
#include "scene/SceneComponent.h"

// ブラウザの「🤖 AI」モーダルを受ける係（src/autogen/ とエンジンの継ぎ目）。
//
//   ai.generate {prompt, mode: "new"|"edit"|"append", meshes: bool}
//   ai.cancel
//
// シーンを書き換える操作なので、他の edit.* と同じく**エディタモード ×
// エディタカメラのページ専用**（判定はサーバー）。生成そのものは
// SceneGenerator の専用スレッドで回り、できた文書は EditorState の Op
// （"xml" = 置き換え / "xml.append" = 追加）として物理スレッドへ渡す
// = 既存の 📄 XML の「✓ 適用」と同じ経路。Chrono を触るのは物理スレッド
// だけ、という約束はそのまま。
//
// 状態（進み・ログ・設定済みか）は /scene の "ai" で返す（main が
// statusJson() を差し込む）。API キーはブラウザへ出さない。
//
// **見た目の見直し**（config.json の ai.vision）: 適用の後、ワーカースレッドが
// 撮影を頼んで待ち（SnapshotFn）、RENDER スレッドの onRenderUnlocked が
// Renderer::captureSnapshot で数枚撮って渡す（Filament を触るのはそのスレッド
// だけ）。撮るのは Op が物理スレッドで適用され、レンダラブルとモデルが
// 揃うまで少し待ってから（kSettle）。視点はエディタカメラと、シーンの物を
// 囲む正面・横・上の 3 方向。
class AutoGenComponent : public SceneComponent {
public:
    explicit AutoGenComponent(wizengine::autogen::AiConfig cfg);

    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override;

    // RENDER スレッド: 撮影の依頼が来ていれば撮って渡す。
    void onRenderUnlocked(Scene& scene) override;

    // /scene の "ai"。sceneFile = 今のシーン名（その履歴を足す）。
    nlohmann::json statusJson(const std::string& sceneFile) const;
    // 終了時に main が Scene より先に呼ぶ（以後、結果を Scene へ渡さない）。
    void shutdown();

    // 撮影の受け渡し（ワーカー ⇄ RENDER）。ワーカーのラムダが shared_ptr で
    // 持つので、係が先に壊れても待っている側は closed で抜けられる。
    struct SnapshotQueue {
        std::mutex mutex;
        std::condition_variable cv;
        bool requested = false;   // ワーカーが頼んだ（RENDER が撮ると false）
        bool done = false;        // 撮り終えた（result / error が入っている）
        bool closed = false;      // 終了（以後は撮らない・待たない）
        std::chrono::steady_clock::time_point notBefore{};
        int width = 512, height = 288;
        wizengine::autogen::SnapshotSet result;
        std::string error;
    };

private:
    wizengine::autogen::SceneGenerator generator_;
    // シーンごとの「これまでの流れ」（assets/generated/history/）。ワーカーの
    // 終わりの知らせ（DoneFn）も持つので shared_ptr。
    std::shared_ptr<wizengine::autogen::SceneHistoryStore> history_;
    std::shared_ptr<SnapshotQueue> snap_ = std::make_shared<SnapshotQueue>();
};
