#pragma once

// PhysicsWorld の担当の入れ物（窓口の pimpl）。PhysicsWorld*.cpp だけが使う。
// 宣言の逆順に壊れる: 名簿（ChBody への参照）を手放してから系を壊す。

#include "physics/BodyFactory.h"
#include "physics/BodyLoads.h"
#include "physics/BodyMotion.h"
#include "physics/BodyRegistry.h"
#include "physics/Cables.h"
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
    // system だけはコンストラクタ引数（系の種類）が要るので初期化子リストに残す。
    // 他の担当の初期化子は宣言の行に書く（初期化は宣言の順 = 下の並び）。
    Parts(PhysicsBackend backend, ContactMethod contact) : system(backend, contact) {
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

    // Chrono の系を（作り直しも含めて）組む。PhysicsWorld のコンストラクタと recreate が呼ぶ。
    void createSystem();
    // 本当に系から外す（Core。Multicore は退避だけ）。PhysicsWorld::disableBody から。
    void detachBody(std::size_t id);

    // system より先に宣言する = 系より後に壊れる（Bullet のペアキャッシュがフィルタを
    // 指すので、系の破棄の途中でも生きている必要がある）。参照を覚えるだけで、
    // 中身に触るのは attach から（system より先に初期化されても構わない）。
    wizengine::physics::StaticPairs staticPairs{system, bodies};
    wizengine::physics::PhysicsSystem system;
    wizengine::physics::BodyRegistry bodies{system};
    wizengine::physics::Sleeping sleeping{system, bodies};
    wizengine::physics::BodyMotion motion{bodies, sleeping};
    wizengine::physics::ContactMaterials materials{system, bodies};
    wizengine::physics::BodyFactory factory{system, bodies, sleeping, materials};
    wizengine::physics::SoftBodies soft{bodies, factory, materials};
    wizengine::physics::BodyLoads loads{system, bodies};
    wizengine::physics::GravityOverride gravity{system, bodies};
    wizengine::physics::VelocityDamping damping{bodies};
    wizengine::physics::Diagnostics diag{system, bodies};
    // system より先に壊れる。系が持ち続けるコールバックはデストラクタが切り離す。
    wizengine::physics::TerrainContact terrain{system, bodies, materials};
    // 系と名簿より後に宣言する = 系と名簿より先に壊れる（リンクと荷重コンテナへの参照を
    // 先に手放す。以前の PhysicsWorld のメンバと同じ順番）。
    wizengine::physics::Joints joints{system, bodies, motion};
    // joints より後 = 最初に壊れる（以前の PhysicsWorld では cables_ が窓口のメンバで、
    // p_ より先 = ジョイントより先に壊れていた。その順番を保つ）。
    wizengine::physics::Cables cables{system, bodies, materials};
};
