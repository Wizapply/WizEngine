#pragma once

#include <cstddef>
#include <memory>

#include <nlohmann/json.hpp>

#include "autogen/SceneGenerator.h"
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
class AutoGenComponent : public SceneComponent {
public:
    explicit AutoGenComponent(wizengine::autogen::AiConfig cfg);

    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override;

    nlohmann::json statusJson() const { return generator_.statusJson(); }
    // 終了時に main が Scene より先に呼ぶ（以後、結果を Scene へ渡さない）。
    void shutdown() { generator_.shutdown(); }

private:
    wizengine::autogen::SceneGenerator generator_;
};
