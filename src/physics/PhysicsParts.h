#pragma once

// PhysicsWorld の担当の入れ物（窓口の pimpl）。PhysicsWorld*.cpp だけが使う。
// 宣言の逆順に壊れる: 名簿（ChBody への参照）を手放してから系を壊す。

#include "physics/BodyRegistry.h"
#include "physics/PhysicsSystem.h"
#include "physics/PhysicsWorld.h"

struct PhysicsWorld::Parts {
    Parts(PhysicsBackend backend, ContactMethod contact)
        : system(backend, contact), bodies(system) {}

    wizengine::physics::PhysicsSystem system;
    wizengine::physics::BodyRegistry bodies;
};
