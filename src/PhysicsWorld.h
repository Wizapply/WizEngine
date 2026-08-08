#pragma once

#include <chrono/physics/ChContactMaterialNSC.h>
#include <chrono/physics/ChSystem.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace chrono {
class ChTriangleMeshConnected;
}

// Where the time went inside the last physics step (seconds), straight from
// Chrono's own timers. Useful for deciding what to optimise: a solver-dominated
// step wants fewer iterations, a collision-dominated one wants a smaller
// contact envelope or a different collision system.
struct StepTimers {
    double step = 0.0;
    double solver = 0.0;
    double collision = 0.0;
    double setup = 0.0;
    double update = 0.0;
};

// Which Chrono system to build on. Multicore needs a Chrono built with the
// MULTICORE module (CMake: -DWIZ_USE_MULTICORE=ON); if that is not available
// the world silently falls back to Core and says so on stdout.
enum class PhysicsBackend {
    Core,       // ChSystemNSC - serial solver, supports sleeping
    Multicore,  // ChSystemMulticoreNSC - OpenMP solver + collision, no sleeping
};

// True when this build can actually use PhysicsBackend::Multicore.
bool multicoreAvailable();

// Rigid-body transform snapshot passed to the render side.
// Position in metres, rotation as a unit quaternion (w, x, y, z).
struct BodyTransform {
    double px, py, pz;
    double qw, qx, qy, qz;
};

// Physics engine: owns the Chrono system (gravity, collision, contact material).
// It holds no scene of its own - bodies are added by the Scene via addBox().
class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsBackend backend = PhysicsBackend::Core);

    // "core" or "multicore" - what actually got created.
    const char* backendName() const;

    void step(double dt);

    // Solver iterations: the main quality/cost dial. Fewer = faster but more
    // jitter in resting stacks.
    // Push a body: applies force * dt as a change in linear velocity, and
    // wakes the body so a sleeping box responds. Force is in newtons, dt in
    // seconds; using an impulse rather than Chrono's force accumulators keeps
    // this independent of which accumulator API the installed version has.
    void applyForce(std::size_t id, const chrono::ChVector3d& force, double dt);
    double bodyMass(std::size_t id) const;
    // Linear velocity, for servo-style forces (mouse joint).
    chrono::ChVector3d bodyVelocity(std::size_t id) const;

    // Number of worker threads the solver and collision detection may use.
    // Must be called before stepping starts; Chrono sizes its thread pool from
    // this. 0 or less leaves the automatic choice in place.
    void setNumThreads(int threads);

    void setSolverIterations(int iterations);

    // Contact envelope/margin. The envelope makes Chrono create contacts before
    // surfaces actually touch; the default (0.03 m) is large next to 0.5 m
    // boxes and multiplies the contact count. Must be set before bodies are
    // created.
    void setCollisionTolerances(double envelope, double margin);

    // How aggressively existing overlap is pushed out (m/s), and how tightly
    // the solver converges. Raising the recovery speed removes visible
    // interpenetration faster but can make resting stacks pop; tightening the
    // tolerance costs iterations.
    void setContactSettings(double recoverySpeed, double tolerance);
    // Recovery speed alone (used for live tuning from the browser).
    void setContactRecoverySpeed(double recoverySpeed);

    // Timers for the most recent step().
    StepTimers timers() const;

    // Chrono's own per-body sleeping: a body whose linear/angular speed stays
    // below the limits for `seconds` is dropped from the solver until a contact
    // island wakes it. Cheap for piles where only part of the scene moves.
    // Caveat: a sleeping body is only woken through contact, so if its support
    // moves away without touching it, it stays put in mid-air. Keep the limits
    // tight (well under Chrono's 0.1 m/s default) to make that rare.
    void setSleepingEnabled(bool enabled, float seconds = 1.0f,
                            float minLinVel = 0.02f, float minAngVel = 0.02f);
    void wakeAll();
    std::size_t sleepingCount() const;

    // Add a box body. density in kg/m^3; fixed=true makes it static. Returns id.
    // Sphere body - rolls, unlike a box. Radius in metres.
    std::size_t addSphere(double radius, double density,
                          const chrono::ChVector3d& pos,
                          const chrono::ChQuaternion<>& rot, bool fixed);

    // Rolling/spinning friction on the shared contact material. Without any,
    // spheres roll forever on a flat floor.
    void setRollingFriction(float rolling, float spinning);

    // Sliding friction and bounciness of the shared contact material.
    // Friction is what converts sliding into rolling: a low value makes
    // spheres skid, a high one makes them spin up as soon as they touch down.
    void setSurfaceMaterial(float friction, float restitution);

    // Velocity damping, applied every step as v *= exp(-k dt): a stand-in for
    // air resistance that also stops spheres from rolling forever. 0 disables
    // it. Applied here rather than through ChBody's own damping so it behaves
    // the same on both backends and across Chrono versions.
    void setDamping(double linearPerSecond, double angularPerSecond);

    std::size_t addBox(double sx, double sy, double sz, double density,
                       const chrono::ChVector3d& pos,
                       const chrono::ChQuaternion<>& rot, bool fixed);

    // Dynamic body colliding as the convex hull of `points` (metres, already
    // scaled; see MeshCollision::loadCollisionPoints). Mass = density x hull
    // volume. NOTE: Chrono re-centres the hull on its barycentre, so a model
    // whose origin sits far from its centre will render offset from where it
    // collides - author models with the origin near the middle.
    std::size_t addConvexHull(const std::vector<chrono::ChVector3d>& points,
                              double density, const chrono::ChVector3d& pos,
                              const chrono::ChQuaternion<>& rot);

    // Fixed body colliding as an exact triangle mesh - for terrain and
    // obstacles only; dynamic mesh-vs-mesh contact is slow and fragile.
    std::size_t addStaticMesh(
        std::shared_ptr<chrono::ChTriangleMeshConnected> mesh,
        const chrono::ChVector3d& pos, const chrono::ChQuaternion<>& rot);

    std::size_t bodyCount() const;
    BodyTransform bodyTransform(std::size_t id) const;

    // Move a body and clear its velocity/acceleration (for re-dropping).
    void setBodyPose(std::size_t id, const chrono::ChVector3d& pos,
                     const chrono::ChQuaternion<>& rot);

private:
    // Either a ChSystemNSC (serial core) or a ChSystemMulticoreNSC, chosen at
    // build time by WIZ_USE_MULTICORE. Everything above this class is unaware
    // of which one is in use.
    std::shared_ptr<chrono::ChSystem> sys_;
    std::shared_ptr<chrono::ChContactMaterialNSC> mat_;
    std::vector<std::shared_ptr<chrono::ChBody>> bodies_;
    PhysicsBackend backend_ = PhysicsBackend::Core;
    double linearDamping_ = 0.0;
    double angularDamping_ = 0.0;
    bool sleepingEnabled_ = false;
    float sleepSeconds_ = 1.0f;
    float sleepMinLinVel_ = 0.02f;
    float sleepMinAngVel_ = 0.02f;
};
