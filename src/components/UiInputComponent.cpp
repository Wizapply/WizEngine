#include "components/UiInputComponent.h"

#include <atomic>
#include <string>
#include <utility>

#include "core/Json.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/UiFiles.h"
#include "scene/UiOps.h"
#include "ui/UiHandles.h"
#include "ui/UiLayout.h"
#include "ui/UiValue.h"

namespace ui = wizengine::ui;

namespace {

// 性質の pos / size（立っていなければ既定値）。
ui::Placement placementOf(const ui::PropSet& props) {
    ui::Placement p;
    for (std::size_t k = 0; k < 2; ++k) {
        p.pos[k] = props.number(ui::Prop::Pos, k);
        p.size[k] = props.number(ui::Prop::Size, k);
    }
    return p;
}

// 性質 p の値が 2 つの集まりで同じか（立っていなければ既定値で比べる）。
bool sameProp(const ui::PropSet& a, const ui::PropSet& b, ui::Prop p) {
    return ui::sameValue(ui::propInfo(p).spec, a.get(p), b.get(p));
}

// ビューの大きさ（画素）を 1 つの atomic に詰める（幅と高さを別々に読むと、解像度の
// 変更の途中で古い幅と新しい高さが組になる）。
std::uint64_t packSize(int w, int h) {
    return (std::uint64_t(std::uint32_t(w)) << 32) | std::uint32_t(h);
}
void unpackSize(std::uint64_t packed, int& w, int& h) {
    w = int(std::uint32_t(packed >> 32));
    h = int(std::uint32_t(packed & 0xffffffffu));
}

}  // namespace

// ---- カメラごとのドラッグの状態 ------------------------------------------------
struct UiInputComponent::Cam {
    // INPUT が書く（pick で掴む・drag で動かす・release で離す）。serial は掴むたびに
    // 2 進む: 欄を書く前に 1 進めて奇数（書いている途中）、全部書いた後にもう 1 進めて
    // 偶数（PHYSICS は偶数の serial の変化で開始を知り、読んだ後に同じ serial かを確かめる）。
    // released は離したドラッグの serial（PHYSICS は最後の位置を 1 回当ててから止める =
    // 押して動かして離すが物理の 1 パスの間に全部届いても、最後の位置が当たる）。
    // held は INPUT だけが見る（掴んでいる間の drag / release を受けるか）。
    std::atomic<bool> held{false};
    std::atomic<std::uint64_t> serial{0};
    std::atomic<std::uint64_t> released{0};
    std::atomic<int> canvas{-1}, element{-1}, handle{int(ui::FrameHandle::None)};
    std::atomic<double> startX{0.0}, startY{0.0}, x{0.0}, y{0.0};  // NDC
    std::atomic<std::uint64_t> startView{0};      // 掴んだときのビューの大きさ（packSize）
    std::atomic<std::uint64_t> epoch{0};          // 掴んだときの EditorState::modeEpoch
    std::atomic<std::uint64_t> loadGeneration{0};  // 掴んだときの UiState の読込の世代
    // 掴んだときの中身の世代（INPUT の写しの ContentsInfo::contentsGeneration）
    std::atomic<std::uint64_t> contentsGeneration{0};
    // RENDER が書く（onRender）: このカメラのビューの今の大きさ（packSize。無ければ 0）。
    std::atomic<std::uint64_t> view{0};

    // PHYSICS だけが使う（開始時の値）。
    std::uint64_t seen = 0;  // 最後に始めた serial
    bool live = false;       // 当て続けてよいドラッグか
    bool moved = false;      // 押した位置から一度でも動いたか
    // 今 pos / size を書いている状態か（開始時に書いていなかった性質は、開始時の値へ
    // 戻したら書かない形へ戻す = 文書に pos="0 0" を増やさない）。
    bool wrotePos = false, wroteSize = false;
    int dragCanvas = -1, dragElement = -1, dragParent = -1;
    double dragHeight = 0.0;             // 開始時の Canvas の基準の高さ
    std::uint64_t dragLoadGeneration = 0;  // 開始時の読込の世代
    std::uint64_t dragContentsGeneration = 0;  // 開始時の中身の世代（ui.xml の適用で変わる）
    std::uint64_t dragEpoch = 0;           // 掴んだときのモードの世代
    ui::FrameHandle dragHandle = ui::FrameHandle::None;
    double startX0 = 0.0, startY0 = 0.0;  // 押した位置（NDC。開始時に読んだ値）
    ui::Rect canvasRect;     // 開始時の Canvas 全体（NDC → 設計の画素の換算）
    ui::Rect parentRect;     // 開始時の親の矩形
    ui::PropSet start;       // 開始時の性質（設計値）
    ui::Placement last;      // 最後に書いた（または開始時の）pos / size
};

// 掴んだときに INPUT が置いた欄（PHYSICS が readGrab で 1 回だけ写す）。
struct UiInputComponent::Grab {
    int canvas = -1, element = -1;
    ui::FrameHandle handle = ui::FrameHandle::None;
    double startX = 0.0, startY = 0.0;
    int viewW = 0, viewH = 0;
    std::uint64_t epoch = 0, loadGeneration = 0, contentsGeneration = 0;
};

UiInputComponent::UiInputComponent(std::size_t cameraCount) {
    for (std::size_t i = 0; i < cameraCount; ++i) {
        cams_.push_back(std::make_unique<Cam>());
    }
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
    if (cmd.rfind(kLibPrefix, 0) == 0) return pushLibrary(scene, camIndex, cmd, msg);
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
    // XML の直接編集は壊れたテキストをキューに積まない（理由をステータスへ。適用は
    // 非同期なので、レスポンスでは返せない）。PHYSICS ももう一度確かめてから当てる。
    if (kind == "ui.xml") {
        const std::string problem =
            wizengine::uiops::xmlTextProblem(wizengine::jsonString(msg, "text", ""));
        if (!problem.empty()) {
            state.setStatus(problem);
            return true;
        }
    }
    // 値の読み方と丸め・対象が今もあるかの確かめは PHYSICS の UiOps（ここで読んだ
    // 写しは 1 フレーム古いことがあるので、判断に使わない）。
    state.push({kind, msg, camIndex});
    return true;
}

bool UiInputComponent::pushLibrary(Scene& scene, std::size_t camIndex, const std::string& cmd,
                                   const nlohmann::json& msg) {
    // kind = ui だけを受ける（ほかの種類は EditorComponent へ回す）。
    const std::string kind = wizengine::jsonString(msg, "kind", "");
    if (kind != wizengine::uifiles::kUiLibraryKind) return false;
    const std::string what = cmd.substr(std::char_traits<char>::length(kLibPrefix));
    // 一覧の取り直しは種類によらない（EditorComponent が積む）。
    if (what == "refresh") return false;
    // 受け付けないページ（エディタモード × エディタカメラでない）は黙って捨てる
    // （EditorComponent の edit.* と同じ）。
    if (!scene.editsFrom(camIndex)) return true;
    EditorState& state = scene.editor();
    if (!scene.editsUiFrom(camIndex)) {
        state.setStatus("UI: プレハブの編集中は共用 UI の操作はできません（◀ 戻る で抜けてから）");
        return true;
    }
    const bool known =
        what == "share" || what == "unshare" || what == "import" || what == "delete";
    if (!known) {
        state.setStatus("UI: 知らない操作です: " + cmd);
        return true;
    }
    // Op の種類は計算式・プレハブと同じ "lib.*"（当てるのは Scene::applyLibraryOp →
    // UI の部品は scene/UiLibraryOps。名前と canvas の読み方も向こう）。
    state.push({"lib." + what, msg, camIndex});
    return true;
}

bool UiInputComponent::pick(Scene& scene, std::size_t camIndex, const nlohmann::json& msg) {
    if (camIndex >= cams_.size()) return false;
    Cam& cam = *cams_[camIndex];
    // 前の押下の印が残っていれば下ろす（release が届かなかった）。残すと、この押下が UI に
    // 当たらなくても後の drag / release を食べ続け、ギズモが動かない。前のドラッグも止める。
    if (cam.held.exchange(false)) cam.released.store(cam.serial.load());
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
    int viewW = 0, viewH = 0;
    unpackSize(cam.view.load(), viewW, viewH);
    if (viewW <= 0 || viewH <= 0) return false;  // まだビューが無い

    const std::vector<ui::ElementDesc>& elements = snap.elementsOf(*desc);
    const std::size_t at = ui::elementIndex(elements, elementId);
    if (at == elements.size()) return false;
    const ui::Rect canvas = ui::canvasRect(desc->height, viewW, viewH);
    const std::vector<ui::LayoutNode> nodes = ui::layoutTree(elements, {}, canvas);
    // 編集してよくない中身（読めなかった共用 UI など）・見えない要素は掴まない（押しても
    // 下へ流す。枠は描く側が線だけ出す）。
    const wizengine::UiState::ContentsInfo info = snap.contentsInfo(canvasIndex);
    const bool editable = wizengine::uiops::elementsEditable(info);
    if (!ui::frameGrabbable(editable, nodes[at])) return false;
    const double x = wizengine::jsonNumber(msg, "x", 0.0);
    const double y = wizengine::jsonNumber(msg, "y", 0.0);
    double px = 0.0, py = 0.0;
    ui::ndcToCanvas(canvas, x, y, px, py);
    // つまみの当たりの大きさは画面の上で一定（描く四角より大きい。換算は buildFrameMesh と同じ）。
    const double half = ui::handleHitHalfDesign(ui::designPerScreenPx(canvas.h, viewH));
    const ui::FrameHandle handle = ui::hitHandle(nodes[at].rect, px, py, half);
    if (handle == ui::FrameHandle::None) return false;

    // 欄を書く間は serial を奇数にする（PHYSICS は奇数の間は読まない）。
    cam.serial.fetch_add(1);
    cam.canvas.store(canvasIndex);
    cam.element.store(elementId);
    cam.handle.store(int(handle));
    cam.startX.store(x);
    cam.startY.store(y);
    cam.x.store(x);
    cam.y.store(y);
    cam.startView.store(packSize(viewW, viewH));
    cam.epoch.store(epoch);
    cam.loadGeneration.store(snap.loadGeneration);
    cam.contentsGeneration.store(info.contentsGeneration);
    cam.held.store(true);
    cam.serial.fetch_add(1);  // 最後に進めて偶数に（PHYSICS はこれで開始を知る）
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
        if (serial & 1u) continue;  // INPUT が欄を書いている途中（次のパスで読む）
        const bool ended = cam.released.load() == serial;
        if (serial != cam.seen) {
            Grab grab;
            // 読んでいる間に次の押下が来たら（欄が混ざった）次のパスで読み直す。
            if (!readGrab(cam, serial, grab)) continue;
            cam.seen = serial;
            cam.live = beginDrag(scene, c, cam, grab);
        }
        if (!cam.live) continue;
        // 掴んだ後にモードが往復した・プレハブ編集に入ったドラッグは止める。
        const bool sameMode = cam.dragEpoch == scene.editor().modeEpoch();
        if (!sameMode || !scene.editsUiFrom(c)) {
            cam.live = false;
            continue;
        }
        // 離したドラッグも、届いている最後の位置を当ててから止める。
        cam.live = applyDrag(scene, c, cam, serial) && !ended;
    }
}

bool UiInputComponent::readGrab(const Cam& cam, std::uint64_t serial, Grab& out) {
    out.canvas = cam.canvas.load();
    out.element = cam.element.load();
    out.handle = ui::FrameHandle(cam.handle.load());
    out.startX = cam.startX.load();
    out.startY = cam.startY.load();
    unpackSize(cam.startView.load(), out.viewW, out.viewH);
    out.epoch = cam.epoch.load();
    out.loadGeneration = cam.loadGeneration.load();
    out.contentsGeneration = cam.contentsGeneration.load();
    return cam.serial.load() == serial;
}

bool UiInputComponent::beginDrag(Scene& scene, std::size_t camIndex, Cam& cam,
                                 const Grab& grab) {
    // 掴んだ時点から変わっていないか（INPUT の写しは 1 フレーム古いことがある）。
    const std::uint64_t loadGeneration = scene.ui().loadGeneration();
    const bool sameMode = grab.epoch == scene.editor().modeEpoch();
    const bool sameLoad = grab.loadGeneration == loadGeneration;
    if (!sameMode || !sameLoad || !scene.editsUiFrom(camIndex)) return false;
    ui::CanvasDesc desc;  // 要素は中身の持ち主（自分か、使っている UI）から
    wizengine::UiState::ContentsInfo info;
    if (!scene.ui().copyCanvasContents(grab.canvas, desc, info)) return false;  // 消えた
    // 掴んだ後（物理の次のパスまでの間）に中身を XML で入れ替えた・持ち主が替わった =
    // 同じ id でも別の要素（世代は作ったとき・入れ替えたときに新しい値になるので、別の UI に
    // 差し替えても違う値になる。共用にしたときだけは引き継ぐ = 同じ中身のまま）。
    if (info.contentsGeneration != grab.contentsGeneration) return false;
    const bool editable = wizengine::uiops::elementsEditable(info);
    if (!editable || !ui::frameShownOnCamera(desc, int(camIndex))) return false;
    const std::size_t at = ui::elementIndex(desc.elements, grab.element);
    if (at == desc.elements.size()) return false;  // 消えた
    cam.dragCanvas = grab.canvas;
    cam.dragElement = grab.element;
    cam.dragHandle = grab.handle;
    cam.dragParent = desc.elements[at].parent;
    cam.dragHeight = desc.height;
    cam.dragLoadGeneration = loadGeneration;
    cam.dragContentsGeneration = info.contentsGeneration;
    cam.dragEpoch = grab.epoch;
    cam.startX0 = grab.startX;
    cam.startY0 = grab.startY;
    // 開始時の Canvas と親の矩形（以後この値だけを見る = 解像度が途中で変わっても、
    // 押した位置からの動きの換算は変わらない）。
    cam.canvasRect = ui::canvasRect(desc.height, grab.viewW, grab.viewH);
    const std::vector<ui::LayoutNode> nodes = ui::layoutTree(desc.elements, {}, cam.canvasRect);
    const int parent = nodes[at].parent;
    cam.parentRect = parent >= 0 ? nodes[std::size_t(parent)].rect : cam.canvasRect;
    cam.start = desc.elements[at].props;
    cam.last = placementOf(cam.start);
    cam.moved = false;
    cam.wrotePos = cam.start.has(ui::Prop::Pos);
    cam.wroteSize = cam.start.has(ui::Prop::Size);
    return true;
}

bool UiInputComponent::applyDrag(Scene& scene, std::size_t camIndex, Cam& cam,
                                 std::uint64_t serial) {
    // 読込・全消し・シーンの XML の適用が入った（新しいシーンの同じ番号・id の要素に書かない）。
    if (scene.ui().loadGeneration() != cam.dragLoadGeneration) return false;
    // この中身を XML で丸ごと入れ替えた（同じ id の別の要素に書かない。ほかの Canvas は続く）。
    if (scene.ui().contentsGeneration(cam.dragCanvas) != cam.dragContentsGeneration) return false;
    const double curX = cam.x.load();
    const double curY = cam.y.load();
    // 読んでいる間に次の押下が来た（今の位置が新しい押下のもの）= このパスは当てない
    // （次のパスで新しいドラッグを始める）。
    if (cam.serial.load() != serial) return true;
    // 押した位置からの動き（設計の画素。y は下向き）。開始時の Canvas で換算する。
    double sx = 0.0, sy = 0.0, nx = 0.0, ny = 0.0;
    ui::ndcToCanvas(cam.canvasRect, cam.startX0, cam.startY0, sx, sy);
    ui::ndcToCanvas(cam.canvasRect, curX, curY, nx, ny);
    const double dx = nx - sx;
    const double dy = ny - sy;
    // 押しただけ（まだ動かしていない）では書かない（刻みへ寄せて値が変わらないように）。
    // 一度動いた後に押した位置へ戻したときは、戻した値を書く。
    if (!cam.moved && dx == 0.0 && dy == 0.0) return true;
    cam.moved = true;
    const double step = scene.editor().gizmo().snap ? ui::kSnapStepPx : ui::kFineStepPx;
    const ui::Placement next =
        ui::dragPlacement(cam.parentRect, cam.start, cam.dragHandle, dx, dy, step);
    if (next == cam.last) return true;  // 変わらなければ書かない（版を進めない）

    // 書くのは今の設計値の 1 枚（Inspector の編集などが間に入っていてもよい。pos / size
    // だけを開始時の値 + 動きで上書きする）。手順は Inspector の編集と同じ editElement /
    // writeOwnProps（種類が持つ性質だけを丸めて書く）。
    const ui::Placement from = placementOf(cam.start);
    const bool posChanged = next.pos[0] != from.pos[0] || next.pos[1] != from.pos[1];
    const bool sizeChanged = next.size[0] != from.size[0] || next.size[1] != from.size[1];
    // 開始時に書いてあった性質は書き続ける。開始時に書いていなかった性質は、開始時の値と
    // 違う間だけ書き、開始時の値へ戻したら書かない形（既定値）へ戻す。
    const bool writePos = posChanged || cam.start.has(ui::Prop::Pos);
    const bool writeSize = sizeChanged || cam.start.has(ui::Prop::Size);
    const bool clearPos = !writePos && cam.wrotePos;
    const bool clearSize = !writeSize && cam.wroteSize;
    ui::PropSet incoming;
    if (writePos) incoming.setNumber(ui::Prop::Pos, next.pos[0], next.pos[1]);
    if (writeSize) incoming.setNumber(ui::Prop::Size, next.size[0], next.size[1]);
    bool basisChanged = false;
    const wizengine::uiops::ElementWrite w = wizengine::uiops::editElement(
        scene.ui(), cam.dragCanvas, cam.dragElement,
        [&](ui::CanvasDesc& desc, std::size_t at) {
            ui::ElementDesc& e = desc.elements[at];
            // 開始時の値で計算した pos / size が、今の要素でも同じ場所を意味するか。
            const bool anchorKept = sameProp(e.props, cam.start, ui::Prop::Anchor);
            const bool pivotKept = sameProp(e.props, cam.start, ui::Prop::Pivot);
            const bool parentKept = e.parent == cam.dragParent;
            const bool canvasKept = desc.height == cam.dragHeight &&
                                    ui::frameShownOnCamera(desc, int(camIndex));
            if (!anchorKept || !pivotKept || !parentKept || !canvasKept) {
                basisChanged = true;
                return false;
            }
            wizengine::uiops::writeOwnProps(e, std::move(incoming));
            if (clearPos) e.props.clear(ui::Prop::Pos);
            if (clearSize) e.props.clear(ui::Prop::Size);
            return true;
        });
    // 消えた・編集してよくなくなった・置き方が変わった。
    if (!w.written || basisChanged) return false;
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
        cams_[c]->view.store(packSize(size.x, size.y));
    }
}
