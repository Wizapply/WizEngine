#include "components/UiPointerInput.h"

#include <algorithm>

#include "core/Json.h"
#include "core/Log.h"
#include "scene/Scene.h"
#include "ui/UiKinds.h"
#include "ui/UiLayout.h"
#include "ui/UiTypes.h"

namespace ui = wizengine::ui;
using UiState = wizengine::UiState;

UiPointerInput::UiPointerInput(std::size_t cameraCount) : cams_(cameraCount) {}

UiPointerInput::~UiPointerInput() = default;

bool UiPointerInput::onCommand(Scene& scene, std::size_t camIndex, const std::string& cmd,
                               const nlohmann::json& msg, const UiState::Snapshot& snap,
                               int viewW, int viewH) {
    if (camIndex >= cams_.size()) return false;
    Cam& cam = cams_[camIndex];
    const bool dragOrRelease = cmd == "drag" || cmd == "release";
    if (cam.pressing && dragOrRelease) {
        pressCommand(scene, camIndex, cmd, msg, snap, viewW, viewH);
        return true;
    }
    if (cmd == "pick") cam.pressing = false;  // 届かなかった release の残り（押下は捨てる）
    if (cmd != "pick" && cmd != "hover") return false;
    // 世代は受け付けるかを見る前に読む（読んだ後に切り替わっても、古い世代として捨てる）。
    const std::uint64_t epoch = scene.editor().modeEpoch();
    const std::uint64_t generation = scene.ui().runtimeGeneration();
    if (scene.editor().isEditor()) return false;  // エディタ中は選んで編集する物
    return hoverOrPick(scene, camIndex, cmd, msg, snap, viewW, viewH, epoch, generation);
}

UiPointerInput::ButtonHit UiPointerInput::buttonAt(const UiState::Snapshot& snap,
                                                   std::size_t camIndex, double x, double y,
                                                   int viewW, int viewH) {
    ButtonHit hit;
    if (viewW <= 0 || viewH <= 0) return hit;  // まだビューが無い
    // このカメラの映像に重なる Canvas を手前から（描く順と同じ規則 ui::canvasBehind の逆）。
    std::vector<int> order;
    for (std::size_t i = 0; i < snap.canvases.size(); ++i) {
        const ui::CanvasDesc* desc = snap.liveCanvas(int(i));
        if (desc && ui::overlayShownOnCamera(*desc, int(camIndex))) order.push_back(int(i));
    }
    std::sort(order.begin(), order.end(), [&snap](int a, int b) {
        return ui::canvasBehind(snap.liveCanvas(b)->order, b, snap.liveCanvas(a)->order, a);
    });
    for (const int index : order) {
        const ui::CanvasDesc& desc = *snap.liveCanvas(index);
        const std::vector<ui::ElementDesc>& elements = snap.elementsOf(desc);
        const ui::Rect canvas = ui::canvasRect(desc.height, viewW, viewH);
        const std::vector<ui::LayoutNode> nodes = ui::layoutTree(elements, {}, canvas);
        double px = 0.0, py = 0.0;
        ui::ndcToCanvas(canvas, x, y, px, py);
        // 押下を受け止めるのはボタンだけ（上に重なった字や板はボタンを隠さない）。
        const int k = ui::hitTest(nodes, px, py, [&elements](int i) {
            return ui::catchesPointer(elements[std::size_t(i)]);
        });
        if (k < 0) continue;
        const ui::ElementDesc& e = elements[std::size_t(k)];
        hit.ref = snap.elementRef(index, e.id);
        hit.accepts = ui::acceptsClick(e, e.props, nodes[std::size_t(k)].visible);
        return hit;
    }
    return hit;
}

UiState::Pointer UiPointerInput::pointerOver(const ButtonHit& hit, bool pressed) {
    UiState::Pointer pointer;
    if (hit.ref.canvas < 0) return pointer;
    pointer.canvas = hit.ref.canvas;
    pointer.hover = hit.ref.element;
    if (pressed) pointer.pressed = hit.ref.element;
    return pointer;
}

bool UiPointerInput::hoverOrPick(Scene& scene, std::size_t camIndex, const std::string& cmd,
                                 const nlohmann::json& msg, const UiState::Snapshot& snap,
                                 int viewW, int viewH, std::uint64_t epoch,
                                 std::uint64_t generation) {
    const ButtonHit hit = buttonAt(snap, camIndex, wizengine::jsonNumber(msg, "x", 0.0),
                                   wizengine::jsonNumber(msg, "y", 0.0), viewW, viewH);
    const bool pick = cmd == "pick";
    const bool pressed = pick && hit.ref.canvas >= 0;
    scene.ui().setPointer(camIndex, pointerOver(hit, pressed));
    // hover はほかの係（ギズモのハイライト）にも渡す。ボタンに当たらない pick も流す。
    if (!pressed) return false;
    Cam& cam = cams_[camIndex];
    cam.pressing = true;
    cam.press = hit;
    cam.pressOver = true;
    cam.pressEpoch = epoch;
    cam.pressGeneration = generation;
    // 物の掴みが同じフレームで動かないよう、カーソルを外す（ギズモと同じ）。
    scene.boxController(camIndex).clearPointer();
    return true;
}

void UiPointerInput::pressCommand(Scene& scene, std::size_t camIndex, const std::string& cmd,
                                  const nlohmann::json& msg, const UiState::Snapshot& snap,
                                  int viewW, int viewH) {
    Cam& cam = cams_[camIndex];
    // 押した後にモードが変わった・Reset をまたいだ押下は受け止めるだけ（見た目もクリックも
    // 出さない = Reset で消えた見た目を戻さない）。
    const bool sameMode = cam.pressEpoch == scene.editor().modeEpoch();
    const bool sameRun = cam.pressGeneration == scene.ui().runtimeGeneration();
    const bool live = sameMode && sameRun;
    if (cmd == "release") {
        cam.pressing = false;
        if (!live) return;
        // ボタンの上で離したらクリック（release は位置を運ばないので、最後の drag の位置）。
        // 積むのは受け付けるボタンだけ（最後の確かめは takeClicks）。
        if (cam.pressOver && cam.press.accepts) {
            UiState::PendingClick click;
            click.camera = camIndex;
            click.ref = cam.press.ref;
            click.modeEpoch = cam.pressEpoch;
            click.runtimeGeneration = cam.pressGeneration;
            scene.ui().pushClick(click);
        }
        scene.ui().setPointer(camIndex, cam.pressOver ? pointerOver(cam.press, false)
                                                      : UiState::Pointer{});
        return;
    }
    if (!live) return;
    // drag: 押したボタンの上にいる間だけ押下の見た目（外で離せばクリックにならない）。
    const ButtonHit hit = buttonAt(snap, camIndex, wizengine::jsonNumber(msg, "x", 0.0),
                                   wizengine::jsonNumber(msg, "y", 0.0), viewW, viewH);
    cam.pressOver = hit.ref.canvas == cam.press.ref.canvas &&
                    hit.ref.element == cam.press.ref.element;
    UiState::Pointer pointer;
    pointer.canvas = cam.press.ref.canvas;
    if (cam.pressOver) pointer = pointerOver(cam.press, true);
    scene.ui().setPointer(camIndex, pointer);
}

void UiPointerInput::onPhysicsStep(Scene& scene) {
    // 確かめたクリック（UiState::takeClicks が唯一の確かめ）。受け手のノード（OnUiClick）が
    // できるまではログに出すだけ（取り出さないと、積んだ物が上限まで溜まる）。
    for (const auto& c : scene.ui().takeClicks(scene.editor().modeEpoch())) {
        LOGI("ui", "button click: canvas %d element %d '%s' (camera %zu)", c.canvas, c.element,
             c.name.c_str(), c.camera);
    }
}
