#pragma once

// PhysicsWorld の担当の入れ物（窓口の pimpl）。PhysicsWorld*.cpp だけが使う。
// 宣言の逆順に壊れる: 名簿（ChBody への参照）を手放してから系を壊す。

#include "physics/BodyFactory.h"
#include "physics/BodyLoads.h"
#include "physics/BodyMotion.h"
#include "physics/BodyRegistry.h"
#include "physics/ContactMaterials.h"
#include "physics/Diagnostics.h"
#include "physics/GravityOverride.h"
#include "physics/Joints.h"
#include "physics/PhysicsSystem.h"
#include "physics/PhysicsWorld.h"
#include "physics/Sleeping.h"
#include "physics/SoftBodies.h"
#include "physics/StaticPairs.h"
#include "physics/TerrainContact.h"
#include "physics/VelocityDamping.h"

struct PhysicsWorld::Parts {
    Parts(PhysicsBackend backend, ContactMethod contact)
        : staticPairs(system, bodies),  // 参照を覚えるだけ（中身に触るのは attach から）
          system(backend, contact),
          bodies(system),
          sleeping(system, bodies),
          motion(bodies, sleeping),
          materials(system, bodies),
          factory(system, bodies, sleeping, materials),
          soft(bodies, factory, materials),
          loads(system, bodies),
          gravity(system, bodies),
          damping(bodies),
          diag(system, bodies),
          terrain(system, bodies, materials),
          joints(system, bodies, motion) {
        // 出来事の受け手はここで登録する。受け手どうしは順番に依存しない
        // （それぞれ自分の状態と Chrono の別々の部分だけを触る）。
        bodies.events().add(&sleeping);
        bodies.events().add(&materials);
        bodies.events().add(&loads);
        bodies.events().add(&gravity);
        bodies.events().add(&staticPairs);
        bodies.events().add(&terrain);
        bodies.events().add(&soft);
    }

    // system より先に宣言する = 系より後に壊れる（Bullet のペアキャッシュがフィルタを
    // 指すので、系の破棄の途中でも生きている必要がある）。
    wizengine::physics::StaticPairs staticPairs;
    wizengine::physics::PhysicsSystem system;
    wizengine::physics::BodyRegistry bodies;
    wizengine::physics::Sleeping sleeping;
    wizengine::physics::BodyMotion motion;
    wizengine::physics::ContactMaterials materials;
    wizengine::physics::BodyFactory factory;
    wizengine::physics::SoftBodies soft;
    wizengine::physics::BodyLoads loads;
    wizengine::physics::GravityOverride gravity;
    wizengine::physics::VelocityDamping damping;
    wizengine::physics::Diagnostics diag;
    // system より先に壊れる。系が持ち続けるコールバックはデストラクタが切り離す。
    wizengine::physics::TerrainContact terrain;
    // 最後に宣言する = 最初に壊れる（以前の PhysicsWorld のメンバと同じく、系と名簿より
    // 先にリンクと荷重コンテナへの参照を手放す）。
    wizengine::physics::Joints joints;
};
