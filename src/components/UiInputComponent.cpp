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
#include "ui/UiPlane.h"
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

// 映像の位置 (x, y)（NDC）が、選んでいる要素 rect のある Canvas desc（番号 index、全体の矩形
// canvas）の上のどこか（設計の座標 px, py）と、そこでの「映像の 1 画素 = 設計の何画素」
// （scale = つまみの当たりの大きさ。描く側 UiRenderComponent::updateFrame と同じ求め方）。
// overlay は映像全体に重なる。板（camera / world）はカーソルのレイと板の面の交点で、
// カーソルの下にもっと近い板があれば（Scene::plateUnderCursor = ボタンの当たりと同じ
// 決め方）ふさがれて false。板の矩形の外でも面に当たれば true（つまみは縁からはみ出す）。
bool pointOnCanvas(const Scene& scene, const wizengine::UiState::Snapshot& snap,
                   std::size_t camIndex, const ui::CanvasDesc& desc, int index,
                   const ui::Rect& canvas, const ui::Rect& rect, double x, double y, int viewW,
                   int viewH, double& px, double& py, double& scale) {
    if (!ui::isPlateCanvas(desc)) {
        ui::ndcToCanvas(canvas, x, y, px, py);
        scale = ui::designPerScreenPx(canvas.h, viewH);
        return true;
    }
    double distance = 0.0;
    if (!scene.canvasPointAt(desc, camIndex, x, y, px, py, distance)) return false;
    const Scene::UiPlatePoint front = scene.plateUnderCursor(snap, camIndex, x, y);
    const bool otherPlate = front.canvas >= 0 && front.canvas != index;
    const bool inFront = front.distance < distance;
    if (otherPlate && inFront) return false;
    return scene.plateDesignPerScreenPx(desc, camIndex, viewW, viewH, rect.x + rect.w * 0.5,
                                        rect.y + rect.h * 0.5, scale);
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
    wizengine::UiState::ElementRef dragRef;  // 掴んだ要素（開始時の世代。refValid で確かめる）
    int dragParent = -1;
    double dragHeight = 0.0;             // 開始時の Canvas の基準の高さ
    ui::CanvasMode dragMode = ui::CanvasMode::Overlay;  // 開始時の Canvas の表示方法
    std::uint64_t dragEpoch = 0;           // 掴んだときのモードの世代
    ui::FrameHandle dragHandle = ui::FrameHandle::None;
    // 板（camera / world）の上のドラッグは、開始時の板の面にレイを当て直して設計の座標を
    // 出す（途中で別の板の上に入っても、カメラに付いた板が動いても、掴んだ面のまま）。
    ui::CanvasPlane dragPlane;
    double startPx = 0.0, startPy = 0.0;  // 押した位置（設計の画素。開始時に求めた値）
    ui::Rect canvasRect;     // 開始時の Canvas 全体（overlay: NDC → 設計の画素の換算）
    ui::Rect parentRect;     // 開始時の親の矩形
    ui::PropSet start;       // 開始時の性質（設計値）
    ui::Placement last;      // 最後に書いた（または開始時の）pos / size
};

// 掴んだときに INPUT が置いた欄（PHYSICS が readGrab で 1 回だけ写す）。
struct UiInputComponent::Grab {
    wizengine::UiState::ElementRef ref;  // 掴んだ要素（INPUT の写しの世代）
    ui::FrameHandle handle = ui::FrameHandle::None;
    double startX = 0.0, startY = 0.0;
    int viewW = 0, viewH = 0;
    std::uint64_t epoch = 0;
};

UiInputComponent::UiInputComponent(std::size_t cameraCount) : pointer_(cameraCount) {
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
    // シミュレート中のボタン（エディタ中は下の選択・ドラッグ）。
    const bool pointerCmd = cmd == "pick" || cmd == "drag" || cmd == "release" || cmd == "hover";
    if (pointerCmd && camIndex < cams_.size()) {
        Cam& cam = *cams_[camIndex];
        // エディタの押下の印は、ボタンが pick を取る経路でも下ろす（取ると pick() へ届かない）。
        if (cmd == "pick") lowerHeld(cam);
        int viewW = 0, viewH = 0;
        unpackSize(cam.view.load(), viewW, viewH);
        if (pointer_.onCommand(scene, camIndex, cmd, msg, snapshot(scene), viewW, viewH)) {
            return true;
        }
    }
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
    lowerHeld(cam);
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
    if (!desc || !ui::canvasShownOnCamera(*desc, int(camIndex))) return false;
    int viewW = 0, viewH = 0;
    unpackSize(cam.view.load(), viewW, viewH);
    if (viewW <= 0 || viewH <= 0) return false;  // まだビューが無い

    const std::vector<ui::ElementDesc>& elements = snap.elementsOf(*desc);
    const std::size_t at = ui::elementIndex(elements, elementId);
    if (at == elements.size()) return false;
    const ui::Rect canvas = ui::designCanvasRect(*desc, viewW, viewH);
    const std::vector<ui::LayoutNode> nodes = ui::layoutTree(elements, {}, canvas);
    // 編集してよくない中身（読めなかった共用 UI など）・見えない要素は掴まない（押しても
    // 下へ流す。枠は描く側が線だけ出す）。
    const wizengine::UiState::ContentsInfo info = snap.contentsInfo(canvasIndex);
    const bool editable = wizengine::uiops::elementsEditable(info);
    if (!ui::frameGrabbable(editable, nodes[at])) return false;
    const double x = wizengine::jsonNumber(msg, "x", 0.0);
    const double y = wizengine::jsonNumber(msg, "y", 0.0);
    double px = 0.0, py = 0.0, scale = 1.0;
    if (!pointOnCanvas(scene, snap, camIndex, *desc, canvasIndex, canvas, nodes[at].rect, x, y,
                       viewW, viewH, px, py, scale)) {
        return false;
    }
    // つまみの当たりの大きさは画面の上で一定（描く四角より大きい。換算は buildFrameMesh と同じ）。
    const double half = ui::handleHitHalfDesign(scale);
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
    // 掴んだ要素の参照の世代（PHYSICS が UiState::refValid で確かめる）。
    const wizengine::UiState::ElementRef ref = snap.elementRef(canvasIndex, elementId);
    cam.loadGeneration.store(ref.loadGeneration);
    cam.contentsGeneration.store(ref.contentsGeneration);
    cam.held.store(true);
    cam.serial.fetch_add(1);  // 最後に進めて偶数に（PHYSICS はこれで開始を知る）
    // 物の掴み（自由移動）が同じフレームで動かないよう、カーソルを外す（ギズモと同じ）。
    scene.boxController(camIndex).clearPointer();
    return true;
}

void UiInputComponent::lowerHeld(Cam& cam) {
    // 残すと、この押下が UI に当たらなくても後の drag / release を食べ続け、ギズモが動かない。
    if (cam.held.exchange(false)) cam.released.store(cam.serial.load());
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
    out.ref.canvas = cam.canvas.load();
    out.ref.element = cam.element.load();
    out.handle = ui::FrameHandle(cam.handle.load());
    out.startX = cam.startX.load();
    out.startY = cam.startY.load();
    unpackSize(cam.startView.load(), out.viewW, out.viewH);
    out.epoch = cam.epoch.load();
    out.ref.loadGeneration = cam.loadGeneration.load();
    out.ref.contentsGeneration = cam.contentsGeneration.load();
    return cam.serial.load() == serial;
}

bool UiInputComponent::beginDrag(Scene& scene, std::size_t camIndex, Cam& cam,
                                 const Grab& grab) {
    // 掴んだ時点から変わっていないか（INPUT の写しは 1 フレーム古いことがある）: モードと、
    // 掴んだ要素の参照（読み直した・中身を XML で入れ替えた・持ち主が替わった・消えたら外れる
    // = UiState::refValid。共用にしたときは世代を引き継ぐので続く）。書くのはこのスレッドだけ
    // なので、確かめてから写すまでの間に変わらない。
    const bool sameMode = grab.epoch == scene.editor().modeEpoch();
    if (!sameMode || !scene.editsUiFrom(camIndex) || !scene.ui().refValid(grab.ref)) return false;
    ui::CanvasDesc desc;  // 要素は中身の持ち主（自分か、使っている UI）から
    wizengine::UiState::ContentsInfo info;
    if (!scene.ui().copyCanvasContents(grab.ref.canvas, desc, info)) return false;
    const bool editable = wizengine::uiops::elementsEditable(info);
    if (!editable || !ui::canvasShownOnCamera(desc, int(camIndex))) return false;
    const std::size_t at = ui::elementIndex(desc.elements, grab.ref.element);
    if (at == desc.elements.size()) return false;  // refValid の後なので起きない（念のため）
    // 開始時の Canvas と親の矩形（以後この値だけを見る = 解像度が途中で変わっても、
    // 押した位置からの動きの換算は変わらない）。
    cam.canvasRect = ui::designCanvasRect(desc, grab.viewW, grab.viewH);
    // 押した位置（設計の画素）。板は今のカメラのレイと板の面の交点で、その面を覚える。
    if (!ui::isPlateCanvas(desc)) {
        ui::ndcToCanvas(cam.canvasRect, grab.startX, grab.startY, cam.startPx, cam.startPy);
    } else {
        double distance = 0.0;
        if (!scene.canvasPointAt(desc, camIndex, grab.startX, grab.startY, cam.startPx,
                                 cam.startPy, distance, &cam.dragPlane)) {
            return false;  // 板が作れない・面と平行（掴んだ後にカメラが回った）
        }
    }
    cam.dragRef = grab.ref;
    cam.dragHandle = grab.handle;
    cam.dragParent = desc.elements[at].parent;
    cam.dragHeight = desc.height;
    cam.dragMode = desc.mode;
    cam.dragEpoch = grab.epoch;
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
    // 読込・全消し・シーンの XML の適用・この中身の XML の適用が入った・消えた（同じ番号・id の
    // 別の要素に書かない。ほかの Canvas のドラッグは続く）。
    if (!scene.ui().refValid(cam.dragRef)) return false;
    const double curX = cam.x.load();
    const double curY = cam.y.load();
    // 読んでいる間に次の押下が来た（今の位置が新しい押下のもの）= このパスは当てない
    // （次のパスで新しいドラッグを始める）。
    if (cam.serial.load() != serial) return true;
    // 今の位置（設計の画素。y は下向き）。開始時の Canvas（overlay）か開始時の板の面（板）で
    // 換算する。面と平行に近くて当たらないフレームは書かない（前の値のまま）。
    double nx = 0.0, ny = 0.0;
    if (!ui::isPlateCanvasMode(cam.dragMode)) {
        ui::ndcToCanvas(cam.canvasRect, curX, curY, nx, ny);
    } else {
        double distance = 0.0;
        if (!scene.planePointAt(cam.dragPlane, cam.dragHeight, camIndex, curX, curY, nx, ny,
                                distance)) {
            return true;
        }
    }
    // 押した位置からの動き。
    const double dx = nx - cam.startPx;
    const double dy = ny - cam.startPy;
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
        scene.ui(), cam.dragRef.canvas, cam.dragRef.element,
        [&](ui::CanvasDesc& desc, std::size_t at) {
            ui::ElementDesc& e = desc.elements[at];
            // 開始時の値で計算した pos / size が、今の要素でも同じ場所を意味するか。
            const bool anchorKept = sameProp(e.props, cam.start, ui::Prop::Anchor);
            const bool pivotKept = sameProp(e.props, cam.start, ui::Prop::Pivot);
            const bool parentKept = e.parent == cam.dragParent;
            // Canvas の基準の高さ・表示方法・出すカメラと、板なら板の縦横比（= Canvas の幅。
            // overlay の幅はビューの縦横比で決まり、開始時の値で換算し続ける）。
            const bool heightKept = desc.height == cam.dragHeight;
            const bool modeKept =
                desc.mode == cam.dragMode && ui::canvasShownOnCamera(desc, int(camIndex));
            const bool overlay = !ui::isPlateCanvasMode(cam.dragMode);
            const bool widthKept = overlay || ui::plateCanvasWidth(desc) == cam.canvasRect.w;
            const bool canvasKept = heightKept && modeKept && widthKept;
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
