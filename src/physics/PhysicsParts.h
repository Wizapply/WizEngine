#pragma once

// PhysicsWorld の担当の入れ物（窓口の pimpl）。PhysicsWorld*.cpp だけが使う。
// 宣言の逆順に壊れる: 名簿（ChBody への参照）を手放してから系を壊す。

#include "physics/BodyMotion.h"
#include "physics/BodyRegistry.h"
#include "physics/ContactMaterials.h"
#include "physics/PhysicsSystem.h"
#include "physics/PhysicsWorld.h"
#include "physics/Sleeping.h"

struct PhysicsWorld::Parts {
    Parts(PhysicsBackend backend, ContactMethod contact)
        : system(backend, contact),
          bodies(system),
          sleeping(system, bodies),
          motion(bodies, sleeping),
          materials(system, bodies) {
        // 出来事の受け手はここで登録する。受け手どうしは順番に依存しない。
        bodies.addListener(&sleeping);
        bodies.addListener(&materials);
    }

    wizengine::physics::PhysicsSystem system;
    wizengine::physics::BodyRegistry bodies;
    wizengine::physics::Sleeping sleeping;
    wizengine::physics::BodyMotion motion;
    wizengine::physics::ContactMaterials materials;
};
