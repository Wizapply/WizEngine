// ボディの組み立て: 形ごとの追加・追加の当たり形状・形の記録の作成。

#include "physics/PhysicsWorldInternal.h"
#include "core/Log.h"

#include <chrono/collision/ChCollisionModel.h>
#ifdef WIZ_HAVE_COLLISION_SHAPES
#include <chrono/collision/ChCollisionShapeBox.h>
#include <chrono/collision/ChCollisionShapeSphere.h>
#endif
#ifdef WIZ_HAVE_CYLINDER_SHAPE
#include <chrono/collision/ChCollisionShapeCylinder.h>
#endif
#ifdef WIZ_HAVE_TRIMESH_SHAPE
#include <chrono/collision/ChCollisionShapeTriangleMesh.h>
#endif
#include <chrono/collision/bullet/ChCollisionUtilsBullet.h>  // 凸包の体積重心
#include <chrono/core/ChMatrix33.h>
#include <chrono/geometry/ChTriangleMeshConnected.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChBodyEasy.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

using namespace chrono;
using namespace wizengine::physics::compat;

namespace {

// 追加の当たり形状（attachExtraShapes が実際に足す形と同じもの）を proxy へ。
// 箱は各辺の長さ、球は x = 直径、円柱は x = 長さ（軸 X）・y = 直径。
void appendExtraProxies(const std::vector<PhysicsWorld::ExtraShape>& extras,
                        wterrain::BodyProxy& proxy) {
#ifdef WIZ_HAVE_COLLISION_SHAPES
    for (const PhysicsWorld::ExtraShape& s : extras) {
        wterrain::ShapeProxy sp;
        sp.position = toTerrainVec(s.pos);
        sp.rotation = toTerrainMat(ChMatrix33<>(s.rot));
        if (s.cylinder) {
#ifdef WIZ_HAVE_CYLINDER_SHAPE
            // ShapeProxy の円柱も形のローカル X が軸（部品の円柱と同じ）なので、
            // Chrono の形に重ねる「Z を X へ」の回転は要らない。
            sp.kind = wterrain::ShapeProxy::Kind::Cylinder;
            sp.radius = s.size.y() * 0.5;
            sp.halfLength = s.size.x() * 0.5;
#else
            continue;  // 当たり形状にならない（attachExtraShapes と同じ）
#endif
        } else if (s.sphere) {
            sp.kind = wterrain::ShapeProxy::Kind::Sphere;
            sp.radius = s.size.x() * 0.5;
        } else {
            sp.kind = wterrain::ShapeProxy::Kind::Box;
            sp.halfExtents = wterrain::Vec3(s.size.x() * 0.5, s.size.y() * 0.5, s.size.z() * 0.5);
        }
        proxy.shapes.push_back(sp);
    }
#else
    (void)extras;
    (void)proxy;
#endif
}

// テレインの接触に使う点の上限（凸包・三角メッシュの頂点）。凸包の入力
// （MeshCollision::loadCollisionPoints の既定 512）と同じ桁。1 ボディの判定は
// 点数 × 約 100 ns なので、512 点でも 0.05 ms。接触として残るのは深い順 16 点。
constexpr std::size_t kTerrainMaxProxyPoints = 512;

// 点を格子に寄せて 1 セル 1 点に間引く（MeshCollision::loadCollisionPoints と
// 同じ方式。格子を粗くしながら maxPoints 以下になるまで）。外形は 1 セル以内に
// 保たれる。点の順は元の順のまま（pointId を安定させるため）。
std::vector<ChVector3d> decimatePoints(std::vector<ChVector3d> pts, std::size_t maxPoints) {
    if (pts.size() <= maxPoints || pts.empty()) return pts;
    ChVector3d lo = pts[0], hi = pts[0];
    for (const auto& p : pts) {
        lo = ChVector3d(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
        hi = ChVector3d(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
    }
    const ChVector3d span = hi - lo;
    const double maxSpan = std::max({span.x(), span.y(), span.z(), 1e-9});
    for (int res = 64; res >= 2 && pts.size() > maxPoints; res /= 2) {
        std::unordered_set<std::uint64_t> cells;
        std::vector<ChVector3d> kept;
        kept.reserve(maxPoints);
        const double cell = maxSpan / res;
        for (const auto& p : pts) {
            const std::uint64_t cx = std::uint64_t((p.x() - lo.x()) / cell);
            const std::uint64_t cy = std::uint64_t((p.y() - lo.y()) / cell);
            const std::uint64_t cz = std::uint64_t((p.z() - lo.z()) / cell);
            if (cells.insert((cx << 42) | (cy << 21) | cz).second) kept.push_back(p);
        }
        pts = std::move(kept);
    }
    // 格子 2 でも多い（同じ点が大量など）ときは等間隔に抜く。
    if (pts.size() > maxPoints) {
        std::vector<ChVector3d> kept;
        kept.reserve(maxPoints);
        const double stride = double(pts.size()) / double(maxPoints);
        for (std::size_t k = 0; k < maxPoints; ++k) kept.push_back(pts[std::size_t(k * stride)]);
        pts = std::move(kept);
    }
    return pts;
}

// 点の集合（ボディローカル）1 個の形。
wterrain::ShapeProxy pointsShapeProxy(const std::vector<ChVector3d>& pts) {
    wterrain::ShapeProxy sp;
    sp.kind = wterrain::ShapeProxy::Kind::Points;
    sp.points.reserve(pts.size());
    for (const auto& p : pts) sp.points.push_back(toTerrainVec(p));
    return sp;
}

}  // namespace

void PhysicsWorld::prepareBody(ChBody& body, const BodyOptions& options) {
    // 衝突レイヤ（ファミリ 0〜7）。レイヤ 0 で除外無しなら既定のまま触らない。
    // 衝突系へ登録する前（AddBody の前）に呼ぶこと - 登録後の変更は Bullet が
    // モデルを Remove / Add し直し、Multicore の衝突系は Remove が未実装で
    // 例外を投げる。
    if (options.layer != 0 || options.nocollide != 0) {
        static bool warned = false;
        bool ok = true;
        if (auto model = body.GetCollisionModel()) {
            const int layer = std::max(0, std::min(kCollisionLayerCount - 1, options.layer));
            // レイヤ 0 は Chrono の既定ファミリなので触らない（ソフトボディの
            // 粒子は先に 8〜15 を付けてここへ来る - 上書きしない）。
            if (layer != 0) ok = setFamilyOf(&*model, layer, 0) && ok;
            for (int L = 0; L < kCollisionLayerCount; ++L) {
                if (options.nocollide & (1u << L)) ok = disallowFamily(&*model, L, 0) && ok;
            }
        }
        if (!ok && !warned) {
            warned = true;
            LOGW("physics", "this Chrono has no collision family API - collision "
                            "layers are ignored");
        }
    }
    // 重力オフ: Chrono に per-body の口があれば使う（9.0 には無い）。無くても
    // GravityOverride::cancel が重力ぶんを毎ステップ打ち消すので、どちらでも効く。
    if (!options.gravity) setUseGravity(&body, false, 0);
}

void PhysicsWorld::finishBody(const std::shared_ptr<chrono::ChBody>& body,
                              const std::shared_ptr<chrono::ChContactMaterial>& mat,
                              const BodyOptions& options, bool allowSleeping) {
    registerBody(body, mat, options, allowSleeping);
}

void PhysicsWorld::attachExtraShapes(ChBody& body, const std::vector<ExtraShape>& extras,
                                     const std::shared_ptr<ChContactMaterial>& mat) {
    if (extras.empty()) return;
    static bool warned = false;
    static bool warnedCylinder = false;
    bool ok = true;
#ifdef WIZ_HAVE_COLLISION_SHAPES
    for (const ExtraShape& s : extras) {
        std::shared_ptr<ChCollisionShape> shape;
        ChQuaternion<> rot = s.rot;
        if (s.cylinder) {
            // Chrono の円柱は形状座標系の Z 軸が軸。部品の円柱は X 軸なので
            // 「Z を X へ」の回転を部品の回転に重ねる。
#ifdef WIZ_HAVE_CYLINDER_SHAPE
            shape = chrono_types::make_shared<ChCollisionShapeCylinder>(
                mat, s.size.y() * 0.5, s.size.x());
            rot = s.rot * quatFromZAxis(ChVector3d(1, 0, 0));
#else
            if (!warnedCylinder) {
                warnedCylinder = true;
                LOGW("physics", "this Chrono build has no cylinder collision shape - "
                                "cylinder parts with collide=\"true\" are visual only");
            }
            continue;
#endif
        } else if (s.sphere) {
            shape = chrono_types::make_shared<ChCollisionShapeSphere>(mat, s.size.x() * 0.5);
        } else {
            shape = chrono_types::make_shared<ChCollisionShapeBox>(mat, s.size.x(),
                                                                   s.size.y(), s.size.z());
        }
        ok = addShapeImpl(body, shape, ChFrame<>(s.pos, rot), 0) && ok;
    }
#else
    ok = false;
#endif
    if (!ok && !warned) {
        warned = true;
        LOGW("physics", "this Chrono build has no compound collision shapes - "
                        "prefab parts with collide=\"true\" are visual only");
    }
}

std::size_t PhysicsWorld::addSphere(double radius, double density,
                                    const ChVector3d& pos,
                                    const ChQuaternion<>& rot, bool fixed,
                                    const std::vector<ExtraShape>& extras,
                                    const BodyOptions& options) {
    const auto mat = p_->materials.materialFor(options);
    auto b = chrono_types::make_shared<ChBodyEasySphere>(
        radius, density, /*visualize*/ true, /*collide*/ true, mat);
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    b->EnableCollision(true);
    p_->sleeping.applyTo(*b);
    prepareBody(*b, options);
    p_->system.sys()->AddBody(b);
    finishBody(b, mat, options, /*allowSleeping*/ true);
    const std::size_t id = p_->bodies.count() - 1;
    {
        // テレインの接触用の形の記録（本体の球 + 追加の当たり形状）。
        wterrain::BodyProxy proxy;
        wterrain::ShapeProxy sp;
        sp.kind = wterrain::ShapeProxy::Kind::Sphere;
        sp.radius = radius;
        proxy.shapes.push_back(sp);
        appendExtraProxies(extras, proxy);
        setTerrainProxy(id, proxy);
    }
    return id;
}

std::size_t PhysicsWorld::addBox(double sx, double sy, double sz, double density,
                                 const ChVector3d& pos,
                                 const ChQuaternion<>& rot, bool fixed,
                                 const std::vector<ExtraShape>& extras,
                                 const BodyOptions& options) {
    const auto mat = p_->materials.materialFor(options);
    auto b = chrono_types::make_shared<ChBodyEasyBox>(
        sx, sy, sz, density, /*visualize*/ true, /*collide*/ true, mat);
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    b->EnableCollision(true);
    p_->sleeping.applyTo(*b);
    prepareBody(*b, options);
    p_->system.sys()->AddBody(b);
    finishBody(b, mat, options, /*allowSleeping*/ true);
    const std::size_t id = p_->bodies.count() - 1;
    {
        // テレインの接触用の形の記録（本体の箱 + 追加の当たり形状）。
        wterrain::BodyProxy proxy;
        wterrain::ShapeProxy sp;
        sp.kind = wterrain::ShapeProxy::Kind::Box;
        sp.halfExtents = wterrain::Vec3(sx * 0.5, sy * 0.5, sz * 0.5);
        proxy.shapes.push_back(sp);
        appendExtraProxies(extras, proxy);
        setTerrainProxy(id, proxy);
    }
    return id;
}

std::size_t PhysicsWorld::addTriangleMesh(const std::vector<ChVector3d>& vertices,
                                          const std::vector<std::array<int, 3>>& triangles,
                                          double mass, const ChVector3d& pos,
                                          const ChQuaternion<>& rot, bool fixed,
                                          const std::vector<ExtraShape>& extras,
                                          const BodyOptions& options) {
#if defined(WIZ_HAVE_COLLISION_SHAPES) && defined(WIZ_HAVE_TRIMESH_SHAPE)
    if (vertices.empty() || triangles.empty()) return kInvalidId;
    auto mesh = chrono_types::make_shared<ChTriangleMeshConnected>();
    mesh->GetCoordsVertices() = vertices;
    auto& tris = mesh->GetIndicesVertexes();
    tris.reserve(triangles.size());
    double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
    for (const auto& v : vertices) {
        const double c[3] = {v.x(), v.y(), v.z()};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], c[a]);
            hi[a] = std::max(hi[a], c[a]);
        }
    }
    for (const auto& t : triangles) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || std::size_t(t[0]) >= vertices.size() ||
            std::size_t(t[1]) >= vertices.size() || std::size_t(t[2]) >= vertices.size()) {
            continue;
        }
        tris.push_back(ChVector3i(t[0], t[1], t[2]));
    }
    if (tris.empty()) return kInvalidId;

    const auto mat = p_->materials.materialFor(options);
    auto b = chrono_types::make_shared<ChBody>();
    const double m = std::max(mass, 1e-3);
    b->SetMass(m);
    // 慣性は外接箱の均質な箱として（凹メッシュの体積は当てにならない）。
    double L[3];
    for (int a = 0; a < 3; ++a) L[a] = std::max(hi[a] - lo[a], 0.01);
    b->SetInertiaXX(ChVector3d(m / 12.0 * (L[1] * L[1] + L[2] * L[2]),
                               m / 12.0 * (L[0] * L[0] + L[2] * L[2]),
                               m / 12.0 * (L[0] * L[0] + L[1] * L[1])));
    // 三角メッシュ形状: is_static = 固定なら true（Bullet が静的メッシュとして
    // 最適化する）、is_convex = false（凹形状のため）。sphere_swept は 0。
    auto shape = chrono_types::make_shared<ChCollisionShapeTriangleMesh>(
        mat, mesh, fixed, /*is_convex*/ false, /*sphere_swept*/ 0.0);
    if (!addShapeImpl(*b, shape, ChFrame<>(), 0)) return kInvalidId;
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    b->EnableCollision(true);
    p_->sleeping.applyTo(*b);
    prepareBody(*b, options);
    p_->system.sys()->AddBody(b);
    finishBody(b, mat, options, /*allowSleeping*/ true);
    const std::size_t id = p_->bodies.count() - 1;
    if (!fixed) {
        LOGW("physics", "triangle-mesh body is not fixed - concave mesh vs mesh "
                        "collision is slow (GImpact); prefer fixed=\"true\"");
        // テレインの接触: 頂点（ボディローカル。ChBody の原点 = 頂点の原点）を
        // 間引いて点の集合に + 追加の当たり形状。固定の物はテレインと接触を
        // 作らないので記録しない（大きな固定の器・トンネルの頂点列を持たない）。
        // 走行中に固定を外された物は安全網（AABB）になる。
        wterrain::BodyProxy proxy;
        proxy.shapes.push_back(pointsShapeProxy(decimatePoints(vertices, kTerrainMaxProxyPoints)));
        appendExtraProxies(extras, proxy);
        setTerrainProxy(id, proxy);
    }
    return id;
#else
    (void)vertices; (void)triangles; (void)mass; (void)pos; (void)rot; (void)fixed;
    (void)extras; (void)options;
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOGW("physics", "this Chrono build has no triangle-mesh collision shape - "
                        "collision=\"trimesh\" falls back to the convex hull");
    }
    return kInvalidId;
#endif
}

std::size_t PhysicsWorld::addFrame(double mass, const ChVector3d& pos,
                                   const ChQuaternion<>& rot, bool fixed,
                                   const std::vector<ExtraShape>& extras,
                                   const BodyOptions& options, bool terrainContact) {
    const auto mat = p_->materials.materialFor(options);
    auto b = chrono_types::make_shared<ChBody>();
    const double m = std::max(mass, 1e-3);
    b->SetMass(m);
    // 慣性: 追加形状の外接箱（回転は無視、ローカルの軸に沿った箱）を
    // 均質な箱として。無ければ 0.5 m の箱（フレームだけなら動かないので
    // 値はほぼ効かない）。
    double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
    for (const ExtraShape& e : extras) {
        const double hx = e.sphere ? e.size.x() * 0.5 : e.size.x() * 0.5;
        const double hy = e.sphere ? e.size.x() * 0.5 : e.size.y() * 0.5;
        const double hz = e.sphere ? e.size.x() * 0.5 : e.size.z() * 0.5;
        const double c[3] = {e.pos.x(), e.pos.y(), e.pos.z()};
        const double h[3] = {hx, hy, hz};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], c[a] - h[a]);
            hi[a] = std::max(hi[a], c[a] + h[a]);
        }
    }
    double L[3] = {0.5, 0.5, 0.5};
    if (!extras.empty()) {
        for (int a = 0; a < 3; ++a) L[a] = std::max(hi[a] - lo[a], 0.01);
    }
    b->SetInertiaXX(ChVector3d(m / 12.0 * (L[1] * L[1] + L[2] * L[2]),
                               m / 12.0 * (L[0] * L[0] + L[2] * L[2]),
                               m / 12.0 * (L[0] * L[0] + L[1] * L[1])));
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(fixed);
    if (!extras.empty()) b->EnableCollision(true);
    p_->sleeping.applyTo(*b);
    prepareBody(*b, options);
    p_->system.sys()->AddBody(b);
    finishBody(b, mat, options, /*allowSleeping*/ true);
    const std::size_t id = p_->bodies.count() - 1;
    if (terrainContact && !extras.empty()) {
        // テレインの接触: 追加の当たり形状（箱・球・円柱）だけ。extras が空の
        // フレームは当たり判定が無いので、テレインにも当たらない。
        wterrain::BodyProxy proxy;
        appendExtraProxies(extras, proxy);
        if (!proxy.shapes.empty()) setTerrainProxy(id, proxy);
    }
    return id;
}

std::size_t PhysicsWorld::addConvexHull(
    const std::vector<ChVector3d>& points, double mass,
    const ChVector3d& pos, const ChQuaternion<>& rot,
    const std::vector<ExtraShape>& extras, ChVector3d* hullCenter,
    const BodyOptions& options) {
    const auto mat = p_->materials.materialFor(options);
    // 凸包の体積重心を先に求める。ChBodyEasyConvexHull は内部で同じ計算を
    // して頂点をここへ寄せる（ボディの原点 = 重心）ので、呼び出し側が
    // 見た目を同じだけずらせるように返す。Chrono と同じ道具（Bullet の
    // 凸包 + 三角メッシュの質量特性）で出すから、寄せ量と一致する。
    ChVector3d center(0.0, 0.0, 0.0);
    if (points.size() >= 4) {
        chrono::ChTriangleMeshConnected hull;
        chrono::bt_utils::ChConvexHullLibraryWrapper wrapper;
        wrapper.ComputeHull(points, hull);
        double volume = 0.0;
        chrono::ChMatrix33<> inertia;
        hull.ComputeMassProperties(true, volume, center, inertia);
    }
    if (hullCenter) *hullCenter = center;

    // visualize=false: our renderer draws the glTF model itself; Chrono's
    // visual assets are unused here.
    //
    // Chrono's constructor takes the points by NON-const reference and
    // translates them in place (it moves the barycentre onto the centre of
    // mass) - hence the local copy: the caller's point cloud is shared by
    // every body and must not be mutated.
    //
    // 密度 1 で作って、あとから文書の質量へ合わせる。凸包の体積はモデルの
    // 大きさ次第（文書の size とは無関係）なので、密度から出すと質量が
    // 桁違いになる - 以前は 1 m のモデルに size 0.05 を書くと 100 kg 超の
    // 「りんご」ができて、触れた物を弾き飛ばしていた。
    std::vector<ChVector3d> local = points;
    auto b = chrono_types::make_shared<ChBodyEasyConvexHull>(
        local, /*density*/ 1.0, /*visualize*/ false, /*collide*/ true, mat);
    const double unitMass = b->GetMass();  // 密度 1 なので = 凸包の体積
    if (unitMass > 1e-12 && std::isfinite(unitMass)) {
        // 慣性は Chrono が凸包から出した形（密度 1）のまま、質量の比で伸縮。
        b->SetInertia(b->GetInertia() * (mass / unitMass));
    } else {
        // 退化した凸包（平ら・点が 4 個未満）: 外接半径の球として扱う。
        double r2 = 1e-6;
        for (const auto& p : points) {
            const ChVector3d d = p - center;
            r2 = std::max(r2, d.x() * d.x() + d.y() * d.y() + d.z() * d.z());
        }
        const double i = 0.4 * mass * r2;
        b->SetInertiaXX(ChVector3d(i, i, i));
        LOGW("physics",
             "convex hull is degenerate (%zu points) - using a sphere inertia",
             points.size());
    }
    b->SetMass(mass);
    // 部品の位置は「寄せる前の原点」基準のまま = 原点が重心から離れた
    // モデルではそのぶんずれる（見た目は hullCenter で合わせるが、複合形状
    // の部品までは動かさない）。
    attachExtraShapes(*b, extras, mat);
    b->SetPos(pos);
    b->SetRot(rot);
    b->SetFixed(false);
    b->EnableCollision(true);
    p_->sleeping.applyTo(*b);
    prepareBody(*b, options);
    p_->system.sys()->AddBody(b);
    finishBody(b, mat, options, /*allowSleeping*/ true);
    const std::size_t id = p_->bodies.count() - 1;
    {
        // テレインの接触: Chrono が当たり形状にした凸包の頂点そのもの
        // （GetMesh = 体積重心へ寄せた後 = ボディローカル。ChBodyEasyConvexHull が
        // 衝突形状に渡す points_reduced と同じ列）。寄せる前の点を使うなら
        // 「p − center」になるが、こちらは Chrono の形と完全に一致する。
        // 凸包が作れなかった（点が 4 個未満・退化 = 頂点が空）ときは Chrono 側にも
        // 凸包の当たり形状が無いので、記録しない（テレインにも当てない）。
        // 追加の当たり形状は attachExtraShapes と同じ置き場（ボディ原点 = 重心
        // 基準。寄せる前の原点基準ではない）なので、そのまま足す。
        std::vector<ChVector3d> hullPts;
        if (const auto mesh = b->GetMesh()) hullPts = mesh->GetCoordsVertices();
        wterrain::BodyProxy proxy;
        if (!hullPts.empty())
            proxy.shapes.push_back(pointsShapeProxy(decimatePoints(hullPts, kTerrainMaxProxyPoints)));
        appendExtraProxies(extras, proxy);
        if (!proxy.shapes.empty()) setTerrainProxy(id, proxy);
    }
    return id;
}
