// ボディの追加の中継（本体は BodyFactory）と退場の順番（名簿は BodyRegistry）。

#include "physics/PhysicsParts.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChSystem.h>

std::size_t PhysicsWorld::addSphere(double radius, double density,
                                    const chrono::ChVector3d& pos,
                                    const chrono::ChQuaternion<>& rot, bool fixed,
                                    const std::vector<ExtraShape>& extras,
                                    const BodyOptions& options) {
    return p_->factory.addSphere(radius, density, pos, rot, fixed, extras, options);
}

std::size_t PhysicsWorld::addBox(double sx, double sy, double sz, double density,
                                 const chrono::ChVector3d& pos,
                                 const chrono::ChQuaternion<>& rot, bool fixed,
                                 const std::vector<ExtraShape>& extras,
                                 const BodyOptions& options) {
    return p_->factory.addBox(sx, sy, sz, density, pos, rot, fixed, extras, options);
}

std::size_t PhysicsWorld::addTriangleMesh(const std::vector<chrono::ChVector3d>& vertices,
                                          const std::vector<std::array<int, 3>>& triangles,
                                          double mass, const chrono::ChVector3d& pos,
                                          const chrono::ChQuaternion<>& rot, bool fixed,
                                          const std::vector<ExtraShape>& extras,
                                          const BodyOptions& options) {
    return p_->factory.addTriangleMesh(vertices, triangles, mass, pos, rot, fixed, extras, options);
}

std::size_t PhysicsWorld::addFrame(double mass, const chrono::ChVector3d& pos,
                                   const chrono::ChQuaternion<>& rot, bool fixed,
                                   const std::vector<ExtraShape>& extras,
                                   const BodyOptions& options, bool terrainContact) {
    return p_->factory.addFrame(mass, pos, rot, fixed, extras, options, terrainContact);
}

std::size_t PhysicsWorld::addConvexHull(const std::vector<chrono::ChVector3d>& points,
                                        double mass, const chrono::ChVector3d& pos,
                                        const chrono::ChQuaternion<>& rot,
                                        const std::vector<ExtraShape>& extras,
                                        chrono::ChVector3d* hullCenter,
                                        const BodyOptions& options) {
    return p_->factory.addConvexHull(points, mass, pos, rot, extras, hullCenter, options);
}

std::size_t PhysicsWorld::addSoftBody(const SoftBodySpec& spec, const chrono::ChVector3d& pos,
                                      const chrono::ChQuaternion<>& rot) {
    return p_->soft.add(spec, pos, rot);
}

void PhysicsWorld::setAlias(std::size_t id, std::size_t owner) {
    p_->bodies.setAlias(id, owner);
}

std::size_t PhysicsWorld::bodyCount() const {
    return p_->bodies.count();
}

void PhysicsWorld::disableBody(std::size_t id) {
    if (id >= p_->bodies.count() || !p_->bodies.active(id)) return;
    // テレインのゴーストは TerrainContact の持ち物: 系から外すと、直前の
    // ステップの接触が外れたボディを指したまま残るので、外さずに接触だけ止める。
    if (p_->terrain.isGhost(id)) {
        clearTerrain();
        return;
    }
    // ソフトボディは粒子を全部（剛体は自分だけ）。全員を退場させてから外す。
    // ばねを解かなくするのは SoftBodies が退場の通知で受け持つ。
    const auto members = p_->bodies.members(id);
    for (const std::size_t p : members) p_->bodies.retire(p);
    // Core では本当に系から外す（RemoveBody）。粒子も同じ。
    for (const std::size_t p : members) p_->detachBody(p);
}

// Core（Bullet）は RemoveBody を実装しているので、退場したボディは系から
// 本当に外す: ソルバと衝突系がその変数・形状を見なくなり、エディタで
// 大きさを何度も変えても空のボディが溜まらない。ChBody のオブジェクト自体は
// 名簿に残す（番号を詰めない・接触コールバックの逆引きが壊れない）。
// Multicore は系から外さない（退避のみ）: ボディを削除するとデータマネージャの
// 配列が崩れることがあるため。当たり判定のフラグを触らない理由は BodyRegistry::retire。
void PhysicsWorld::Parts::detachBody(std::size_t id) {
    if (system.backend() == PhysicsBackend::Multicore || id >= bodies.count()) return;
    auto& b = bodies.body(id);
    if (!b->GetSystem()) return;  // 既に外れている
    // 固定物の集合からは退場（retire → StaticPairs::onBodyRetiring）で外れている。
    system.sys()->RemoveBody(b);
}

bool PhysicsWorld::bodyActive(std::size_t id) const {
    return p_->bodies.active(id);
}
