#pragma once

#include <cstddef>
#include <string>
#include <memory>
#include <mutex>
#include <vector>

#include <math/vec3.h>

#include "BoxController.h"
#include "CameraObject.h"
#include "LightObject.h"
#include "GameObject.h"
#include "SceneComponent.h"
#include "PhysicsWorld.h"  // for BodyTransform
#include "WebRtcStreamer.h"  // for VideoCodec

namespace wizengine {
class Renderer;
}

// One scene tying physics bodies to their renderables, split for threading:
//   - build()          : set up both engines (single-threaded, before threads)
//   - stepPhysics(dt)  : PHYSICS thread - advance the sim, snapshot poses
//   - reset()          : PHYSICS thread - re-drop the boxes
//   - applyToRenderer(): RENDER thread  - push the latest poses to renderables
// The pose snapshot is the only shared state between the two threads.
// All scene content and parameters live in scene.cpp.
// Which physics backend the scene wants. Defined in scene.cpp with the rest of
// the scene configuration; main constructs the PhysicsWorld from it.
PhysicsBackend scenePhysicsBackend();

// Streaming settings (defined in scene.cpp with the rest of the config).
VideoCodec sceneVideoCodec();
int sceneVideoBitrate();

class Scene {
public:
    Scene(PhysicsWorld& physics, wizengine::Renderer& renderer);

    // The scene's cameras. Each one has its own Filament view and its own
    // browser endpoint; index 0 is the default camera. Input events are routed
    // to one of these (input thread); the render loop draws each in turn.
    CameraObject& camera(std::size_t index = 0) { return *cameras_[index]; }
    std::size_t cameraCount() const { return cameras_.size(); }

    // One grab/push component per camera, so each viewer holds and drives its
    // own object without fighting over a shared selection.
    BoxController& boxController(std::size_t cameraIndex = 0) {
        return *controllers_[cameraIndex];
    }

    // The scene's lights, in lightConfigs() order (scene.cpp). Change their
    // position/direction/colour/intensity from anywhere; the render loop
    // pushes the state to the renderer once per frame.
    LightObject& light(std::size_t index = 0) { return *lights_[index]; }
    std::size_t lightCount() const { return lights_.size(); }

    // Pick the object under a screen position, given in normalised device
    // coords (x, y in [-1, 1], y up), through the given camera. Selects it (or
    // clears that camera's selection on a miss) and returns the index or
    // BoxController::kNone. Called from the INPUT thread.
    std::size_t pickBoxAt(double ndcX, double ndcY, std::size_t cameraIndex);

    // ---- Component & object access (for SceneComponent / ObjectAction) ----
    void addComponent(std::unique_ptr<SceneComponent> component);
    // INPUT thread: offers a browser command to every component; true when one
    // of them consumed it.
    bool dispatchCommand(std::size_t camIndex, const nlohmann::json& msg);

    PhysicsWorld& physics() { return physics_; }
    wizengine::Renderer& renderer() { return renderer_; }
    std::size_t objectCount() const { return boxes_.size(); }
    GameObject& object(std::size_t i) { return boxes_[i]; }
    // True when objects are drawn as glTF instances (affects highlighting).
    bool drawnAsModel() const { return useModel_; }
    // Whiten strength for a grabbed object (kSelectedWhiten in scene.cpp).
    float selectedWhiten() const;
    // Highlight colour of the given camera's selection (scene.cpp).
    filament::math::float3 cameraColor(std::size_t cameraIndex) const;

    // JSON for the sidebar hierarchy, from the point of view of one camera:
    // cameras (with colours and who selected what) and the objects. Called
    // from the HTTP thread.
    std::string hierarchyJson(std::size_t cameraIndex);

    void build();

    void stepPhysics(double dt);
    // Re-drop the boxes (browser Reset / R key).
    // Integration substeps per physics update (perf/stability trade-off).
    int substeps() const;
    int solverIterations() const;
    // Physics updates per second - independent of the render frame rate.
    int physicsHz() const;
    double collisionEnvelope() const;
    double contactRecovery() const;
    // Stop stepping and rendering while no browser is watching (web mode only).
    bool idleWhenUnwatched() const;
    void reset();
    void applyToRenderer();

private:
    void placeBoxes();
    void snapshot();  // copy body poses into latestPoses_ (thread-safe)

    PhysicsWorld& physics_;
    wizengine::Renderer& renderer_;
    std::vector<std::unique_ptr<CameraObject>> cameras_;
    std::vector<std::unique_ptr<BoxController>> controllers_;
    std::vector<std::unique_ptr<LightObject>> lights_;

    std::vector<GameObject> boxes_;
    std::vector<std::unique_ptr<SceneComponent>> components_;
    // True when the dynamic objects are drawn as glTF instances rather than
    // the built-in cube (see kBoxModelPath in scene.cpp).
    bool useModel_ = false;
    float modelScale_ = 1.0f;  // model units -> metres (see scene.cpp)
    std::mutex poseMutex_;
    std::vector<BodyTransform> latestPoses_;  // one per box, in box order
};
