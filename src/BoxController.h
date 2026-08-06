#pragma once

#include <atomic>
#include <cstddef>

// Holds one camera's grab state: which object is grabbed, and where that
// camera's cursor currently is on screen (normalised device coords, x/y in
// [-1,1], y up).
//
// The cursor is stored as an ABSOLUTE position rather than accumulated drag
// deltas: deltas need a pixels-to-metres conversion that depends on the
// object's distance, so any error in it accumulates and the object drifts away
// from the cursor. With the absolute position the physics side can put the
// target exactly on the cursor's ray every step, and it can never drift.
//
// Threading: setSelected()/setPointer() come from the INPUT thread,
// selected()/pointer() from the PHYSICS thread; all state is atomic.
class BoxController {
public:
    struct Config {
        // Mouse-joint servo: the grabbed object is pulled towards the cursor
        // with F = m * (kp*(target-pos) - kd*v). Stiffness kp is the pull
        // (1/s^2), damping kd stops overshoot (1/s); critically damped when
        // kd ~ 2*sqrt(kp).
        double stiffness = 80.0;
        double damping = 12.0;
        // Cap on the correction acceleration (m/s^2), so a fast flick cannot
        // launch the object across the scene.
        double maxAcceleration = 60.0;
    };

    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    explicit BoxController(const Config& config) : cfg_(config) {}

    // INPUT thread ---------------------------------------------------------
    void setSelected(std::size_t index) { selected_.store(index); }
    void setPointer(double ndcX, double ndcY) {
        pointerX_.store(ndcX);
        pointerY_.store(ndcY);
        hasPointer_.store(true);
    }
    void clearPointer() { hasPointer_.store(false); }

    // PHYSICS thread -------------------------------------------------------
    std::size_t selected() const { return selected_.load(); }
    bool pointer(double& ndcX, double& ndcY) const {
        ndcX = pointerX_.load();
        ndcY = pointerY_.load();
        return hasPointer_.load();
    }
    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    std::atomic<std::size_t> selected_{kNone};
    std::atomic<double> pointerX_{0.0};
    std::atomic<double> pointerY_{0.0};
    std::atomic<bool> hasPointer_{false};
};
