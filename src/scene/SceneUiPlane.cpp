// UI の板（camera / world の Canvas）を、カメラから見た板と、カーソルのレイが当たる所に
// 直す（Scene のメンバの実装だけ）。板の置き方の式は ui/UiPlane.h、オイラー角 → 向きと
// カメラの基底・レイ・画面への投影は scene/SceneMath.h（どちらも唯一の定義）。描画
// （UiRenderComponent）と当たり（UiPointerInput・UiInputComponent）が同じ板を使うための 1 か所。
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>

#include "render/Renderer.h"  // 画角と縦横比（投影）

namespace ui = wizengine::ui;

namespace {

wizengine::Vec3d toVec(const scenemath::Vec3& v) { return {v.x(), v.y(), v.z()}; }
scenemath::Vec3 fromVec(const wizengine::Vec3d& v) { return {v.x, v.y, v.z}; }

// 板の大きさと基準の高さが正か（0 の板は置けない）。
bool plateSized(const ui::CanvasDesc& desc) {
    return desc.sizeX > 0.0 && desc.sizeY > 0.0 && desc.height > 0.0;
}

// 板（world は向きから、camera はカメラの基底 basis から）。basis は camera のときだけ使う。
bool planeFrom(const ui::CanvasDesc& desc, const scenemath::Basis& basis, ui::CanvasPlane& out) {
    if (!plateSized(desc)) return false;
    if (desc.mode == ui::CanvasMode::World) {
        const scenemath::Quat q =
            scenemath::quatFromEulerDegrees(desc.rot.x, desc.rot.y, desc.rot.z);
        out = ui::worldCanvasPlane(desc, toVec(q * scenemath::Vec3::UnitX()),
                                   toVec(q * scenemath::Vec3::UnitY()));
        return true;
    }
    if (desc.mode != ui::CanvasMode::Camera || !basis.valid) return false;
    out = ui::cameraCanvasPlane(desc, toVec(basis.eye), toVec(basis.forward), toVec(basis.right),
                                toVec(basis.up));
    return true;
}

// 視点 basis.eye から向き dir のレイと板 plane の交点の設計の座標と距離。後ろ・平行は false。
bool hitPlane(const scenemath::Basis& basis, const scenemath::Vec3& dir,
              const ui::CanvasPlane& plane, double designHeight, double& px, double& py,
              double& distance) {
    const double t = scenemath::rayHitsPlane(basis.eye, dir, fromVec(plane.center),
                                             fromVec(ui::planeNormal(plane)));
    if (t < 0.0) return false;
    ui::designPointOnPlane(plane, designHeight, toVec(basis.eye + dir * t), px, py);
    distance = t;
    return true;
}

// 板の選択枠のつまみの大きさを測る、設計の画素での小さな刻み（Canvas の基準の高さに対する
// 割合。投影した 2 点の間の画面の長さから「映像の 1 画素 = 設計の何画素」を出す）。
constexpr double kScaleProbeFraction = 0.01;

}  // namespace

bool Scene::canvasPlane(const ui::CanvasDesc& desc, std::size_t cameraIndex,
                        ui::CanvasPlane& out) const {
    if (cameraIndex >= cameras_.size()) return false;
    // カメラの基底を読むのは camera の板のときだけ（world はカメラに依らない）。
    const bool camera = desc.mode == ui::CanvasMode::Camera;
    const scenemath::Basis basis =
        camera ? scenemath::cameraBasis(*cameras_[cameraIndex]) : scenemath::Basis{};
    return planeFrom(desc, basis, out);
}

bool Scene::canvasPointAt(const ui::CanvasDesc& desc, std::size_t cameraIndex, double ndcX,
                          double ndcY, double& px, double& py, double& distance,
                          ui::CanvasPlane* plane) const {
    // カメラの基底は 1 回だけ読み（cameraRay）、レイと camera の板の両方をそれから作る
    // （別々に読むと、間でカメラが動いたときに板とレイが別の視点になる）。
    scenemath::Basis basis;
    scenemath::Vec3 dir;
    if (!cameraRay(cameraIndex, ndcX, ndcY, basis, dir)) return false;
    ui::CanvasPlane made;
    if (!planeFrom(desc, basis, made)) return false;
    if (!hitPlane(basis, dir, made, desc.height, px, py, distance)) return false;
    if (plane) *plane = made;
    return true;
}

bool Scene::planePointAt(const ui::CanvasPlane& plane, double designHeight,
                         std::size_t cameraIndex, double ndcX, double ndcY, double& px,
                         double& py, double& distance) const {
    scenemath::Basis basis;
    scenemath::Vec3 dir;
    if (!cameraRay(cameraIndex, ndcX, ndcY, basis, dir)) return false;
    return hitPlane(basis, dir, plane, designHeight, px, py, distance);
}

Scene::UiPlatePoint Scene::plateUnderCursor(const wizengine::UiState::Snapshot& snap,
                                            std::size_t cameraIndex, double ndcX,
                                            double ndcY) const {
    // カーソルの下にある板（枠の中）のうち、カメラにいちばん近い板だけ = 手前の板は
    // そこにボタンや要素が無くても後ろの板をふさぐ。
    UiPlatePoint nearest;
    for (std::size_t i = 0; i < snap.canvases.size(); ++i) {
        const ui::CanvasDesc* desc = snap.liveCanvas(int(i));
        const bool plate = desc && ui::isPlateCanvas(*desc);
        if (!plate || !ui::canvasShownOnCamera(*desc, int(cameraIndex))) continue;
        double px = 0.0, py = 0.0, distance = 0.0;
        if (!canvasPointAt(*desc, cameraIndex, ndcX, ndcY, px, py, distance)) continue;
        const bool onPlate = ui::plateCanvasRect(*desc).contains(px, py);
        const bool nearer = nearest.canvas < 0 || distance < nearest.distance;
        if (!onPlate || !nearer) continue;
        nearest = {int(i), px, py, distance};
    }
    return nearest;
}

bool Scene::plateToOverlay(const ui::CanvasDesc& desc, std::size_t cameraIndex,
                           const ui::Rect& overlay,
                           std::vector<std::array<double, 2>>& points) const {
    if (cameraIndex >= cameras_.size()) return false;
    // 板と投影は同じカメラの読みから（canvasPointAt と同じ理由）。
    const scenemath::Basis basis = scenemath::cameraBasis(*cameras_[cameraIndex]);
    if (!basis.valid) return false;
    ui::CanvasPlane plane;
    if (!planeFrom(desc, basis, plane)) return false;
    const double fov = renderer_.verticalFovDegrees();
    const double aspect = renderer_.aspect();
    for (auto& p : points) {
        const wizengine::Vec3d world = ui::worldPointOnPlane(plane, desc.height, p[0], p[1]);
        double ndcX = 0.0, ndcY = 0.0;
        if (!scenemath::projectToNdc(basis, fromVec(world), fov, aspect, ndcX, ndcY)) return false;
        ui::ndcToCanvas(overlay, ndcX, ndcY, p[0], p[1]);
    }
    return true;
}

bool Scene::plateDesignPerScreenPx(const ui::CanvasDesc& desc, std::size_t cameraIndex,
                                   int viewWidth, int viewHeight, double px, double py,
                                   double& scale) const {
    if (viewWidth <= 0 || viewHeight <= 0) return false;
    // 映像の画素の座標（overlay の Canvas の高さ = ビューの高さの画素）へ、点と、そこから
    // 設計の画素で横・縦に少し離れた 2 点を写し、画面の上で長く見える方で割る（板を斜めから
    // 見ても、つまみが画面で小さくなりすぎない）。
    const ui::Rect screen = ui::canvasRect(double(viewHeight), viewWidth, viewHeight);
    const double step = desc.height * kScaleProbeFraction;
    std::vector<std::array<double, 2>> pts = {{px, py}, {px + step, py}, {px, py + step}};
    if (!plateToOverlay(desc, cameraIndex, screen, pts)) return false;
    const double alongX = std::hypot(pts[1][0] - pts[0][0], pts[1][1] - pts[0][1]);
    const double alongY = std::hypot(pts[2][0] - pts[0][0], pts[2][1] - pts[0][1]);
    const double longest = std::max(alongX, alongY);
    if (!(longest > 0.0)) return false;
    scale = step / longest;
    return true;
}

bool Scene::newWorldCanvasPlacement(std::size_t cameraIndex, double sizeY, wizengine::Vec3d& pos,
                                    wizengine::Vec3d& euler) const {
    if (cameraIndex >= cameras_.size()) return false;
    const CameraObject& cam = *cameras_[cameraIndex];
    const auto eye = cam.eye();
    const auto target = cam.target();
    // 板の表（+Z）をカメラの方へ向ける（水平の向きだけ = 板は立てたまま）。
    const double toEyeX = eye.x - target.x;
    const double toEyeZ = eye.z - target.z;
    const bool level = toEyeX * toEyeX + toEyeZ * toEyeZ > 0.0;
    const double yaw = level ? std::atan2(toEyeX, toEyeZ) * 180.0 / scenemath::kPi : 0.0;
    pos = {target.x, std::max(target.y, sizeY * 0.5), target.z};
    euler = {0.0, yaw, 0.0};
    return true;
}
