#include "LightObject.h"

#include <cmath>

#include "Renderer.h"

using filament::math::float3;

LightObject::LightObject(const Config& config)
    : cfg_(config),
      rendererIndex_(kNotAttached),
      px_(config.position.x),
      py_(config.position.y),
      pz_(config.position.z),
      dx_(config.direction.x),
      dy_(config.direction.y),
      dz_(config.direction.z),
      r_(config.color.x),
      g_(config.color.y),
      b_(config.color.z),
      intensity_(config.intensity) {}

void LightObject::attachTo(wizengine::Renderer& renderer) {
    wizengine::LightDesc d;
    switch (cfg_.type) {
        case Type::Directional:
            d.type = wizengine::LightDesc::Type::Directional;
            break;
        case Type::Point:
            d.type = wizengine::LightDesc::Type::Point;
            break;
        case Type::Spot:
            d.type = wizengine::LightDesc::Type::Spot;
            break;
    }
    d.color = color();
    d.intensity = intensity();
    d.direction = direction();
    d.position = position();
    d.castShadows = cfg_.castShadows;
    d.falloffRadius = cfg_.falloffRadius;
    d.spotInnerRadians = cfg_.spotInnerRadians;
    d.spotOuterRadians = cfg_.spotOuterRadians;
    rendererIndex_ = renderer.addLight(d);
}

void LightObject::setPosition(const float3& p) {
    px_.store(p.x);
    py_.store(p.y);
    pz_.store(p.z);
}

void LightObject::setDirection(const float3& d) {
    // Normalise here so callers can pass any vector; a zero vector keeps the
    // previous direction instead of feeding NaNs to the renderer.
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len <= 0.0f) return;
    dx_.store(d.x / len);
    dy_.store(d.y / len);
    dz_.store(d.z / len);
}

void LightObject::setColor(const float3& c) {
    r_.store(c.x);
    g_.store(c.y);
    b_.store(c.z);
}

void LightObject::setIntensity(float value) { intensity_.store(value); }

float3 LightObject::position() const {
    return {px_.load(), py_.load(), pz_.load()};
}

float3 LightObject::direction() const {
    return {dx_.load(), dy_.load(), dz_.load()};
}

float3 LightObject::color() const { return {r_.load(), g_.load(), b_.load()}; }

float LightObject::intensity() const { return intensity_.load(); }

void LightObject::applyTo(wizengine::Renderer& renderer) const {
    if (rendererIndex_ == kNotAttached) return;
    renderer.updateLight(rendererIndex_, color(), intensity(), direction(),
                         position());
}
