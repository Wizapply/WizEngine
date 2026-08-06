#pragma once

#include <cstddef>
#include <memory>
#include <vector>

class PhysicsWorld;
struct GameObject;

// A behaviour attached to ONE object, called from the PHYSICS thread every
// step before integration. This is the per-object counterpart of
// SceneComponent: use a SceneComponent for scene-wide logic, an ObjectAction
// for "this object spins" / "this object pulses" kinds of behaviour.
class ObjectAction {
public:
    virtual ~ObjectAction() = default;
    virtual void onPhysicsStep(GameObject& object, PhysicsWorld& physics,
                               double dt) = 0;
};

// One dynamic object in the scene: a physics body, its renderable, and any
// actions attached to it.
struct GameObject {
    std::size_t physId = 0;
    std::size_t renderId = 0;
    std::vector<std::unique_ptr<ObjectAction>> actions;
};
