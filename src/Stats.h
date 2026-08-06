#pragma once

#include <atomic>

// Performance counters shared between the physics thread, the render thread and
// the HTTP server (which serves them to the browser overlay). Each field is
// written by exactly one thread and read by the others, so plain atomics are
// enough - no locking on the hot path.
struct PerfStats {
    // Physics thread.
    std::atomic<double> physicsHz{0.0};    // completed updates per second
    std::atomic<double> physicsMs{0.0};    // wall time of one update (all substeps)
    std::atomic<int> substeps{1};          // integration substeps per update
    std::atomic<int> iterations{0};        // solver iterations per substep
    std::atomic<int> bodies{0};            // rigid bodies in the world
    std::atomic<int> asleep{0};            // bodies Chrono has put to sleep
    std::atomic<double> solverMs{0.0};     // solver time inside one update
    std::atomic<double> collisionMs{0.0};  // collision detection inside one update
    std::atomic<double> realtime{1.0};     // simulated seconds per wall second

    // Render thread.
    std::atomic<double> renderFps{0.0};    // frames per second actually produced
    std::atomic<double> renderMs{0.0};     // wall time of renderFrame()
    std::atomic<double> frameMs{0.0};      // wall time of the whole loop body
};
