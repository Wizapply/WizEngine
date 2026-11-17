#include "components/UiInputComponent.h"

#include <atomic>
#include <string>
#include <utility>

#include "core/Json.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/UiOps.h"
#include "ui/UiHandles.h"
#include "ui/UiLayout.h"

namespace ui = wizengine::ui;

namespace {

// 映像の位置（NDC: 左下 -1・右上 +1。ブラウザの toNdc と同じ）→ Canvas の設計の画素
// （左上が原点・y 下向き）。overlay の Canvas は映像全体に重なる。
void ndcToCanvas(const ui::Rect& canvas, double x, double y, double& px, double& py) {
    px = canvas.x + (x + 1.0) * 0.5 * canvas.w;
    py = canvas.y + (1.0 - y) * 0.5 * canvas.h;
}

// 性質の pos / size（立っていなければ既定値）。
ui::Placement placementOf(const ui::PropSet& props) {
    ui::Placement p;
    for (std::size_t k = 0; k < 2; ++k) {
        p.pos[k] = props.number(ui::Prop::Pos, k);
        p.size[k] = props.number(ui::Prop::Size, k);
    }
    return p;
}

bool samePlacement(const ui::Placement& a, const ui::Placement& b) {
    return a.pos[0] == b.pos[0] && a.pos[1] == b.pos[1] && a.size[0] == b.size[0] &&
           a.size[1] == b.size[1];
}

}  // namespace

// ---- カメラごとのドラッグの状態 ------------------------------------------------
struct UiInputComponent::Cam {
    // INPUT が書く（pick で掴む・drag で動かす・release で離す）。serial は掴むたびに
    // 1 進み、ほかの欄を全部書いた後に進める（PHYSICS は serial の変化で開始を知る）。
    // released は離したドラッグの serial（PHYSICS は最後の位置を 1 回当ててから止める =
    // 押して動かして離すが物理の 1 パスの間に全部届いても、最後の位置が当たる）。
    // held は INPUT だけが見る（掴んでいる間の drag / release を受けるか）。
    std::atomic<bool> held{false};
    std::atomic<std::uint64_t> serial{0};
    std::atomic<std::uint64_t> released{0};
    std::atomic<int> canvas{-1}, element{-1}, handle{int(ui::FrameHandle::None)};
    std::atomic<double> startX{0.0}, startY{0.0}, x{0.0}, y{0.0};  // NDC
    std::atomic<int> startViewW{0}, startViewH{0};  // 掴んだときのビューの大きさ（画素）
    std::atomic<std::uint64_t> epoch{0};            // 掴んだときの EditorState::modeEpoch
    std::atomic<std::uint64_t> loadGeneration{0};   // 掴んだときの UiState の読込の世代
    // RENDER が書く（onRender）: このカメラのビューの今の大きさ（画素。無ければ 0）。
    std::atomic<int> viewW{0}, viewH{0};

    // PHYSICS だけが使う（開始時の値）。
    std::uint64_t seen = 0;  // 最後に始めた serial
    bool live = false;       // 当て続けてよいドラッグか
    bool moved = false;      // 押した位置から一度でも動いたか
    // このドラッグで一度書いた性質（以後は開始値と同じでも書く = 戻したら戻る）。
    bool wrotePos = false, wroteSize = false;
    int dragCanvas = -1, dragElement = -1;
    ui::FrameHandle dragHandle = ui::FrameHandle::None;
    ui::Rect canvasRect;     // 開始時の Canvas 全体（NDC → 設計の画素の換算）
    ui::Rect parentRect;     // 開始時の親の矩形
    ui::PropSet start;       // 開始時の性質（設計値）
    ui::Placement last;      // 最後に書いた（または開始時の）pos / size
};

UiInputComponent::UiInputComponent(std::size_t cameraCount) {
    for (std::size_t i = 0; i < cameraCount; ++i) cams_.push_back(std::make_unique<Cam>());
}

UiInputComponent::~UiInputComponent() = default;

const wizengine::UiState::Snapshot& UiInputComponent::snapshot(Scene& scene) {
    scene.ui().copyIfChanged(known_, snap_);
    return snap_;
}

bool UiInputComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    const std::string cmd = wizengine::jsonString(msg, "cmd", "");
    if (cmd.rfind(kEditPrefix, 0) == 0) return pushEdit(scene, camIndex, cmd, msg);
    if (cmd == "select.ui") return select(scene, camIndex, msg);
    if (cmd == "pick") return pick(scene, camIndex, msg);
    if (cmd != "drag" && cmd != "release") return false;  // not ours
    // 掴んでいる間の drag / release は全部ここで受ける（ギズモ・物の掴みへ渡さない）。
    // 掴んでいなければ素通し。
    if (camIndex >= cams_.size()) return false;
    Cam& cam = *cams_[camIndex];
    if (!cam.held.load()) return false;
    if (cmd == "release") {
        cam.held.store(false);
        cam.released.store(cam.serial.load());  // このドラッグは最後の位置を当てて終わる
        return true;
    }
    cam.x.store(wizengine::jsonNumber(msg, "x", cam.x.load()));
    cam.y.store(wizengine::jsonNumber(msg, "y", cam.y.load()));
    return true;
}

bool UiInputComponent::select(Scene& scene, std::size_t camIndex, const nlohmann::json& msg) {
    EditorState& state = scene.editor();
    // 受け取って捨てる = true（他の係へ回さない）。判定はサーバーが持つ（editsUiFrom。
    // 一覧の UI の行もプレハブ編集中は選べない見た目にしてある）。
    if (!scene.editsUiFrom(camIndex)) return true;

    const int canvas = wizengine::jsonInt(msg, "canvas", -1);
    // 要素の id は 0 以上。負は全部「Canvas そのもの」(-1) にそろえる。
    const int raw = wizengine::jsonInt(msg, "element", -1);
    const int element = raw < 0 ? -1 : raw;
    if (canvas < 0) {
        if (state.selKind() == EditorState::SelKind::Canvas) state.clearSel();
        return true;
    }
    // 消えた Canvas・無い要素は選ばせない（読込と行き違ったクリックなど）。
    if (!snapshot(scene).selectionExists(canvas, element)) return true;
    scene.setEditorSel(camIndex, EditorState::SelKind::Canvas, canvas, element);
    return true;
}

bool UiInputComponent::pushEdit(Scene& scene, std::size_t camIndex, const std::string& cmd,
                                const nlohmann::json& msg) {
    EditorState& state = scene.editor();
    // 受け取って黙って捨てる = true（EditorComponent へ回さない）。判定は select.ui と同じ
    // editsUiFrom。受け付けないページの edit.* を黙って捨てるのは EditorComponent と同じ。
    if (!scene.editsUiFrom(camIndex)) return true;
    // Op の種類は "edit." を外した名前（"ui.add" など）。知っている操作かは UiOps の一覧。
    const std::string kind = cmd.substr(std::char_traits<char>::length("edit."));
    if (!wizengine::uiops::isUiOp(kind)) {
        state.setStatus("UI: 知らない操作です: " + cmd);
        return true;
    }
    // 値の読み方と丸め・対象が今もあるかの確かめは PHYSICS の UiOps（ここで読んだ
    // 写しは 1 フレーム古いことがあるので、判断に使わない）。
    state.push({kind, msg, camIndex});
    return true;
}

bool UiInputComponent::pick(Scene& scene, std::size_t camIndex, const nlohmann::json& msg) {
    if (camIndex >= cams_.size()) return false;
    Cam& cam = *cams_[camIndex];
    // 世代は受け付けるかを見る前に読む（読んだ後に切り替わっても、PHYSICS が古い世代と
    // して捨てる）。
    const std::uint64_t epoch = scene.editor().modeEpoch();
    if (!scene.editsUiFrom(camIndex)) return false;
    const EditorState& state = scene.editor();
    // 枠が出るのは要素を選んでいるときだけ（Canvas そのものは映像全体 = 枠を出さない）。
    if (state.selKind() != EditorState::SelKind::Canvas) return false;
    const int canvasIndex = state.selIndex();
    const int elementId = state.selSub();
    if (elementId < 0) return false;

    const wizengine::UiState::Snapshot& snap = snapshot(scene);
    const ui::CanvasDesc* desc = snap.liveCanvas(canvasIndex);
    if (!desc || !ui::frameShownOnCamera(*desc, int(camIndex))) return false;
    // 共用 UI の要素は動かさない（Inspector と同じ規則）。押しても下へ流す。
    if (!wizengine::uiops::elementsEditable(*desc)) return false;
    const int viewW = cam.viewW.load();
    const int viewH = cam.viewH.load();
    if (viewW <= 0 || viewH <= 0) return false;  // まだビューが無い

    const std::vector<ui::ElementDesc>& elements = snap.elementsOf(*desc);
    const std::size_t at = ui::elementIndex(elements, elementId);
    if (at == elements.size()) return false;
    const ui::Rect canvas = ui::canvasRect(desc->height, viewW, viewH);
    const std::vector<ui::LayoutNode> nodes = ui::layoutTree(elements, {}, canvas);
    const double x = wizengine::jsonNumber(msg, "x", 0.0);
    const double y = wizengine::jsonNumber(msg, "y", 0.0);
    double px = 0.0, py = 0.0;
    ndcToCanvas(canvas, x, y, px, py);
    // つまみの大きさは画面の上で一定（描く側の buildFrameMesh と同じ換算）。
    const double half = ui::handleHalfDesign(ui::designPerScreenPx(canvas.h, viewH));
    const ui::FrameHandle handle = ui::hitHandle(nodes[at].rect, px, py, half);
    if (handle == ui::FrameHandle::None) return false;

    cam.canvas.store(canvasIndex);
    cam.element.store(elementId);
    cam.handle.store(int(handle));
    cam.startX.store(x);
    cam.startY.store(y);
    cam.x.store(x);
    cam.y.store(y);
    cam.startViewW.store(viewW);
    cam.startViewH.store(viewH);
    cam.epoch.store(epoch);
    cam.loadGeneration.store(snap.loadGeneration);
    cam.held.store(true);
    cam.serial.fetch_add(1);  // 最後に進める（PHYSICS はこれで開始を知る）
    // 物の掴み（自由移動）が同じフレームで動かないよう、カーソルを外す（ギズモと同じ）。
    scene.boxController(camIndex).clearPointer();
    return true;
}

void UiInputComponent::onEditorStep(Scene& scene, double dt) {
    (void)dt;
    for (std::size_t c = 0; c < cams_.size(); ++c) {
        Cam& cam = *cams_[c];
        // serial を先に読む（その後で離されたら、次のパスで最後の 1 回を当てる）。
        const std::uint64_t serial = cam.serial.load();
        const bool ended = cam.released.load() == serial;
        if (serial != cam.seen) {
            cam.seen = serial;
            cam.live = beginDrag(scene, c, cam);
        }
        if (!cam.live) continue;
        // 掴んだ後にモードが往復した・プレハブ編集に入ったドラッグは止める。
        const bool sameMode = cam.epoch.load() == scene.editor().modeEpoch();
        if (!sameMode || !scene.editsUiFrom(c)) {
            cam.live = false;
            continue;
        }
        // 離したドラッグも、届いている最後の位置を当ててから止める。
        cam.live = applyDrag(scene, cam) && !ended;
    }
}

bool UiInputComponent::beginDrag(Scene& scene, std::size_t camIndex, Cam& cam) {
    // 掴んだ時点から変わっていないか（INPUT の写しは 1 フレーム古いことがある）。
    const bool sameMode = cam.epoch.load() == scene.editor().modeEpoch();
    const bool sameLoad = cam.loadGeneration.load() == scene.ui().loadGeneration();
    if (!sameMode || !sameLoad || !scene.editsUiFrom(camIndex)) return false;
    const int canvasIndex = cam.canvas.load();
    ui::CanvasDesc desc;
    if (!scene.ui().copyCanvas(canvasIndex, desc)) return false;  // 消えた
    if (!wizengine::uiops::elementsEditable(desc) || !ui::frameShownOnCamera(desc, int(camIndex))) {
        return false;
    }
    const int elementId = cam.element.load();
    const std::size_t at = ui::elementIndex(desc.elements, elementId);
    if (at == desc.elements.size()) return false;  // 消えた
    cam.dragCanvas = canvasIndex;
    cam.dragElement = elementId;
    cam.dragHandle = ui::FrameHandle(cam.handle.load());
    // 開始時の Canvas と親の矩形（以後この値だけを見る = 解像度が途中で変わっても、
    // 押した位置からの動きの換算は変わらない）。
    cam.canvasRect = ui::canvasRect(desc.height, cam.startViewW.load(), cam.startViewH.load());
    const std::vector<ui::LayoutNode> nodes = ui::layoutTree(desc.elements, {}, cam.canvasRect);
    const int parent = nodes[at].parent;
    cam.parentRect = parent >= 0 ? nodes[std::size_t(parent)].rect : cam.canvasRect;
    cam.start = desc.elements[at].props;
    cam.last = placementOf(cam.start);
    cam.moved = false;
    cam.wrotePos = false;
    cam.wroteSize = false;
    return true;
}

bool UiInputComponent::applyDrag(Scene& scene, Cam& cam) {
    // 押した位置からの動き（設計の画素。y は下向き）。開始時の Canvas で換算する。
    double sx = 0.0, sy = 0.0, nx = 0.0, ny = 0.0;
    ndcToCanvas(cam.canvasRect, cam.startX.load(), cam.startY.load(), sx, sy);
    ndcToCanvas(cam.canvasRect, cam.x.load(), cam.y.load(), nx, ny);
    const double dx = nx - sx;
    const double dy = ny - sy;
    // 押しただけ（まだ動かしていない）では書かない（刻みへ寄せて値が変わらないように）。
    // 一度動いた後に押した位置へ戻したときは、戻した値を書く。
    if (!cam.moved && dx == 0.0 && dy == 0.0) return true;
    cam.moved = true;
    const double step = scene.editor().gizmo().snap ? ui::kSnapStepPx : ui::kFineStepPx;
    const ui::Placement next =
        ui::dragPlacement(cam.parentRect, cam.start, cam.dragHandle, dx, dy, step);
    if (samePlacement(next, cam.last)) return true;  // 変わらなければ書かない（版を進めない）

    // 書くのは今の設計値の 1 枚（Inspector の編集などが間に入っていてもよい。pos / size
    // だけを開始時の値 + 動きで上書きする）。手順は Inspector の編集と同じ editElement /
    // writeOwnProps（種類が持つ性質だけを丸めて書く）。
    const ui::Placement from = placementOf(cam.start);
    const bool posChanged = next.pos[0] != from.pos[0] || next.pos[1] != from.pos[1];
    const bool sizeChanged = next.size[0] != from.size[0] || next.size[1] != from.size[1];
    // 変えていない方は書き足さない（既定値のままの性質を文書に増やさない）。開始時に
    // 書いてあった性質と、このドラッグで一度書いた性質は書き続ける（開始値へ戻したとき、
    // 前に書いた値が残らない）。
    const bool writePos = posChanged || cam.wrotePos || cam.start.has(ui::Prop::Pos);
    const bool writeSize = sizeChanged || cam.wroteSize || cam.start.has(ui::Prop::Size);
    ui::PropSet incoming;
    if (writePos) incoming.setNumber(ui::Prop::Pos, next.pos[0], next.pos[1]);
    if (writeSize) incoming.setNumber(ui::Prop::Size, next.size[0], next.size[1]);
    const wizengine::uiops::ElementWrite w = wizengine::uiops::editElement(
        scene.ui(), cam.dragCanvas, cam.dragElement,
        [&incoming](ui::CanvasDesc& desc, std::size_t at) {
            wizengine::uiops::writeOwnProps(desc.elements[at], std::move(incoming));
        });
    if (!w.written) return false;  // 消えた・共用になった
    cam.wrotePos = writePos;
    cam.wroteSize = writeSize;
    cam.last = next;
    return true;
}

void UiInputComponent::onRender(Scene& scene) {
    // オブジェクトのロックの中: ビューの大きさを写すだけ（INPUT が NDC を Canvas の画素へ
    // 直すのに使う）。
    for (std::size_t c = 0; c < cams_.size() && c < scene.cameraCount(); ++c) {
        const int view = scene.cameraViewIndex(c);
        const filament::math::int2 size =
            view >= 0 ? scene.renderer().viewSize(std::size_t(view)) : filament::math::int2{0, 0};
        cams_[c]->viewW.store(size.x);
        cams_[c]->viewH.store(size.y);
    }
}
