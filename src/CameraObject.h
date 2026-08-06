#pragma once

#include <atomic>
#include <cstddef>

#include <math/vec3.h>

namespace wizengine {
class Renderer;
}

// Unity-style orbit camera as a scene object: it owns its full state (orbit
// angles, distance, pivot target) and every way that state changes (mouse
// orbit/pan/zoom, arrow-key steps). The Scene owns one of these the same way
// it owns the boxes; main just routes input events to it and asks it to apply
// itself to the renderer once per frame.
//
// Threading: mutators are called from the INPUT thread, applyTo() from the
// RENDER thread. Each field is an independent atomic - a torn set of reads
// only ever yields a one-frame-old component, which is harmless for a camera.
class CameraObject {
public:
    // Initial pose and feel. Defined by the scene (scene.cpp) like every other
    // scene parameter.
    struct Config {
        double azimuth = 0.66;    // radians around Y
        double elevation = 0.34;  // radians above the horizon
        double radius = 12.0;     // metres from the target
        double targetX = 0.0, targetY = 1.0, targetZ = 0.0;

        double minElevation = -0.15;  // don't go under the ground
        double maxElevation = 1.30;   // don't flip over the pole
        double minRadius = 2.0;
        double maxRadius = 60.0;

        double keyStep = 0.08;         // radians per arrow-key press
        double panMetresPerPx = 0.0016;  // at radius 1 (scaled by distance)
    };

    explicit CameraObject(const Config& config);

    // INPUT thread ---------------------------------------------------------
    void stepOrbit(double yawSteps, double pitchSteps);  // arrow keys
    void orbit(double dxRadians, double dyRadians);      // mouse drag
    void pan(double dxPixels, double dyPixels);          // mouse drag (target)
    void zoom(double wheelDelta);                        // exponential

    // RENDER thread --------------------------------------------------------
    // Computes eye/target from the current state and sets them on the given
    // renderer view (one view per camera).
    void applyTo(wizengine::Renderer& renderer, std::size_t viewIndex = 0) const;

    filament::math::double3 eye() const;
    filament::math::double3 target() const;

private:
    void addElevation(double delta);

    Config cfg_;
    std::atomic<double> azim_;
    std::atomic<double> elev_;
    std::atomic<double> radius_;
    std::atomic<double> tx_, ty_, tz_;
};
