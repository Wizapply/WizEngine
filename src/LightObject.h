#pragma once

#include <atomic>
#include <cstddef>

#include <math/vec3.h>

namespace wizengine {
class Renderer;
}

// A scene light as an object, in the same spirit as CameraObject: the scene
// defines its initial state (scene.cpp, lightConfigs()), the object owns the
// live state, and applyTo() pushes it to the renderer once per frame. Change
// position/direction/colour/intensity from anywhere (a SceneComponent, a
// browser command handler, ...) and the next frame picks it up.
//
// Threading: setters may be called from any thread; applyTo() runs on the
// RENDER thread. Each field is an independent atomic - a torn set of reads
// only ever yields a one-frame-old value, which is harmless for a light.
class LightObject {
public:
    enum class Type {
        Directional,  // parallel light, position is ignored, intensity in lux
        Point,        // shines in every direction, intensity in lumens
        Spot          // cone from position along direction, intensity in lumens
    };

    // Initial state. Defined by the scene (scene.cpp) like every other scene
    // parameter. Type and castShadows are fixed for the light's lifetime
    // (they define the Filament light entity); everything else can change at
    // runtime through the setters below.
    struct Config {
        Type type = Type::Directional;
        filament::math::float3 color{1.0f, 1.0f, 1.0f};  // linear RGB
        // Directional: illuminance in lux (sun ~100k, overcast ~10k).
        // Point/Spot: luminous power in lumens (60W-ish bulb ~800 lm).
        float intensity = 70000.0f;
        filament::math::float3 direction{0.0f, -1.0f, 0.0f};
        filament::math::float3 position{0.0f, 3.0f, 0.0f};  // point/spot only
        bool castShadows = false;
        float falloffRadius = 20.0f;   // point/spot: reach in metres
        float spotInnerRadians = 0.4f;  // spot: full-brightness cone
        float spotOuterRadians = 0.6f;  // spot: cutoff cone
    };

    explicit LightObject(const Config& config);

    // Setup (single-threaded, Scene::build) --------------------------------
    // Creates the light in the renderer. Must be called once before applyTo.
    void attachTo(wizengine::Renderer& renderer);

    // ANY thread -----------------------------------------------------------
    void setPosition(const filament::math::float3& p);
    void setDirection(const filament::math::float3& d);  // normalised inside
    void setColor(const filament::math::float3& linearRgb);
    void setIntensity(float value);

    filament::math::float3 position() const;
    filament::math::float3 direction() const;
    filament::math::float3 color() const;
    float intensity() const;
    const Config& config() const { return cfg_; }

    // RENDER thread --------------------------------------------------------
    // Pushes the current state to the renderer's light. No-op until attached.
    void applyTo(wizengine::Renderer& renderer) const;

private:
    Config cfg_;
    std::size_t rendererIndex_;  // set by attachTo; kNotAttached before
    static constexpr std::size_t kNotAttached = static_cast<std::size_t>(-1);

    std::atomic<float> px_, py_, pz_;
    std::atomic<float> dx_, dy_, dz_;
    std::atomic<float> r_, g_, b_;
    std::atomic<float> intensity_;
};
