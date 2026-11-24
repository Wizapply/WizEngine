#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "scene/UiState.h"

class Scene;

// シミュレート中のボタン（Interaction::Click の要素）の操作。UiInputComponent が 1 つ持ち、
// onCommand（INPUT）と onPhysicsStep（PHYSICS）から呼ぶだけ（エディタの選択・ドラッグとは
// 状態を共有しない）。
//
//   pick                  … ボタンに当たったら押下を受け止めて true（後ろの物を掴まない =
//                           クリックの優先順位はボタンが最初）。当たらなければ false（ギズモ・
//                           カメラ・物の掴みへ）。
//   drag / release        … 受け止めた押下の間は全部ここで受ける（途中でモードが変わっても、
//                           後ろの物へ途中から渡さない）。ボタンの上で離したら、クリックを
//                           UiState に積む（pushClick）。
//   hover                 … ポインタが上にあるボタン。ギズモのハイライトにも要るので取らない。
//
// どのカメラのページからでも受けるが、そのカメラに出ている Canvas（ui::canvasShownOnCamera）
// だけに当てる: overlay を手前の Canvas（ui::canvasBehind の逆順）から、次に板（camera /
// world）のうちカーソルの下でカメラにいちばん近い板に（手前の板が後ろの板をふさぐ。板の上の
// 座標はカーソルのレイと板の面の交点 = Scene::canvasPointAt）。
// 当たり判定は呼び手の版の写しの設計値。ポインタの様子（上にいる / 押している）は INPUT が
// UiState へ直接置く（見た目に物理の 1 パスを待たせない）。受け付けない（enabled = false の）
// ボタンも同じく置き、見た目を変えないのは描く側（ui::resolve）の判断。クリックを積むのは
// 受け付けるボタン（ui::acceptsClick）だけで、押したときの世代（モード・実行時・要素の参照）を
// 添える。PHYSICS が UiState::takeClicks で確かめて取り出し、ノードの OnUiClick が受ける
// （Scene::collectUiClicks）。当たりの性質はノード（SetUiProp）の上書きまで重ねた値
// （ui::resolveElements。実行時の状態は版で写す）。
class UiPointerInput {
public:
    explicit UiPointerInput(std::size_t cameraCount);
    ~UiPointerInput();

    // INPUT。cmd は pick / drag / release / hover のどれか。snap = 呼び手の版の写し、
    // viewW / viewH = そのカメラのビューの大きさ（画素。無ければ 0）。true = 取った。
    bool onCommand(Scene& scene, std::size_t camIndex, const std::string& cmd,
                   const nlohmann::json& msg, const wizengine::UiState::Snapshot& snap,
                   int viewW, int viewH);

private:
    // 映像の上のボタンの当たり。
    struct ButtonHit {
        wizengine::UiState::ElementRef ref;  // 当たったボタン（canvas = -1 = 無し）
        bool accepts = false;  // クリックを受け付ける（ui::acceptsClick。false でも受け止める）
    };
    // カメラごとの押下（INPUT だけが使う = onCommand は INPUT の 1 本のスレッド）。
    struct Cam {
        bool pressing = false;   // ボタンが押下を受け止めている（release まで drag も受ける）
        ButtonHit press;         // 押したボタン
        bool pressOver = false;  // 今もそのボタンの上にいる（最後の drag の位置）
        std::uint64_t pressEpoch = 0;       // 押したときの EditorState::modeEpoch
        wizengine::UiState::Generations pressGenerations;  // 押したときの UiState::generations
    };

    // 映像の位置 (x, y)（NDC）にあるボタン（押下を受け止める種類だけ = ui::catchesPointer）。
    // overlay を手前から見て、無ければカーソルの下でいちばん近い板（camera / world）だけを
    // 見る（Scene::canvasPointAt。手前の物に隠れた板にも当たる）。
    ButtonHit buttonAt(const Scene& scene, const wizengine::UiState::Snapshot& snap,
                       std::size_t camIndex, double x, double y, int viewW, int viewH) const;
    // Canvas index（全体の矩形 canvas、設計の座標 (px, py)）の上のボタン。当たれば hit に入れて true。
    bool buttonOnCanvas(const wizengine::UiState::Snapshot& snap, int index,
                        const wizengine::ui::Rect& canvas, double px, double py,
                        ButtonHit& hit) const;
    // ボタン hit の上にいる（pressed = 押している）ときのポインタ（hit が無ければ「無し」）。
    static wizengine::UiState::Pointer pointerOver(const ButtonHit& hit, bool pressed);
    // hover / pick。epoch / generations = 受け付けるかを見る前に読んだ世代。
    bool hoverOrPick(Scene& scene, std::size_t camIndex, const std::string& cmd,
                     const nlohmann::json& msg, const wizengine::UiState::Snapshot& snap,
                     int viewW, int viewH, std::uint64_t epoch,
                     const wizengine::UiState::Generations& generations);
    // 受け止めた押下の drag / release（押している間だけ呼ぶ）。
    void pressCommand(Scene& scene, std::size_t camIndex, const std::string& cmd,
                      const nlohmann::json& msg, const wizengine::UiState::Snapshot& snap,
                      int viewW, int viewH);

    std::vector<Cam> cams_;  // 数は作るときに決まり、以後変えない
    // UiState のノードの上書きの写し（当たりに使う）と、その版（INPUT 専用。変わった Canvas
    // だけ写し直す）。
    wizengine::UiState::NodeOverrideSet overrides_;
    std::uint64_t overridesVersion_ = 0;
};
