#pragma once

#include <atomic>

// Live physics-engine tuning, shared between the browser command side
// (PhysicsControlComponent, INPUT thread) and the physics loop in main.cpp,
// which owns the PhysicsWorld and applies the values at a step boundary.
//
// Two kinds of fields:
//   - direct values (paused, substeps, physicsHz): the loop reads them every
//     pass, so a store takes effect on the next step.
//   - pending values (pendingIterations, pendingEnvelope, pendingRecovery):
//     applying these calls into Chrono, which must happen on the physics
//     thread between steps - the loop exchange()s them out, with 0 meaning
//     "nothing to apply". envelope/recovery keep the last applied value too,
//     so the stats overlay can report it.
struct PhysicsTuning {
    std::atomic<bool> reset{false};       // one-shot: re-drop the boxes
    std::atomic<bool> paused{false};
    // 時間の速さ（実時間 1 秒あたりのシミュレーション秒、0.01〜1 = スロー
    // モーション）。物理ループが経過した実時間にこれを掛けて積む。シーン
    // 文書には入れない（見る側の操作。Pause と同じ扱い）。
    std::atomic<double> timeScale{1.0};
    std::atomic<int> substeps{1};
    std::atomic<int> physicsHz{60};
    std::atomic<int> pendingIterations{0};   // 0 = nothing to apply
    std::atomic<double> pendingEnvelope{0.0};
    std::atomic<double> envelope{0.0};       // last applied (for the overlay)
    std::atomic<double> pendingRecovery{0.0};
    std::atomic<double> recovery{0.0};       // last applied (for the overlay)
};
