#include "components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "core/Clock.h"
#include "document/EditorTypes.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/SceneMath.h"

namespace ed = wizengine::editor;
namespace wt = wizengine::terrain;
using wizengine::secondsBetween;

namespace {

// 輪と円盤を地表から浮かせる高さ (m、ローカルの上向き)。地表と同じ高さだと面に埋もれて欠ける。
constexpr double kRingLift = 0.05;
// 筆の入力がこれだけ来なければ途絶えたと見なす (s)。
//   * ストローク: PHYSICS が閉じる（ブラウザが end を送れずに消えた = タブを閉じた・
//     通信が切れたときに開いたままにしない）。閉じるだけで、積まれた入力は捨てない。
//   * 輪: RENDER が消す（ツールを切った・タブを閉じたあとに残さない）。
constexpr double kStrokeTimeoutSeconds = 1.0;
// 筆のカーソルの色（輪と円盤。地表の緑・灰の上で目立つ橙）。
const filament::math::float3 kBrushColor{1.00f, 0.55f, 0.10f};
// 円盤の中心の不透明度（縁へ向かって筆の減衰 brushFalloff と同じ形で 0 へ）。
constexpr double kDiscAlpha = 0.35;
// 円盤の同心円の本数（頂点は 1 + 16 × 48 = 769 個）。半径によらず同じ本数に
// して間隔で大きさを表す = 頂点数が変わらず、半径を変えてもレンダラブルを
// 作り直さない（Renderer::setOverlayMesh は数が変わると作り直す）。
constexpr int kDiscRings = 16;

// 画面の点（NDC）から引いたレイがテレインの地表に当たった点（ローカル）。
struct LocalHit {
    bool hit = false;
    double x = 0.0;
    double z = 0.0;
    double height = 0.0;  // 当たった点のローカルの高さ（= heightAt、Flatten の目標）
    // 外れたときも、レイが引けてテレインがあれば持つ（edgePoint が使う）。
    bool hasRay = false;
    wt::Vec3 eye = wt::Vec3::Zero();  // レイの始点（ローカル）
    wt::Vec3 dir = wt::Vec3::Zero();  // レイの向き（ローカル）
    std::shared_ptr<const wt::HeightField> field;
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
    out.field = scene.terrainField(&frame);
    if (!out.field) return out;
    out.hasRay = true;
    out.eye = frame.toLocal(basis.eye);
    out.dir = frame.dirToLocal(dir);
    double t = 0.0;
    wt::Vec3 normal = wt::Vec3::Zero();
    // 判定の戻り値は一度変数に受けてから使う（出力引数と同じ式に並べない規約）。
    const bool hit = frame.raycast(*out.field, basis.eye, dir,
                                   std::numeric_limits<double>::infinity(), t, normal);
    if (!hit) return out;
    // 回転で長さは変わらない = ローカルのレイでも同じ t。当たった点は地表の上。
    const wt::Vec3 local = out.eye + out.dir * t;
    out.hit = true;
    out.x = local.x();
    out.z = local.z();
    out.height = local.y();
    return out;
}

// 外れた点の最後の 1 区間が、前の点から筆の半径の何倍まで伸びてよいか。1 回の
// 入力の間（約 33 ms）にカーソルが縁を越えて動くのはこの程度までで、これより
// 遠い終点はカーソルの下ではない（低い角度のレイが空へ抜けて交点が数百 m 先に
// なった、など）= 触れていない所まで線を引かないために積まない。
constexpr double kMaxEdgeReachRadii = 2.0;

// 外れた点の最後の 1 区間の終点（テレインの外へ出た・外で離したとき、縁まで
// 塗るため）。レイと「前の点の高さ (prevY) の水平面」の交点が格子の外にある
// ときだけ、前の点 → 交点の線分が格子の矩形から出る点を返す。次のときは false
// （積まない）: レイが無い・面と交わらない、交点が格子の内側（穴の上・山の
// 輪郭をかすめた = 外へ出たのではない）、終点が前の点から半径 ×
// kMaxEdgeReachRadii より遠い。
bool edgePoint(const LocalHit& h, double prevX, double prevY, double prevZ, double radius,
               double& x, double& z) {
    if (!h.hasRay) return false;
    const double t = scenemath::rayHitsGround(h.eye, h.dir, prevY);
    if (!(t > 0.0)) return false;
    const wt::Vec3 p = h.eye + h.dir * t;
    const wt::HeightField& f = *h.field;
    if (f.contains(p.x(), p.z())) return false;
    // 交点（外）から前の点（内）へ向かう線分が矩形に入る点 = 前の点から見て
    // 矩形を出る点。矩形は高さを持たないので y は 0 にそろえて箱の判定に通す。
    const double hx = 0.5 * (f.maxX() - double(f.minX()));
    const double hz = 0.5 * (f.maxZ() - double(f.minZ()));
    const BodyTransform rect{double(f.minX()) + hx, 0.0, double(f.minZ()) + hz,
                             1.0, 0.0, 0.0, 0.0};
    const wt::Vec3 from(p.x(), 0.0, p.z());
    const wt::Vec3 toPrev(prevX - p.x(), 0.0, prevZ - p.z());
    const double s = scenemath::rayHitsBox(from, toPrev, rect, wt::Vec3(hx, 1.0, hz));
    if (!(s >= 0.0 && s <= 1.0)) return false;
    x = p.x() + toPrev.x() * s;
    z = p.z() + toPrev.z() * s;
    return std::hypot(x - prevX, z - prevZ) <= radius * kMaxEdgeReachRadii;
}

// ローカルの (x, z) の地表（+ 浮かせ）のワールドの点。輪と円盤が共用。
filament::math::float3 surfacePoint(const wt::HeightField& field, const wt::TerrainFrame& frame,
                                    double x, double z) {
    const wt::Vec3 w = frame.toWorld(wt::Vec3(x, field.heightAt(x, z) + kRingLift, z));
    return {float(w.x()), float(w.y()), float(w.z())};
}

// 筆の円盤（中心 + 同心円 kDiscRings 本 × kCircleSegments 点、扇と帯の三角形）を
// 地表に沿わせて組む。色の不透明度は中心 kDiscAlpha → 縁 0（brushFalloff）。最後の
// kCircleSegments 個の頂点が縁の円（輪の線はここから作る）。
void buildDisc(const wt::HeightField& field, const wt::TerrainFrame& frame, double cx,
               double cz, double radius, std::vector<filament::math::float3>& positions,
               std::vector<filament::math::float4>& colors, std::vector<uint16_t>& indices) {
    constexpr int S = scenemath::kCircleSegments;
    constexpr int m = kDiscRings;
    positions.clear();
    colors.clear();
    indices.clear();
    const auto add = [&](double x, double z, double d) {
        positions.push_back(surfacePoint(field, frame, x, z));
        colors.push_back(filament::math::float4(
            kBrushColor, float(kDiscAlpha * wt::brushFalloff(d, radius))));
    };
    add(cx, cz, 0.0);
    for (int k = 1; k <= m; ++k) {
        const double d = radius * double(k) / double(m);
        for (int i = 0; i < S; ++i) {
            const double a = 2.0 * scenemath::kPi * double(i) / double(S);
            add(cx + d * std::cos(a), cz + d * std::sin(a), d);
        }
    }
    // 円 k（1 始まり）の i 番目の頂点。
    const auto at = [&](int k, int i) { return uint16_t(1 + (k - 1) * S + (i % S)); };
    for (int i = 0; i < S; ++i) indices.insert(indices.end(), {0, at(1, i), at(1, i + 1)});
    for (int k = 2; k <= m; ++k) {
        for (int i = 0; i < S; ++i) {
            const uint16_t a = at(k - 1, i), b = at(k - 1, i + 1);
            const uint16_t c = at(k, i), d = at(k, i + 1);
            indices.insert(indices.end(), {a, c, d, a, d, b});
        }
    }
}

// 円盤の縁の円（最後の kCircleSegments 頂点）を輪の線分（2 点で 1 本）にする。
void ringFromDisc(const std::vector<filament::math::float3>& disc,
                  std::vector<filament::math::float3>& lines) {
    constexpr std::size_t S = std::size_t(scenemath::kCircleSegments);
    lines.clear();
    if (disc.size() < S) return;
    const std::size_t first = disc.size() - S;
    for (std::size_t i = 0; i < S; ++i) {
        lines.push_back(disc[first + i]);
        lines.push_back(disc[first + (i + 1) % S]);
    }
}

}  // namespace

void TerrainComponent::setRing(const Ring& ring) {
    std::lock_guard<std::mutex> lk(ringMutex_);
    ring_ = ring;
}

void TerrainComponent::push(Pending::Kind kind, const wt::BrushStamp& stamp) {
    std::lock_guard<std::mutex> lk(pendingMutex_);
    pending_.push_back(Pending{kind, stamp, seenEpoch_});
}

void TerrainComponent::resetStroke() {
    stroking_ = false;
    havePrev_ = false;
    haveTarget_ = false;
}

void TerrainComponent::pushSegment(wt::BrushStamp s, double x, double z,
                                   Clock::time_point now) {
    s.x1 = x;
    s.z1 = z;
    s.x0 = havePrev_ ? prevX_ : x;
    s.z0 = havePrev_ ? prevZ_ : z;
    // dt = 前の点からの時間。始点（begin・外れた後）は 0 = 高さの道具は書かず、
    // 穴の道具だけがその場で効く。
    s.dt = havePrev_ ? secondsBetween(prevTime_, now) : 0.0;
    s.target = flattenTarget_;
    // maxHeight は Scene::stampTerrain が今のテレインの値を入れる。
    push(Pending::Kind::Stamp, wt::clampBrush(s));
    prevX_ = x;
    prevZ_ = z;
    prevTime_ = now;
}

bool TerrainComponent::onCommand(Scene& scene, std::size_t camIndex,
                                 const nlohmann::json& msg) {
    if (ed::jsonString(msg, "cmd", "") != "edit.terrain.brush") return false;
    // 前回の入力からモードが切り替わっていたら（筆の入力が来ないままの往復
    // でも）、線分のつなぎと Flatten の目標を下ろす: begin の無い move が古い
    // 始点・古い目標で塗らないように。世代はモードを見る前に読む（読んだ後に
    // 切り替わっても、積んだぶんは古い世代として PHYSICS が捨てる）。
    const std::uint64_t epoch = scene.editor().modeEpoch();
    if (epoch != seenEpoch_) {
        seenEpoch_ = epoch;
        resetStroke();
    }
    // 筆はシーンを書き換える = エディタモード × エディタカメラのページ専用
    // （他の edit.* と同じ守り）。それ以外のぶんは消費して捨てる。他のカメラの
    // ページからのものは、エディタカメラで進行中のストロークに触らない。
    if (!scene.editor().isEditor()) return true;
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
    setRing(Ring{h.hit, h.x, h.z, s.radius, now});

    // end は離した点（x, y）を添えて来る: 前の点からそこまでを move と同じスタンプに
    // してから End を積む（別の move で送ると、届く順が end と入れ替わりうる）。
    // テレインの外で離した点は下の「外れた点」と同じく縁までの 1 区間、画面の外
    // （位置の無い）end は End だけ。begin を受けていない end は何も積まない。
    const bool ending = phase == "end";
    if (ending && !stroking_) {
        resetStroke();
        return true;
    }
    // 線分の始点と Flatten の目標は begin から end まで保つ（入力が途絶えて
    // Scene がストロークを閉じても、続く move のスタンプで PHYSICS が開き直す）。
    if (phase == "begin") {
        resetStroke();
        stroking_ = true;
        push(Pending::Kind::Begin);
    } else if (!ending && (phase != "move" || !stroking_)) {
        return true;  // hover（と begin の無い move）は輪だけ
    }

    if (h.hit) {
        // Flatten の目標 = このストロークで最初に当たった点の高さ（ローカル）。
        if (!haveTarget_) {
            haveTarget_ = true;
            flattenTarget_ = h.height;
        }
        pushSegment(s, h.x, h.z, now);
        havePrev_ = true;
        prevY_ = h.height;
    } else {
        // 外れた点: テレインの外へ出たのなら縁までを最後の 1 区間として塗る
        // （edgePoint）。その点は次の始点に残さない（外を回り込んだ所まで線を
        // 引かない）。
        double ex = 0.0, ez = 0.0;
        const bool edge = havePrev_ && edgePoint(h, prevX_, prevY_, prevZ_, s.radius, ex, ez);
        if (edge) pushSegment(s, ex, ez, now);
        havePrev_ = false;
    }
    if (ending) {
        push(Pending::Kind::End);
        resetStroke();
    }
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
    // 今の世代の積まれたものは捨てずに順に当てる（物理が遅れても 1 回の
    // クリックを失わない）。
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
    // 今の世代でないもの = モードが切り替わる前に積まれたものは捨てる
    // （エディタ中に世代が変わるのは applyPendingEdits = このスレッドだけ）。
    const std::uint64_t epoch = scene.editor().modeEpoch();
    for (const Pending& p : work_) {
        if (p.epoch != epoch) continue;
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

void TerrainComponent::hideCursor(wizengine::Renderer& renderer) {
    // 見えないときは空を 1 回だけ送る。
    if (!drawn_.visible) return;
    drawn_ = Ring{};
    drawnField_.reset();
    ringPoints_.clear();
    discPositions_.clear();
    discColors_.clear();
    discIndices_.clear();
    renderer.setLineSet(ringSet_, ringPoints_);
    renderer.setOverlayMesh(discMesh_, discPositions_, discColors_, discIndices_);
}

void TerrainComponent::onRender(Scene& scene) {
    wizengine::Renderer& renderer = scene.renderer();
    // 輪の細線セットと円盤は自分で作る（RENDER スレッドで 1 回だけ）。
    if (!cursorMade_) {
        ringSet_ = renderer.addLineSet(kBrushColor);
        discMesh_ = renderer.addOverlayMesh();
        cursorMade_ = true;
    }
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
        hideCursor(renderer);
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

    // 円盤を地表に沿わせて組み、その縁の円を輪の線にする。
    buildDisc(*field, frame, ring.x, ring.z, ring.radius, discPositions_, discColors_,
              discIndices_);
    ringFromDisc(discPositions_, ringPoints_);
    renderer.setOverlayMesh(discMesh_, discPositions_, discColors_, discIndices_);
    renderer.setLineSet(ringSet_, ringPoints_);
}
