#include "Scene.h"
#include "SceneConfig.h"
#include "MeshCollision.h"
#include "Log.h"

#include "scene_math.h"

#include <chrono/core/ChQuaternion.h>
#include <chrono/core/ChVector3.h>

#include <math/mat4.h>
#include <math/quat.h>
#include <math/vec3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <random>

#include "AssetError.h"
#include "PhysicsWorld.h"
#include "Renderer.h"
#include "math_bridge.h"

using namespace chrono;

namespace {

// Scene parameters (grid, cameras, lights, physics, ...) live in
// SceneConfig.h; below are only derived helpers and the implementation.

std::size_t boxTotal() { return static_cast<std::size_t>(kNx * kNy * kNz); }

ChVector3d gridPos(std::size_t k) {
    const int nyz = kNy * kNz;
    const int i = static_cast<int>(k) / nyz;
    const int rem = static_cast<int>(k) % nyz;
    const int j = rem / kNz;
    const int l = rem % kNz;
    return ChVector3d((i - (kNx - 1) / 2.0) * kSpacing, kBaseY + j * kSpacing,
                      (l - (kNz - 1) / 2.0) * kSpacing);
}

ChQuaternion<> dropTilt(std::size_t k) {
    const double a = 0.08 + 0.02 * static_cast<double>(k % 5);
    return QuatFromAngleAxis(a, ChVector3d(1, 0.3, 1).GetNormalized());
}

ChVector3d jitter() {
    static std::mt19937 rng(1234);
    std::uniform_real_distribution<double> d(-0.02, 0.02);
    return ChVector3d(d(rng), d(rng), d(rng));
}

}  // namespace

namespace {

// Routes camera commands from a browser page to that page's CameraObject.
class CameraControlComponent : public SceneComponent {
public:
    bool onCommand(Scene& scene, std::size_t cam,
                   const nlohmann::json& msg) override {
        const std::string cmd = msg.value("cmd", "");
        if (cmd == "camera") {  // arrow keys: fixed-size orbit steps
            scene.camera(cam).stepOrbit(msg.value("yaw", 0.0),
                                        msg.value("pitch", 0.0));
        } else if (cmd == "orbit") {
            scene.camera(cam).orbit(msg.value("dx", 0.0), msg.value("dy", 0.0));
        } else if (cmd == "pan") {
            scene.camera(cam).pan(msg.value("dx", 0.0), msg.value("dy", 0.0));
        } else if (cmd == "zoom") {
            scene.camera(cam).zoom(msg.value("d", 0.0));
        } else if (cmd == "pick") {
            const std::size_t hit =
                scene.pickBoxAt(msg.value("x", 0.0), msg.value("y", 0.0), cam);
            LOGD("scene", "pick (camera %zu): %s", cam,
                 hit == BoxController::kNone
                     ? "(none)"
                     : ("object " + std::to_string(hit)).c_str());
        } else {
            return false;
        }
        return true;
    }
};

// Applies each camera's mouse drag to its grabbed object and keeps the
// per-camera coloured highlight in sync.
class BoxControlComponent : public SceneComponent {
public:
    void onPhysicsStep(Scene& scene, double dt) override {
        if (depths_.size() != scene.cameraCount()) {
            depths_.assign(scene.cameraCount(), Depth{});
            lines_.clear();
            for (std::size_t k = 0; k < scene.cameraCount(); ++k) {
                lines_.push_back(std::make_unique<LineState>());
            }
        }

        for (std::size_t c = 0; c < scene.cameraCount(); ++c) {
            BoxController& ctl = scene.boxController(c);
            const std::size_t sel = ctl.selected();
            Depth& depth = depths_[c];
            if (sel >= scene.objectCount()) {
                depth.valid = false;
                lines_[c]->on.store(false);
                continue;
            }

            double ndcX = 0.0, ndcY = 0.0;
            if (!ctl.pointer(ndcX, ndcY)) {
                lines_[c]->on.store(false);
                continue;
            }

            const std::size_t physId = scene.object(sel).physId;
            const BodyTransform tr = scene.physics().bodyTransform(physId);

            // Camera basis and the point under the cursor, both from the
            // shared Eigen helpers so picking and dragging cannot drift apart.
            const auto basis = scenemath::cameraBasis(scene.camera(c));
            if (!basis.valid) continue;

            // Depth of the object when it was grabbed: the target rides on the
            // plane at that depth, so the object stays under the cursor
            // without being pulled towards or away from the camera.
            const scenemath::Vec3 objPos(tr.px, tr.py, tr.pz);
            if (!depth.valid || depth.sel != sel) {
                depth.valid = true;
                depth.sel = sel;
                depth.z = std::max(0.1, (objPos - basis.eye).dot(basis.forward));
            }

            // Ray through the cursor, hit against that plane. Absolute, so the
            // target is exactly under the cursor every step - no drift.
            const scenemath::Vec3 dir = scenemath::rayThrough(
                basis, ndcX, ndcY, scene.renderer().verticalFovDegrees(),
                scene.renderer().aspect());
            const double along = dir.dot(basis.forward);
            if (along <= 1e-6) continue;
            const scenemath::Vec3 target = basis.eye + dir * (depth.z / along);

            // Servo towards the target: F = m * (kp*e - kd*v), acceleration
            // capped. Applied every step, which is what makes it track - a
            // one-shot impulse dies to friction immediately.
            const auto& cfg = ctl.config();
            const chrono::ChVector3d cv = scene.physics().bodyVelocity(physId);
            const scenemath::Vec3 vel(cv.x(), cv.y(), cv.z());
            scenemath::Vec3 accel =
                cfg.stiffness * (target - objPos) - cfg.damping * vel;
            const double a = accel.norm();
            if (a > cfg.maxAcceleration) accel *= cfg.maxAcceleration / a;

            const double mass = scene.physics().bodyMass(physId);
            if (mass <= 0.0) continue;
            const scenemath::Vec3 force = accel * mass;
            scene.physics().applyForce(
                physId, chrono::ChVector3d(force.x(), force.y(), force.z()), dt);

            // Line endpoints for the render thread: object -> cursor point.
            LineState& ln = *lines_[c];
            ln.ax.store(tr.px);
            ln.ay.store(tr.py);
            ln.az.store(tr.pz);
            ln.bx.store(target.x());
            ln.by.store(target.y());
            ln.bz.store(target.z());
            ln.on.store(true);
        }
    }

    bool onCommand(Scene& scene, std::size_t cam,
                   const nlohmann::json& msg) override {
        const std::string cmd = msg.value("cmd", "");
        if (cmd == "drag") {
            const double x = msg.value("x", 0.0);
            const double y = msg.value("y", 0.0);
            BoxController& ctl = scene.boxController(cam);
            if (ctl.selected() != BoxController::kNone) {
                ctl.setPointer(x, y);  // holding something: pull it
            } else if (msg.value("touch", false) &&
                       lastPointer_.size() > cam && lastPointer_[cam].valid) {
                // Touch only: a finger that grabbed nothing orbits instead.
                // The client cannot know whether the press hit an object
                // without waiting for a round trip, which would swallow the
                // start of the gesture, so the decision is made here.
                // With a mouse there is no such problem - Ctrl+drag is the
                // camera and a plain drag that hits nothing does nothing.
                scene.camera(cam).orbit(
                    -(x - lastPointer_[cam].x) * kOrbitRadPerNdc,
                    -(y - lastPointer_[cam].y) * kOrbitRadPerNdc);
            }
            if (lastPointer_.size() <= cam) lastPointer_.resize(cam + 1);
            lastPointer_[cam] = {true, x, y};
            return true;
        }
        if (cmd == "release") {
            scene.boxController(cam).setSelected(BoxController::kNone);
            scene.boxController(cam).clearPointer();
            if (lastPointer_.size() > cam) lastPointer_[cam].valid = false;
            return true;
        }
        if (cmd == "select") {  // clicked in the sidebar hierarchy
            const int idx = msg.value("index", -1);
            scene.boxController(cam).setSelected(
                (idx >= 0 && std::size_t(idx) < scene.objectCount())
                    ? std::size_t(idx)
                    : BoxController::kNone);
            return true;
        }
        return false;
    }

    void onRender(Scene& scene) override {
        // Grab lines are real geometry in the scene, so they follow the object
        // in 3D and are visible from every camera.
        for (std::size_t c = 0; c < lines_.size(); ++c) {
            const LineState& ln = *lines_[c];
            const bool on = ln.on.load();
            scene.renderer().setGrabLine(
                c,
                {float(ln.ax.load()), float(ln.ay.load()), float(ln.az.load())},
                {float(ln.bx.load()), float(ln.by.load()), float(ln.bz.load())},
                on);
        }

        // Selections are per camera, so several highlights can be lit at once;
        // the renderer is only touched when a selection changes.
        if (last_.size() != scene.cameraCount()) {
            last_.assign(scene.cameraCount(), BoxController::kNone);
        }
        for (std::size_t c = 0; c < scene.cameraCount(); ++c) {
            const std::size_t sel = scene.boxController(c).selected();
            if (sel == last_[c]) continue;
            setMark(scene, last_[c], c, false);
            setMark(scene, sel, c, true);
            last_[c] = sel;
        }
    }

private:
    static void setMark(Scene& scene, std::size_t index, std::size_t cam,
                        bool on) {
        if (index >= scene.objectCount()) return;
        const std::size_t renderId = scene.object(index).renderId;
        if (scene.drawnAsModel()) {
            scene.renderer().setModelInstanceTint(
                renderId, scene.cameraColor(cam),
                on ? scene.selectedWhiten() : 0.0f);
        } else {
            scene.renderer().setBoxHighlighted(renderId, on ? int(cam) : -1);
        }
    }

    std::vector<std::size_t> last_;  // render thread only

    // Previous pointer position per camera (input thread), so a drag that
    // grabbed nothing can be turned into an orbit.
    struct LastPointer {
        bool valid = false;
        double x = 0.0, y = 0.0;
    };
    std::vector<LastPointer> lastPointer_;
    // Radians of orbit per unit of normalised device coords. The screen spans
    // 2 units, so this is roughly "half a screen drag = this many radians".
    static constexpr double kOrbitRadPerNdc = 1.6;

    // Distance from the camera to the object at grab time (physics thread).
    struct Depth {
        bool valid = false;
        std::size_t sel = BoxController::kNone;
        double z = 0.0;
    };
    std::vector<Depth> depths_;

    // Line endpoints handed from the physics thread to the render thread: the
    // object and the point it is being pulled towards.
    struct LineState {
        std::atomic<bool> on{false};
        std::atomic<double> ax{0.0}, ay{0.0}, az{0.0};  // object
        std::atomic<double> bx{0.0}, by{0.0}, bz{0.0};  // target
    };
    std::vector<std::unique_ptr<LineState>> lines_;
};

// --- Example ObjectAction ---------------------------------------------------
// Shoves its object upward for a moment every few seconds. Attach in
// Scene::build() to see per-object behaviour working:
//     boxes_[0].actions.push_back(std::make_unique<PulseUpAction>());
class PulseUpAction : public ObjectAction {
public:
    void onPhysicsStep(GameObject& object, PhysicsWorld& physics,
                       double dt) override {
        clock_ += dt;
        if (clock_ >= kPeriod) clock_ -= kPeriod;
        if (clock_ < kBurst) {
            const double f = physics.bodyMass(object.physId) * 30.0;  // ~3 g
            physics.applyForce(object.physId, chrono::ChVector3d(0, f, 0), dt);
        }
    }

private:
    static constexpr double kPeriod = 3.0;  // seconds between pulses
    static constexpr double kBurst = 0.15;  // how long each shove lasts
    double clock_ = 0.0;
};

}  // namespace

float Scene::selectedWhiten() const {
    return kSelectedWhiten;
}

std::string Scene::hierarchyJson(std::size_t cameraIndex) {
    auto hex = [](const filament::math::float3& c) {
        auto ch = [](float v) {
            const int i = int(v * 255.0f + 0.5f);
            return i < 0 ? 0 : (i > 255 ? 255 : i);
        };
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", ch(c.x), ch(c.y),
                      ch(c.z));
        return std::string(buf);
    };

    nlohmann::json j;
    j["camera"] = int(cameraIndex);
    j["shape"] = (kBodyShape == BodyShape::Sphere) ? "sphere" : "box";
    j["model"] = useModel_ ? kBoxModelPath : "";

    // Cameras, with what each one currently holds.
    for (std::size_t c = 0; c < cameras_.size(); ++c) {
        nlohmann::json e;
        e["index"] = int(c);
        e["color"] = hex(cameraColor(c));
        const std::size_t sel = controllers_[c]->selected();
        e["selected"] = (sel == BoxController::kNone) ? -1 : int(sel);
        j["cameras"].push_back(e);
    }

    // Lights, so the hierarchy shows the scene's lighting alongside the
    // objects. LightObject's getters are atomics, so reading them from this
    // (HTTP) thread is safe.
    for (std::size_t i = 0; i < lights_.size(); ++i) {
        const LightObject& l = *lights_[i];
        nlohmann::json e;
        e["index"] = int(i);
        const char* type = "directional";
        switch (l.config().type) {
            case LightObject::Type::Directional: type = "directional"; break;
            case LightObject::Type::Point: type = "point"; break;
            case LightObject::Type::Spot: type = "spot"; break;
        }
        e["type"] = type;
        e["color"] = hex(l.color());
        e["intensity"] = l.intensity();
        const auto d = l.direction();
        e["dx"] = d.x;
        e["dy"] = d.y;
        e["dz"] = d.z;
        const auto p = l.position();
        e["x"] = p.x;
        e["y"] = p.y;
        e["z"] = p.z;
        e["shadows"] = l.config().castShadows;
        j["lights"].push_back(e);
    }

    // Objects. Positions come from the shared pose snapshot, so this never
    // touches the physics world from the HTTP thread.
    std::vector<BodyTransform> poses;
    {
        std::lock_guard<std::mutex> lk(poseMutex_);
        poses = latestPoses_;
    }
    // Who (if anyone) is holding each object, so the list can colour it.
    std::vector<int> heldBy(boxes_.size(), -1);
    for (std::size_t c = 0; c < controllers_.size(); ++c) {
        const std::size_t sel = controllers_[c]->selected();
        if (sel < heldBy.size()) heldBy[sel] = int(c);
    }
    for (std::size_t i = 0; i < boxes_.size() && i < poses.size(); ++i) {
        nlohmann::json e;
        e["index"] = int(i);
        e["x"] = poses[i].px;
        e["y"] = poses[i].py;
        e["z"] = poses[i].pz;
        e["heldBy"] = heldBy[i];
        j["objects"].push_back(e);
    }
    return j.dump();
}

filament::math::float3 Scene::cameraColor(std::size_t cameraIndex) const {
    const auto& colors = cameraColors();
    return colors[cameraIndex % colors.size()];
}

void Scene::addComponent(std::unique_ptr<SceneComponent> component) {
    components_.push_back(std::move(component));
}

bool Scene::dispatchCommand(std::size_t camIndex, const nlohmann::json& msg) {
    for (auto& c : components_) {
        if (c->onCommand(*this, camIndex, msg)) return true;
    }
    return false;
}

Scene::Scene(PhysicsWorld& physics, wizengine::Renderer& renderer)
    : physics_(physics), renderer_(renderer) {
    for (const auto& cfg : cameraConfigs()) {
        cameras_.push_back(std::make_unique<CameraObject>(cfg));
        // One controller per camera; they act on the same bodies but hold
        // separate selections.
        controllers_.push_back(
            std::make_unique<BoxController>(boxControllerConfig()));
    }
    for (const auto& cfg : lightConfigs()) {
        lights_.push_back(std::make_unique<LightObject>(cfg));
    }
    addComponent(std::make_unique<CameraControlComponent>());
    addComponent(std::make_unique<BoxControlComponent>());
}

std::size_t Scene::pickBoxAt(double ndcX, double ndcY,
                             std::size_t cameraIndex) {
    if (cameraIndex >= cameras_.size()) return BoxController::kNone;

    // Ray from the camera through the clicked point, intersected with every
    // object. Done here rather than through the collision engine so it behaves
    // the same on the Core and Multicore backends.
    const auto basis = scenemath::cameraBasis(*cameras_[cameraIndex]);
    if (!basis.valid) return BoxController::kNone;
    const scenemath::Vec3 dir = scenemath::rayThrough(
        basis, ndcX, ndcY, renderer_.verticalFovDegrees(), renderer_.aspect());

    std::size_t best = BoxController::kNone;
    double bestT = 0.0;
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        const BodyTransform t = physics_.bodyTransform(boxes_[i].physId);
        const double hit =
            (kBodyShape == BodyShape::Sphere)
                ? scenemath::rayHitsSphere(basis.eye, dir, t, kBoxSize * 0.5)
                : scenemath::rayHitsBox(basis.eye, dir, t, kBoxSize * 0.5);
        if (hit < 0.0) continue;
        if (best == BoxController::kNone || hit < bestT) {
            best = i;
            bestT = hit;
        }
    }

    controllers_[cameraIndex]->setSelected(best);
    return best;
}

PhysicsBackend scenePhysicsBackend() {
    return kBackend;
}

int Scene::substeps() const {
    return kSubsteps;
}

int Scene::solverIterations() const {
    return kSolverIterations;
}

int Scene::physicsHz() const {
    return kPhysicsHz;
}

double Scene::collisionEnvelope() const {
    return kCollisionEnvelope;
}

double Scene::contactRecovery() const {
    return kContactRecovery;
}

bool Scene::idleWhenUnwatched() const {
    return kIdleWhenUnwatched;
}

void Scene::build() {
    // Must precede body creation.
    physics_.setCollisionTolerances(kCollisionEnvelope, kCollisionMargin);
    physics_.setSolverIterations(kSolverIterations);
    physics_.setContactSettings(kContactRecovery, kSolverTolerance);
    physics_.setSurfaceMaterial(kFriction, kRestitution);
    physics_.setRollingFriction(kRollingFriction, kSpinningFriction);
    physics_.setDamping(kLinearDamping, kAngularDamping);
    physics_.setSleepingEnabled(kSleepingEnabled, kSleepSeconds, kSleepMinLinVel,
                                kSleepMinAngVel);
    renderer_.setBoxColor({kBoxR, kBoxG, kBoxB});

    // Ground: a static collision box in physics + a lit plane in the renderer.
    physics_.addBox(kGroundSize, 1.0, kGroundSize, kDensity,
                    ChVector3d(0, -0.5, 0), QUNIT, /*fixed*/ true);
    renderer_.addGround(kGroundHalf, {kGroundTintR, kGroundTintG, kGroundTintB},
                        kGroundTile, kGroundTexture);

    if (kModelPath[0] != '\0') {
        const std::size_t model = renderer_.addModel(kModelPath);
        {
            // Yaw about +Y, built by hand: quaternion factory names differ
            // between filament math versions, and an explicit quatf(w,x,y,z)
            // works everywhere.
            const float yaw = float(scenemath::radians(kModelYawDegrees));
            const filament::math::float3 position{kModelX, kModelY, kModelZ};
            const filament::math::quatf rotation(std::cos(yaw * 0.5f), 0.0f,
                                                 std::sin(yaw * 0.5f), 0.0f);
            renderer_.setModelTransform(model, position, rotation, kModelScale);
        }
        if (kModelCollision) {
            // Static collision mesh at the model's pose. A low-poly proxy
            // (kModelCollisionPath) is preferred when configured; otherwise
            // the drawn model itself is used.
            const char* collisionSrc = kModelCollisionPath[0] != '\0'
                                           ? kModelCollisionPath
                                           : kModelPath;
            auto mesh =
                chrono_types::make_shared<chrono::ChTriangleMeshConnected>();
            if (wizengine::loadCollisionMesh(collisionSrc, kModelScale,
                                             *mesh)) {
                const double yaw = scenemath::radians(kModelYawDegrees);
                physics_.addStaticMesh(
                    mesh, ChVector3d(kModelX, kModelY, kModelZ),
                    ChQuaternion<>(std::cos(yaw * 0.5), 0.0,
                                   std::sin(yaw * 0.5), 0.0));
                LOGI("scene", "model '%s': static mesh collision enabled",
                     collisionSrc);
            } else {
                LOGW("scene",
                     "model '%s': collision mesh unavailable - decoration only",
                     collisionSrc);
            }
        }
    }

    // Optionally draw the dynamic objects as a model instead of the built-in
    // cube. One asset, N instances - the mesh is shared.
    // Loading throws if the model is unusable, so reaching the next line means
    // the objects really are drawn as that model.
    useModel_ = kBoxModelPath[0] != '\0';
    if (useModel_) renderer_.createModelInstances(kBoxModelPath, boxTotal());
    // No list of files to keep in sync here: every loader throws AssetError
    // when it cannot read what it was given, so a file added to the scene
    // config later is validated by the same path automatically.

    // Create the scene's lights in the renderer. Single-threaded here, like
    // the rest of build(); after this the LightObjects are live and their
    // state is pushed every frame from applyToRenderer().
    for (auto& l : lights_) l->attachTo(renderer_);

    if (kEnvironmentHdr[0] != '\0') {
        renderer_.loadEnvironment(kEnvironmentHdr, kEnvironmentIntensity);
    }

    renderer_.configureHighlightColors(cameraColors());
    // One line per camera, in that camera's colour.
    {
        std::vector<filament::math::float3> lineColors;
        for (std::size_t c = 0; c < cameras_.size(); ++c) {
            lineColors.push_back(cameraColor(c));
        }
        renderer_.configureGrabLines(lineColors);
    }

    if (useModel_) {
        modelScale_ = kBoxModelScale;
        const float modelSize = renderer_.modelInstanceSize();
        LOGI("scene",
             "bodies drawn as: glTF model instances ('%s', model size %.4f -> "
             "%.4f m at scale %.3f; shape is %.3f m across)",
             kBoxModelPath, modelSize, modelSize * modelScale_, modelScale_,
             kBoxSize);
    } else {
        LOGI("scene", "bodies drawn as: built-in cube (kBoxModelPath=\"%s\")",
             kBoxModelPath);
    }

    // ConvexHull bodies collide as the drawn model's hull: load the point
    // cloud once here, shared by every body. Empty = unavailable, and the
    // loop below falls back to boxes so the scene still runs.
    std::vector<ChVector3d> hullPoints;
    if (kBodyShape == BodyShape::ConvexHull) {
        if (useModel_) {
            hullPoints =
                wizengine::loadCollisionPoints(kBoxModelPath, kBoxModelScale);
        }
        if (hullPoints.empty()) {
            LOGW("scene",
                 "convex hull unavailable (kBoxModelPath=\"%s\") - bodies "
                 "fall back to boxes",
                 kBoxModelPath);
        }
    }

    // Grid of dynamic boxes: one physics body + one renderable each.
    for (std::size_t k = 0; k < boxTotal(); ++k) {
        std::size_t physId;
        if (kBodyShape == BodyShape::Sphere) {
            physId = physics_.addSphere(kBoxSize * 0.5, kDensity,
                                        gridPos(k) + jitter(), dropTilt(k),
                                        /*fixed*/ false);
        } else if (kBodyShape == BodyShape::ConvexHull &&
                   !hullPoints.empty()) {
            physId = physics_.addConvexHull(hullPoints, kDensity,
                                            gridPos(k) + jitter(),
                                            dropTilt(k));
        } else {
            physId = physics_.addBox(kBoxSize, kBoxSize, kBoxSize, kDensity,
                                     gridPos(k) + jitter(), dropTilt(k),
                                     /*fixed*/ false);
        }
        // With a model, the renderable id is just the instance index.
        const std::size_t renderId = useModel_ ? k : renderer_.addBox();
        boxes_.push_back({physId, renderId});
    }
    snapshot();  // initial poses so the first frame shows the boxes in place
}

void Scene::stepPhysics(double dt) {
    for (auto& c : components_) c->onPhysicsStep(*this, dt);
    for (auto& obj : boxes_) {
        for (auto& a : obj.actions) a->onPhysicsStep(obj, physics_, dt);
    }
    physics_.step(dt);
    snapshot();
}

// Re-drop every box. Only ever called from the browser (Reset / R).
void Scene::reset() {
    placeBoxes();
    physics_.wakeAll();  // last: it also rebuilds the solver layout
    snapshot();
}

void Scene::placeBoxes() {
    for (std::size_t k = 0; k < boxes_.size(); ++k) {
        physics_.setBodyPose(boxes_[k].physId, gridPos(k) + jitter(), dropTilt(k));
    }
}

void Scene::snapshot() {
    std::vector<BodyTransform> poses;
    poses.reserve(boxes_.size());
    for (const auto& obj : boxes_) poses.push_back(physics_.bodyTransform(obj.physId));
    std::lock_guard<std::mutex> lk(poseMutex_);
    latestPoses_.swap(poses);
}

void Scene::applyToRenderer() {
    for (auto& c : components_) c->onRender(*this);
    for (auto& l : lights_) l->applyTo(renderer_);

    std::vector<BodyTransform> poses;
    {
        std::lock_guard<std::mutex> lk(poseMutex_);
        poses = latestPoses_;
    }
    if (poses.size() != boxes_.size()) return;

    // The cube mesh is unit-sized; scale it to the box size when placing. A
    // model gets the same treatment plus its own tuning factor.
    const float s = useModel_ ? modelScale_ : float(kBoxSize);
    const auto scale = filament::math::mat4f::scaling(filament::math::float3{s});
    for (std::size_t k = 0; k < boxes_.size(); ++k) {
        const auto m = toFilament(poses[k]) * scale;
        if (useModel_) {
            renderer_.setModelInstanceTransform(boxes_[k].renderId, m);
        } else {
            renderer_.setBoxTransform(boxes_[k].renderId, m);
        }
    }
}
