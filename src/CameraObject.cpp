#include "CameraObject.h"

#include <cmath>

#include "Renderer.h"

CameraObject::CameraObject(const Config& config)
    : cfg_(config),
      azim_(config.azimuth),
      elev_(config.elevation),
      radius_(config.radius),
      tx_(config.targetX),
      ty_(config.targetY),
      tz_(config.targetZ) {}

void CameraObject::addElevation(double delta) {
    double elev = elev_.load() + delta;
    if (elev > cfg_.maxElevation) elev = cfg_.maxElevation;
    if (elev < cfg_.minElevation) elev = cfg_.minElevation;
    elev_.store(elev);
}

void CameraObject::stepOrbit(double yawSteps, double pitchSteps) {
    azim_.store(azim_.load() + yawSteps * cfg_.keyStep);
    addElevation(pitchSteps * cfg_.keyStep);
}

void CameraObject::orbit(double dxRadians, double dyRadians) {
    azim_.store(azim_.load() + dxRadians);
    addElevation(dyRadians);
}

void CameraObject::pan(double dxPixels, double dyPixels) {
    // Move the pivot in the camera's screen plane, scaled by distance so the
    // motion under the cursor feels the same at any zoom.
    const double azim = azim_.load();
    const double elev = elev_.load();
    const double k = radius_.load() * cfg_.panMetresPerPx;

    // Camera right/up in world space for the current orbit angles.
    const double rx = std::cos(azim), rz = -std::sin(azim);
    const double ux = -std::sin(elev) * std::sin(azim);
    const double uy = std::cos(elev);
    const double uz = -std::sin(elev) * std::cos(azim);

    tx_.store(tx_.load() + (-dxPixels * rx + dyPixels * ux) * k);
    double ty = ty_.load() + (dyPixels * uy) * k;
    if (ty < 0.0) ty = 0.0;  // keep the pivot above the floor
    ty_.store(ty);
    tz_.store(tz_.load() + (-dxPixels * rz + dyPixels * uz) * k);
}

void CameraObject::zoom(double wheelDelta) {
    // Exponential: every wheel notch scales the distance by the same factor.
    double r = radius_.load() * std::exp(wheelDelta);
    if (r < cfg_.minRadius) r = cfg_.minRadius;
    if (r > cfg_.maxRadius) r = cfg_.maxRadius;
    radius_.store(r);
}

void CameraObject::setPose(double azimuthRad, double elevationRad,
                           double radiusMetres) {
    azim_.store(azimuthRad);
    // 既存の操作（orbit / zoom）と同じ制限を通す。エディタの数値入力や
    // ギズモから来た値でも、真上を跨いで裏返ったりはしない。
    double e = elevationRad;
    if (e > cfg_.maxElevation) e = cfg_.maxElevation;
    if (e < cfg_.minElevation) e = cfg_.minElevation;
    elev_.store(e);
    double r = radiusMetres;
    if (r < cfg_.minRadius) r = cfg_.minRadius;
    if (r > cfg_.maxRadius) r = cfg_.maxRadius;
    radius_.store(r);
}

void CameraObject::setTarget(double x, double y, double z) {
    // pan() と違い y をクランプしない: エディタの「位置をそのまま動かす」は
    // 平行移動なので、成分だけ丸めると向きまで変わってしまう。
    tx_.store(x);
    ty_.store(y);
    tz_.store(z);
}

filament::math::double3 CameraObject::target() const {
    return {tx_.load(), ty_.load(), tz_.load()};
}

filament::math::double3 CameraObject::eye() const {
    const double azim = azim_.load();
    const double elev = elev_.load();
    const double radius = radius_.load();
    const filament::math::double3 t = target();
    return {t.x + radius * std::cos(elev) * std::sin(azim),
            t.y + radius * std::sin(elev),
            t.z + radius * std::cos(elev) * std::cos(azim)};
}

void CameraObject::applyTo(wizengine::Renderer& renderer,
                           std::size_t viewIndex) const {
    renderer.setCamera(viewIndex, eye(), target());
}
