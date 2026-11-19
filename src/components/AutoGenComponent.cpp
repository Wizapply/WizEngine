#include "components/AutoGenComponent.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "autogen/SceneDigest.h"
#include "core/Log.h"
#include "document/SceneDocument.h"
#include "render/Renderer.h"
#include "scene/EditorState.h"
#include "scene/Scene.h"

namespace ag = wizengine::autogen;

namespace {

// 依頼文の上限（文字数ではなくバイト。日本語で 1300 字ほど）。LLM への
// 1 往復で済む長さに抑える。
constexpr std::size_t kMaxPromptBytes = 4000;

// 見直しの撮影: 適用の Op を積んでから撮るまでの最短の待ち。物理スレッドが Op を
// 適用し（次のパス）、RENDER スレッドがレンダラブルを作り、glTF を読み込み、
// 環境マップを差し替えるまで。積み残しの Op がある間はさらに待つ。
constexpr auto kSettle = std::chrono::milliseconds(1500);
// ワーカーが撮影を待つ上限（描画が止まっている = 視聴者がいない間など）。
constexpr auto kSnapshotWait = std::chrono::seconds(20);

constexpr double kPi = 3.14159265358979323846;

// 視点（オービット）→ 目の位置。CameraObject と同じ式
// （azimuth は Y 軸まわり、0 で +Z の側から見る。elevation は水平から上向き）。
wizengine::Vec3d orbitEye(const wizengine::Vec3d& target, double azimuth,
                          double elevation, double radius) {
    return {target.x + radius * std::cos(elevation) * std::sin(azimuth),
            target.y + radius * std::sin(elevation),
            target.z + radius * std::cos(elevation) * std::cos(azimuth)};
}

struct ShotPlan {
    std::string view;  // LLM へ渡す説明（英語）
    wizengine::Vec3d eye, target;
};

// 撮る視点を決める: エディタカメラ（ユーザーが見ている絵）と、シーンの物を
// 囲む球に合わせた正面・横・上。物が 1 つも無ければ原点付近を見る。
std::vector<ShotPlan> planShots(const wizengine::editor::SceneDocument& doc,
                                std::size_t editorCam) {
    namespace ed = wizengine::editor;
    using wizengine::Vec3d;
    // 物の外接球（各物の中心 + 大きさの半分の対角）。
    Vec3d lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    int count = 0;
    for (const auto& b : doc.bodies) {
        const double r =
            b.shape == ed::ShapeKind::Box
                ? 0.5 * std::sqrt(b.size.x * b.size.x + b.size.y * b.size.y +
                                  b.size.z * b.size.z)
                : 0.5 * std::max(b.size.x, 0.1);
        lo = {std::min(lo.x, b.position.x - r), std::min(lo.y, b.position.y - r),
              std::min(lo.z, b.position.z - r)};
        hi = {std::max(hi.x, b.position.x + r), std::max(hi.y, b.position.y + r),
              std::max(hi.z, b.position.z + r)};
        ++count;
    }
    Vec3d center{0.0, 0.5, 0.0};
    double radius = 2.0;
    if (count > 0) {
        center = {(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5};
        const double dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
        radius = std::max(0.5 * std::sqrt(dx * dx + dy * dy + dz * dz), 0.5);
    }
    // 縦の画角 45°（Renderer::kFovDegrees）で球が収まる距離 = r / sin(22.5°) ≒ 2.61 r。
    const double dist = std::max(radius * 2.7, 1.5);
    const double deg = kPi / 180.0;

    std::vector<ShotPlan> plans;
    {
        ed::CameraPose cam;
        if (editorCam < doc.cameras.size()) cam = doc.cameras[editorCam];
        else if (!doc.cameras.empty()) cam = doc.cameras.front();
        plans.push_back({"the editor camera (the view the user sees first)",
                         orbitEye(cam.target, cam.azimuth, cam.elevation, cam.radius),
                         cam.target});
    }
    plans.push_back({"front, from +Z looking toward -Z (image right = +X, image up = +Y)",
                     orbitEye(center, 0.0, 15.0 * deg, dist), center});
    plans.push_back({"right side, from +X looking toward -X (image right = -Z, image up = +Y)",
                     orbitEye(center, 90.0 * deg, 15.0 * deg, dist), center});
    plans.push_back({"top, from above looking down (image up = -Z, image right = +X)",
                     orbitEye(center, 0.0, 80.0 * deg, dist), center});
    return plans;
}

}  // namespace

AutoGenComponent::AutoGenComponent(ag::AiConfig cfg) : generator_(std::move(cfg)) {
    const auto& c = generator_.config();
    if (c.llmConfigured()) {
        LOGI("ai", "scene generation: LLM %s (%s), text-to-3d %s",
             c.llmModel.c_str(), c.llmUrl.c_str(),
             c.meshyConfigured() ? c.meshyUrl.c_str() : "off (no MESHY key)");
    } else {
        LOGI("ai", "scene generation: off (set ai.llm.model in config.json to enable)");
    }
}

bool AutoGenComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    const std::string cmd = msg.value("cmd", "");
    if (cmd != "ai.generate" && cmd != "ai.cancel") return false;
    EditorState& state = scene.editor();
    // シーンを書き換える操作 = エディタモード × エディタカメラのページだけ。
    if (camIndex != scene.editorCamera() || !state.isEditor()) {
        state.setStatus("AI シーン生成はエディタモードの Editor Camera のページで使えます");
        return true;
    }

    if (cmd == "ai.cancel") {
        generator_.cancel();
        return true;
    }

    ag::GenRequest req;
    req.prompt = (msg.contains("prompt") && msg["prompt"].is_string())
                     ? msg["prompt"].get<std::string>()
                     : std::string();
    if (req.prompt.size() > kMaxPromptBytes) req.prompt.resize(kMaxPromptBytes);
    req.mode = ag::genModeFromName(
        (msg.contains("mode") && msg["mode"].is_string()) ? msg["mode"].get<std::string>()
                                                          : std::string(),
        ag::GenMode::New);
    req.allowMeshes = !(msg.contains("meshes") && msg["meshes"].is_boolean() &&
                        !msg["meshes"].get<bool>());
    if (req.prompt.empty()) {
        state.setStatus("AI: 依頼文が空です");
        return true;
    }
    const std::size_t editorCam = scene.editorCamera();

    // 選んでいる物（エディタカメラのページの選択）を、文書の中の番号（削除済みを
    // 詰めた番号 = document() の bodies の添字）に直す。ロックは数えるときだけ。
    int selectedDocIndex = -1;
    const bool useSelection = !(msg.contains("useSelection") &&
                                msg["useSelection"].is_boolean() &&
                                !msg["useSelection"].get<bool>());
    if (useSelection && req.mode != ag::GenMode::New) {
        const std::size_t sel = scene.boxController(editorCam).selected();
        auto lk = scene.lockObjects();
        if (scene.objectAlive(sel)) {
            int k = 0;
            for (std::size_t i = 0; i < sel; ++i) {
                if (scene.objectAlive(i)) ++k;
            }
            selectedDocIndex = k;
        }
    }

    // 今のシーン（INPUT スレッドで写す。document は自分でロックを取る）。名前の無い
    // 物には LLM が名前で指せるよう "body<番号>" を付けた写しを送る（編集モードでは
    // 返ってきた文書にそのまま残る = 適用後は名前付きになる）。
    namespace ed = wizengine::editor;
    ed::SceneDocument doc = scene.document();
    if (req.mode != ag::GenMode::New) ag::nameUnnamedBodies(doc);

    // 依頼文の @名前 を確かめる（無い名前のまま LLM へ送ると、似た名前の別の物を
    // 黙って書き換えるか、新しく作ってしまう）。
    const ag::Mentions mentions = ag::resolveMentions(req.prompt, ag::mentionableNames(doc));
    if (req.mode == ag::GenMode::New && (!mentions.found.empty() || !mentions.unknown.empty())) {
        state.setStatus("AI: 「新規」では今のシーンの物を @ で指せません（「編集」か「追加」を選んでください）");
        return true;
    }
    if (!mentions.unknown.empty()) {
        std::string list;
        for (std::size_t i = 0; i < mentions.unknown.size() && i < 5; ++i) {
            list += (i ? "、@" : "@") + mentions.unknown[i];
        }
        state.setStatus("AI: シーンに無い名前です: " + list +
                        "（オブジェクト・ジョイント・ライトの名前を @ の後に書いてください）");
        return true;
    }
    if (req.mode != ag::GenMode::New) {
        req.currentXml = ed::toXmlText(doc);
        req.editorCamera = int(editorCam);
        if (selectedDocIndex >= 0 && std::size_t(selectedDocIndex) < doc.bodies.size()) {
            req.focus.push_back(doc.bodies[std::size_t(selectedDocIndex)].name);
        }
    }

    // 見直しの撮影（ai.vision.rounds が 0 なら渡さない = 見直さない）。ワーカー
    // スレッドから呼ばれ、RENDER スレッドが撮り終えるまで待つ。
    ag::SnapshotFn snapshot;
    if (generator_.config().visionRounds > 0) {
        auto q = snap_;
        const int w = generator_.config().visionWidth;
        const int h = std::max(2, (w * 9 / 16) & ~1);  // 16:9（偶数）
        snapshot = [q, w, h](ag::SnapshotSet& out, std::string& err) {
            std::unique_lock<std::mutex> lk(q->mutex);
            if (q->closed) {
                err = "shutting down";
                return false;
            }
            q->requested = true;
            q->done = false;
            q->error.clear();
            q->result = ag::SnapshotSet{};
            q->width = w;
            q->height = h;
            q->notBefore = std::chrono::steady_clock::now() + kSettle;
            const bool woke =
                q->cv.wait_for(lk, kSnapshotWait, [&] { return q->done || q->closed; });
            if (!woke || !q->done) {
                q->requested = false;
                err = q->closed ? "shutting down"
                                : "the render thread did not take the snapshot in time";
                return false;
            }
            q->done = false;
            if (!q->error.empty()) {
                err = q->error;
                return false;
            }
            out = std::move(q->result);
            return true;
        };
    }
    const ag::GenMode mode = req.mode;  // req は start へ渡す（move）ので先に写す
    std::string whyNot;
    const bool started = generator_.start(
        std::move(req),
        // ワーカースレッドから。物理スレッドへ Op を積むだけ（EditorState の
        // キューはスレッド安全）。適用の結果（警告の件数）は物理スレッドが
        // ステータスに出す。
        [&state, editorCam](const ag::GenOutcome& res) {
            EditorState::Op op;
            op.kind = res.mode == ag::GenMode::Append ? "xml.append" : "xml";
            op.args = {{"text", res.xml}};
            op.camera = editorCam;
            state.push(std::move(op));
        },
        whyNot, std::move(snapshot));
    if (started) {
        std::string what = "AI: シーンを生成しています...";
        if (mode != ag::GenMode::New) {
            // 何を「指している」と受け取ったかを見せる（ずれていたらすぐ分かる）。
            std::string pointed;
            if (selectedDocIndex >= 0 && std::size_t(selectedDocIndex) < doc.bodies.size()) {
                pointed = "選択中 " + doc.bodies[std::size_t(selectedDocIndex)].name;
            }
            for (const auto& m : mentions.found) {
                pointed += (pointed.empty() ? "" : "、") + std::string("@") + m;
            }
            if (!pointed.empty()) what += "（" + pointed + "）";
        }
        state.setStatus(what);
    } else {
        state.setStatus("AI: " + whyNot);
    }
    return true;
}

void AutoGenComponent::onRenderUnlocked(Scene& scene) {
    int width = 0, height = 0;
    {
        std::lock_guard<std::mutex> lk(snap_->mutex);
        if (!snap_->requested || snap_->closed) return;
        if (std::chrono::steady_clock::now() < snap_->notBefore) return;
        width = snap_->width;
        height = snap_->height;
    }
    // 適用の Op がまだ物理スレッドに積まれている = シーンが変わる前。次のフレームへ。
    if (scene.editor().hasPending()) return;

    // 撮る（この中はロックを持たない: 撮影は GPU を待つので数十 ms かかる）。
    // document() はオブジェクト一覧のロックを自分で取る（onRenderUnlocked は
    // そのロックの外 = 取ってよい）。
    namespace ed = wizengine::editor;
    ag::SnapshotSet set;
    std::string error;
    const ed::SceneDocument doc = scene.document();
    set.sceneXml = ed::toXmlText(doc);
    for (const ShotPlan& plan : planShots(doc, scene.editorCamera())) {
        ag::Snapshot shot;
        shot.view = plan.view;
        shot.width = width;
        shot.height = height;
        const bool ok = scene.renderer().captureSnapshot(
            {plan.eye.x, plan.eye.y, plan.eye.z},
            {plan.target.x, plan.target.y, plan.target.z}, width, height, shot.rgba);
        if (!ok) {
            error = "the renderer could not take the snapshot";
            break;
        }
        set.shots.push_back(std::move(shot));
    }
    LOGI("ai", "vision review: took %zu snapshot(s) at %dx%d", set.shots.size(), width,
         height);

    std::lock_guard<std::mutex> lk(snap_->mutex);
    if (!snap_->requested) return;  // 待ちきれずに諦めた（結果は捨てる）
    snap_->requested = false;
    snap_->done = true;
    snap_->error = error;
    snap_->result = std::move(set);
    snap_->cv.notify_all();
}

void AutoGenComponent::shutdown() {
    {
        std::lock_guard<std::mutex> lk(snap_->mutex);
        snap_->closed = true;
        snap_->requested = false;
    }
    snap_->cv.notify_all();
    generator_.shutdown();
}
