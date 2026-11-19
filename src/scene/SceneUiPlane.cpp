// UI の板（camera / world の Canvas）を、カメラから見た板と、カーソルのレイが当たる所に
// 直す（Scene のメンバの実装だけ）。板の置き方の式は ui/UiPlane.h、オイラー角 → 向きと
// カメラの基底・レイは scene/SceneMath.h（どちらも唯一の定義）。描画（UiRenderComponent）と
// 当たり（UiPointerInput）が同じ板を使うための 1 か所。
#include "scene/Scene.h"

namespace ui = wizengine::ui;

namespace {

wizengine::Vec3d toVec(const scenemath::Vec3& v) { return {v.x(), v.y(), v.z()}; }

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
                          double ndcY, double& px, double& py, double& distance) const {
    // カメラの基底は 1 回だけ読み（cameraRay）、レイと camera の板の両方をそれから作る
    // （別々に読むと、間でカメラが動いたときに板とレイが別の視点になる）。
    scenemath::Basis basis;
    scenemath::Vec3 dir;
    if (!cameraRay(cameraIndex, ndcX, ndcY, basis, dir)) return false;
    ui::CanvasPlane plane;
    if (!planeFrom(desc, basis, plane)) return false;
    const wizengine::Vec3d c = plane.center;
    const wizengine::Vec3d n = ui::planeNormal(plane);
    const double t = scenemath::rayHitsPlane(basis.eye, dir, scenemath::Vec3(c.x, c.y, c.z),
                                             scenemath::Vec3(n.x, n.y, n.z));
    if (t < 0.0) return false;
    const scenemath::Vec3 hit = basis.eye + dir * t;
    ui::designPointOnPlane(plane, desc.height, toVec(hit), px, py);
    distance = t;
    return true;
}
