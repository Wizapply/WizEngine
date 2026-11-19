#pragma once

#include <array>
#include <cmath>

#include "core/Vec3d.h"
#include "ui/UiLayout.h"  // Rect
#include "ui/UiTypes.h"

// 板の Canvas（camera / world）の置き方（唯一の定義）。Chrono も Filament も知らない。
//
// 板は幅 sizeX × 高さ sizeY（m）の長方形で、Canvas の中身は「高さ = 基準の高さ（設計の画素）、
// 幅 = 基準の高さ × 板の縦横比」として組む（plateCanvasWidth。ビューの縦横比に依らないので
// 全ビューで同じ中身）。設計の座標は overlay と同じく左上が原点・x 右・y 下で、板の上では
// 「右 = 板の右、下 = 板の上の逆」に写す。板の表（読める側）は right × up の向き。
//   world:  中心 = <canvas pos>、向き = <canvas euler>（オイラー角から right / up を出すのは
//           呼び手 = Scene::canvasPlane。変換の向きは scene/SceneMath.h が唯一の定義）。
//   camera: カメラの正面 distance の所に、カメラと同じ向き（right / up = カメラの基底）で。
// 使い手: 描画（UiRenderComponent が plateUnitToWorld を Renderer::setUiCanvasTransform へ）と、
// 当たり（レイと板の交点を designPointOnPlane で設計の座標に直す）。
namespace wizengine {
namespace ui {

struct CanvasPlane {
    Vec3d center;              // 板の中心（ワールド、m）
    Vec3d right{1.0, 0.0, 0.0};  // 板の右（単位ベクトル）
    Vec3d up{0.0, 1.0, 0.0};     // 板の上（単位ベクトル、right と直交）
    double width = kCanvasDefaultSizeX;   // 板の幅（m）
    double height = kCanvasDefaultSizeY;  // 板の高さ（m）
};

namespace planedetail {
inline Vec3d add(const Vec3d& a, const Vec3d& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3d sub(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3d scale(const Vec3d& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3d cross(const Vec3d& a, const Vec3d& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
}  // namespace planedetail

// 板の Canvas を組む幅（設計の画素）= 基準の高さ × 板の縦横比。
inline double plateCanvasWidth(const CanvasDesc& c) { return c.height * c.sizeX / c.sizeY; }
// 板の Canvas 全体の矩形（設計の画素。overlay の canvasRect に当たる物）。
inline Rect plateCanvasRect(const CanvasDesc& c) { return {0.0, 0.0, plateCanvasWidth(c), c.height}; }

// world の板: 中心は pos、right / up は euler で回した X / Y 軸（呼び手が出す）。
inline CanvasPlane worldCanvasPlane(const CanvasDesc& c, const Vec3d& right, const Vec3d& up) {
    CanvasPlane p;
    p.center = c.pos;
    p.right = right;
    p.up = up;
    p.width = c.sizeX;
    p.height = c.sizeY;
    return p;
}

// camera の板: カメラの視点 eye と基底（forward / right / up は単位ベクトル）から。
inline CanvasPlane cameraCanvasPlane(const CanvasDesc& c, const Vec3d& eye, const Vec3d& forward,
                                     const Vec3d& right, const Vec3d& up) {
    CanvasPlane p;
    p.center = planedetail::add(eye, planedetail::scale(forward, c.distance));
    p.right = right;
    p.up = up;
    p.width = c.sizeX;
    p.height = c.sizeY;
    return p;
}

// 板の表の向き（right × up の単位ベクトル。当たりの平面の法線）。
inline Vec3d planeNormal(const CanvasPlane& p) { return planedetail::cross(p.right, p.up); }

// 板の左上の角（ワールド）。
inline Vec3d planeTopLeft(const CanvasPlane& p) {
    using namespace planedetail;
    return add(sub(p.center, scale(p.right, p.width * 0.5)), scale(p.up, p.height * 0.5));
}

// 「Canvas の高さ = 1 の単位（左上が原点・x 右・y 下）」→ ワールドの 4×4 行列（列優先の
// 16 個 = filament::math::mat4f の並び）。単位の 1 は板の高さ（m）、z は板の表の向き。
inline std::array<float, 16> plateUnitToWorld(const CanvasPlane& p) {
    using namespace planedetail;
    const Vec3d x = scale(p.right, p.height);
    const Vec3d y = scale(p.up, -p.height);
    const Vec3d z = planeNormal(p);
    const Vec3d o = planeTopLeft(p);
    return {float(x.x), float(x.y), float(x.z), 0.0f,  //
            float(y.x), float(y.y), float(y.z), 0.0f,  //
            float(z.x), float(z.y), float(z.z), 0.0f,  //
            float(o.x), float(o.y), float(o.z), 1.0f};
}

// 板の面の上のワールドの点 → Canvas の設計の座標（画素。左上が原点・y 下）。板の外の点は
// 範囲外の座標になる（当たりは呼び手の矩形の判定に任せる）。
inline void designPointOnPlane(const CanvasPlane& p, double designHeight, const Vec3d& world,
                               double& x, double& y) {
    using namespace planedetail;
    const Vec3d rel = sub(world, planeTopLeft(p));
    const double perMetre = p.height > 0.0 ? designHeight / p.height : 0.0;
    x = dot(rel, p.right) * perMetre;
    y = -dot(rel, p.up) * perMetre;
}

}  // namespace ui
}  // namespace wizengine
