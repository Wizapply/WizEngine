#include "components/TerrainComponent.h"

#include <cmath>
#include <limits>
#include <string>

#include "components/GizmoComponent.h"  // gizmo::kLineSetBrushRing / kRingSegments
#include "core/Clock.h"
#include "document/EditorTypes.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/SceneMath.h"

namespace ed = wizengine::editor;
namespace wt = wizengine::terrain;
using wizengine::secondsBetween;

namespace {

// 輪を地表から浮かせる高さ (m、ローカルの上向き)。地表と同じ高さだと面に埋もれて欠ける。
constexpr double kRingLift = 0.05;
// 筆の入力がこれだけ来なければ途絶えたと見なす (s)。
//   * ストローク: PHYSICS が閉じる（ブラウザが end を送れずに消えた = タブを閉じた・
//     通信が切れたときに開いたままにしない）。閉じるだけで、積まれた入力は捨てない。
//   * 輪: RENDER が消す（ツールを切った・タブを閉じたあとに残さない）。
constexpr double kStrokeTimeoutSeconds = 1.0;

// 画面の点（NDC）から引いたレイがテレインの地表に当たった点（ローカル）。
struct LocalHit {
    bool hit = false;
    double x = 0.0;
    double z = 0.0;
    double height = 0.0;  // heightAt（ローカルの高さ = Flatten の目標）
};

// x, y が [-1, 1] の外なら「画面の外」（ブラウザはポインタが出たとき 9 を送る）。
LocalHit hitTerrain(Scene& scene, std::size_t camIndex, double ndcX, double ndcY) {
    LocalHit out;
    if (!(std::abs(ndcX) <= 1.0 && std::abs(ndcY) <= 1.0)) return out;
    scenemath::Basis basis;
    scenemath::Vec3 dir;
    if (!scene.cameraRay(camIndex, ndcX, ndcY, basis, dir)) return out;
    // 格子は共有ポインタで受け取ってからロックの外で辿る（差し替えられても
    // 手元の格子は生きている）。
    wt::TerrainFrame frame;
    const std::shared_ptr<const wt::HeightField> field = scene.terrainField(&frame);
    if (!field) return out;
    double t = 0.0;
    wt::Vec3 normal = wt::Vec3::Zero();
    // 判定の戻り値は一度変数に受けてから使う（出力引数と同じ式に並べない規約）。
    const bool hit = frame.raycast(*field, basis.eye, dir,
                                   std::numeric_limits<double>::infinity(), t, normal);
    if (!hit) return out;
    const wt::Vec3 local = frame.toLocal(basis.eye + dir * t);
    out.hit = true;
    out.x = local.x();
    out.z = local.z();
    out.height = field->heightAt(out.x, out.z);
    return out;
}

}  // namespace

void TerrainComponent::setRing(const Ring& ring) {
    std::lock_guard<std::mutex> lk(ringMutex_);
    ring_ = ring;
}

void TerrainComponent::push(Pending::Kind kind, const wt::BrushStamp& stamp) {
    std::lock_guard<std::mutex> lk(pendingMutex_);
    pending_.push_back(Pending{kind, stamp});
}

void TerrainComponent::resetStroke() {
    stroking_ = false;
    havePrev_ = false;
    haveTarget_ = false;
}

bool TerrainComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    if (ed::jsonString(msg, "cmd", "") != "edit.terrain.brush") return false;
    // 筆はシーンを書き換える = エディタモード × エディタカメラのページ専用
    // （他の edit.* と同じ守り）。それ以外のぶんは消費して捨てる。エディタで
    // ないときは線分のつなぎも下ろす（シミュレートを往復したあと、begin の無い
    // move が古い始点・古い Flatten の目標で塗らないように）。他のカメラの
    // ページからのものは、エディタカメラで進行中のストロークに触らない。
    if (!scene.editor().isEditor()) {
        resetStroke();
        return true;
    }
    if (camIndex != scene.editorCamera()) return true;

    const Clock::time_point now = Clock::now();
    const std::string phase = ed::jsonString(msg, "phase", "hover");
    if (phase == "begin" || phase == "move" || phase == "end") {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        lastInput_ = now;
    }

    // 値の範囲は clampBrush の 1 か所（輪の半径も丸めた値で描く）。
    wt::BrushStamp s;
    s.tool = wt::brushToolFromName(ed::jsonString(msg, "tool", ""), s.tool);
    s.radius = ed::jsonNumber(msg, "radius", s.radius);
    s.strength = ed::jsonNumber(msg, "strength", s.strength);
    s = wt::clampBrush(s);

    const LocalHit h = hitTerrain(scene, camIndex, ed::jsonNumber(msg, "x", 9.0),
                                  ed::jsonNumber(msg, "y", 9.0));
    Ring ring;
    ring.visible = h.hit;
    ring.x = h.x;
    ring.z = h.z;
    ring.radius = s.radius;
    ring.seen = now;
    setRing(ring);

    if (phase == "end") {
        if (stroking_) push(Pending::Kind::End);
        resetStroke();
        return true;
    }
    // 線分の始点と Flatten の目標は begin から end まで保つ（入力が途絶えて
    // Scene がストロークを閉じても、続く move のスタンプで PHYSICS が開き直す）。
    if (phase == "begin") {
        resetStroke();
        stroking_ = true;
        push(Pending::Kind::Begin);
    } else if (phase != "move" || !stroking_) {
        return true;  // hover（と begin の無い move）は輪だけ
    }

    // 外れた点はスタンプにしない。次に当たった点は線分の始点からやり直す
    // （テレインの外を回り込んだ所まで線を引かない）。
    if (!h.hit) {
        havePrev_ = false;
        return true;
    }
    // Flatten の目標 = このストロークで最初に当たった点の高さ（ローカル）。
    if (!haveTarget_) {
        haveTarget_ = true;
        flattenTarget_ = h.height;
    }
    s.x1 = h.x;
    s.z1 = h.z;
    s.x0 = havePrev_ ? prevX_ : h.x;
    s.z0 = havePrev_ ? prevZ_ : h.z;
    // dt = 前の点からの時間。始点（begin・外れた後）は 0 = 高さの道具は書かず、
    // 穴の道具だけがその場で効く。
    s.dt = havePrev_ ? secondsBetween(prevTime_, now) : 0.0;
    s.target = flattenTarget_;
    // maxHeight は Scene::stampTerrain が今のテレインの値を入れる。
    push(Pending::Kind::Stamp, wt::clampBrush(s));
    havePrev_ = true;
    prevX_ = h.x;
    prevZ_ = h.z;
    prevTime_ = now;
    return true;
}

void TerrainComponent::onEditorStep(Scene& scene, double dt) {
    (void)dt;
    Clock::time_point last;
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        work_.swap(pending_);
        last = lastInput_;
    }
    // ストロークが開いているかの持ち主は Scene だけ（terrainStrokeActive）。
    // 積まれたものは捨てずに順に当てる（物理が遅れても 1 回のクリックを失わない）。
    batch_.clear();
    const auto flush = [&]() {
        if (batch_.empty()) return;
        // 開いていなければ開く: Scene が自分で閉じた（ストローク中の他の編集・
        // 取り消し・モード切替・形の変更）か、下の途絶えで閉じた後の続き。
        // Undo の段はそこで分かれる。
        if (!scene.terrainStrokeActive()) scene.beginTerrainStroke();
        scene.stampTerrain(batch_);
        batch_.clear();  // 容量は使い回す
    };
    for (const Pending& p : work_) {
        switch (p.kind) {
            case Pending::Kind::Begin:
                flush();  // 前のストロークのぶんは前のストロークへ
                scene.beginTerrainStroke();
                break;
            case Pending::Kind::Stamp:
                batch_.push_back(p.stamp);
                break;
            case Pending::Kind::End:
                flush();
                scene.endTerrainStroke();
                break;
        }
    }
    flush();
    work_.clear();

    // 入力が途絶えたら閉じるだけ（ブラウザが end を送れずに消えたとき、開いた
    // ままにしない）。続きの move が来れば flush が開き直す。
    if (secondsBetween(last, Clock::now()) > kStrokeTimeoutSeconds) scene.endTerrainStroke();
}

void TerrainComponent::onRender(Scene& scene) {
    Ring ring;
    {
        std::lock_guard<std::mutex> lk(ringMutex_);
        ring = ring_;
    }
    // エディタでない・入力が途絶えた輪は出さない。
    if (!scene.editor().isEditor() ||
        secondsBetween(ring.seen, Clock::now()) > kStrokeTimeoutSeconds) {
        ring.visible = false;
    }

    // applyToRenderer は objectsMutex_ を持ったまま呼ぶ。ここで取るのは
    // terrainField の末端のロックだけ（順序 objects → terrainField）。
    wt::TerrainFrame frame;
    std::shared_ptr<const wt::HeightField> field;
    if (ring.visible) {
        field = scene.terrainField(&frame);
        if (!field) ring.visible = false;
    }
    if (!ring.visible) {
        // 見えないときは空を 1 回だけ送る。
        if (!drawn_.visible) return;
        drawn_ = Ring{};
        drawnField_.reset();
        ringPoints_.clear();
        scene.renderer().setLineSet(gizmo::kLineSetBrushRing, ringPoints_);
        return;
    }
    // 前回と同じ中心・半径・格子・置き場所なら送らない。
    if (ring == drawn_ && field == drawnField_ && frame.origin == drawnFrame_.origin &&
        frame.rotation == drawnFrame_.rotation) {
        return;
    }
    drawn_ = ring;
    drawnField_ = field;
    drawnFrame_ = frame;

    // ローカルの円を地表に沿わせて（heightAt + 浮かせ）からワールドへ。
    ringPoints_.clear();
    ringPoints_.reserve(std::size_t(gizmo::kRingSegments) * 2);
    filament::math::float3 prev{};
    for (int i = 0; i <= gizmo::kRingSegments; ++i) {
        const double a = 2.0 * scenemath::kPi * double(i) / double(gizmo::kRingSegments);
        const double x = ring.x + ring.radius * std::cos(a);
        const double z = ring.z + ring.radius * std::sin(a);
        const wt::Vec3 w = frame.toWorld(wt::Vec3(x, field->heightAt(x, z) + kRingLift, z));
        const filament::math::float3 p{float(w.x()), float(w.y()), float(w.z())};
        if (i > 0) {
            ringPoints_.push_back(prev);
            ringPoints_.push_back(p);
        }
        prev = p;
    }
    scene.renderer().setLineSet(gizmo::kLineSetBrushRing, ringPoints_);
}
