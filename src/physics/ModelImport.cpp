#include "physics/ModelImport.h"

#include "core/AssetError.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

// Chrono::Parsers は CMake の WIZ_WITH_CHRONO_PARSERS で付ける。ヘッダが
// 無ければこのファイルは「無い」と答えるだけになる。
#if defined(WIZ_WITH_CHRONO_PARSERS) && __has_include(<chrono_parsers/ChParserURDF.h>)
#define WIZ_HAVE_PARSERS 1
#include <chrono/core/ChMatrix33.h>
#include <chrono/collision/ChCollisionModel.h>
#include <chrono/collision/ChCollisionShape.h>
#include <chrono/collision/ChCollisionShapeBox.h>
#include <chrono/collision/ChCollisionShapeConvexHull.h>
#include <chrono/collision/ChCollisionShapeCylinder.h>
#include <chrono/collision/ChCollisionShapeSphere.h>
#include <chrono/collision/ChCollisionShapeTriangleMesh.h>
#include <chrono/geometry/ChTriangleMeshConnected.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChLinkDistance.h>
#include <chrono/physics/ChLinkLock.h>
#include <chrono/physics/ChLinkMotorLinearForce.h>
#include <chrono/physics/ChLinkMotorLinearPosition.h>
#include <chrono/physics/ChLinkMotorLinearSpeed.h>
#include <chrono/physics/ChLinkMotorRotationAngle.h>
#include <chrono/physics/ChLinkMotorRotationSpeed.h>
#include <chrono/physics/ChLinkMotorRotationTorque.h>
#include <chrono/physics/ChLinkTSDA.h>
#include <chrono/physics/ChLinkUniversal.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono_parsers/ChParserURDF.h>
#if __has_include(<chrono_parsers/ChParserOpenSim.h>)
#include <chrono_parsers/ChParserOpenSim.h>
#define WIZ_HAVE_PARSER_OPENSIM 1
#endif
#if __has_include(<chrono_parsers/ChParserAdams.h>)
#include <chrono_parsers/ChParserAdams.h>
#define WIZ_HAVE_PARSER_ADAMS 1
#endif
#endif

namespace ed = wizengine::editor;

namespace wizengine {

bool importAvailable() {
#ifdef WIZ_HAVE_PARSERS
    return true;
#else
    return false;
#endif
}

#ifdef WIZ_HAVE_PARSERS
namespace {

using namespace chrono;

// ---- 版差の吸収（PhysicsWorld.cpp と同じ SFINAE の手口）--------------------
template <typename S>
auto bodiesOf(const S* s, int) -> decltype(s->GetBodies()) {
    return s->GetBodies();
}
template <typename S>
auto bodiesOf(const S* s, long) -> decltype(s->Get_bodylist()) {
    return s->Get_bodylist();
}
template <typename S>
auto linksOf(const S* s, int) -> decltype(s->GetLinks()) {
    return s->GetLinks();
}
template <typename S>
auto linksOf(const S* s, long) -> decltype(s->Get_linklist()) {
    return s->Get_linklist();
}
// リンクの絶対座標系（マーカー 2 側）。Chrono 9 は GetFrame2Abs、旧版の
// ChLinkLock は GetLinkAbsoluteCoords。どちらも無ければ false。
template <typename L>
auto linkFrame(const L* l, ChFrame<>& out, int) -> decltype(l->GetFrame2Abs(), bool()) {
    out = l->GetFrame2Abs();
    return true;
}
template <typename L>
auto linkFrame(const L* l, ChFrame<>& out, long)
    -> decltype(l->GetLinkAbsoluteCoords(), bool()) {
    const ChCoordsys<> c = l->GetLinkAbsoluteCoords();
    out = ChFrame<>(c.pos, c.rot);
    return true;
}
template <typename L>
bool linkFrame(const L*, ChFrame<>&, ...) {
    return false;
}
// 可動範囲（Chrono 9: LimitRz() が ChLinkLimit&、IsActive / GetMin / GetMax）。
template <typename L>
auto limitRz(L* l, double& lo, double& hi, int)
    -> decltype(l->LimitRz().IsActive(), l->LimitRz().GetMin(), bool()) {
    if (!l->LimitRz().IsActive()) return false;
    lo = l->LimitRz().GetMin();
    hi = l->LimitRz().GetMax();
    return true;
}
template <typename L>
bool limitRz(L*, double&, double&, long) {
    return false;
}
template <typename L>
auto limitZ(L* l, double& lo, double& hi, int)
    -> decltype(l->LimitZ().IsActive(), l->LimitZ().GetMin(), bool()) {
    if (!l->LimitZ().IsActive()) return false;
    lo = l->LimitZ().GetMin();
    hi = l->LimitZ().GetMax();
    return true;
}
template <typename L>
bool limitZ(L*, double&, double&, long) {
    return false;
}
// 関数の値（モータの目標）: Chrono 9 は GetVal、旧版は Get_y。
template <typename F>
auto functionValue(const F* f, double x, int) -> decltype(f->GetVal(x)) {
    return f->GetVal(x);
}
template <typename F>
auto functionValue(const F* f, double x, long) -> decltype(f->Get_y(x)) {
    return f->Get_y(x);
}
// 箱の寸法: Chrono 9 は GetLengths、旧版は GetHalflengths × 2。
template <typename B>
auto boxLengths(const B* b, int) -> decltype(b->GetLengths()) {
    return b->GetLengths();
}
template <typename B>
auto boxLengths(const B* b, long) -> decltype(b->GetHalflengths() * 2.0) {
    return b->GetHalflengths() * 2.0;
}
// 衝突モデルの形状一覧（Chrono 9: GetShapeInstances = (形状, 座標系) の組）。
template <typename M>
auto shapeInstances(const M* m, int) -> decltype(m->GetShapeInstances()) {
    return m->GetShapeInstances();
}

ed::Vec3d toVec(const ChVector3d& v) {
    return ed::Vec3d{v.x(), v.y(), v.z()};
}
// 四元数 → エディタのオイラー角（度、R = Rz*Ry*Rx）。式は SceneMath.h の
// eulerDegreesFromQuat と同じ（あちらは Filament の math ヘッダを引くので、
// 物理層のこのファイルからは含めない）。
ed::Vec3d eulerOf(const ChQuaternion<>& q) {
    const ChMatrix33<> m(q);
    const double r00 = m(0, 0), r10 = m(1, 0);
    const double r20 = m(2, 0), r21 = m(2, 1), r22 = m(2, 2);
    const double clamped = std::max(-1.0, std::min(1.0, -r20));
    const double y = std::asin(clamped);
    double x, z;
    if (std::abs(r20) < 0.999999) {
        x = std::atan2(r21, r22);
        z = std::atan2(r10, r00);
    } else {
        x = std::atan2(-m(1, 2), m(1, 1));
        z = 0.0;
    }
    const double toDeg = 180.0 / CH_PI;
    return ed::Vec3d{x * toDeg, y * toDeg, z * toDeg};
}
std::string nameOf(const ChObj& o) {
    return std::string(o.GetName());
}

// 1 個の当たり形状をプレハブの部品へ。凸包・三角メッシュは外接箱。
bool shapeToPart(const std::shared_ptr<ChCollisionShape>& shape, const ChFrame<>& frame,
                 ed::PartDesc& part, std::string& note) {
    part.position = toVec(frame.GetPos());
    part.rotation = eulerOf(frame.GetRot());
    part.collide = true;
    switch (shape->GetType()) {
        case ChCollisionShape::Type::BOX: {
            auto box = std::static_pointer_cast<ChCollisionShapeBox>(shape);
            part.kind = ed::PartKind::Box;
            part.size = toVec(boxLengths(box.get(), 0));
            return true;
        }
        case ChCollisionShape::Type::SPHERE: {
            auto s = std::static_pointer_cast<ChCollisionShapeSphere>(shape);
            part.kind = ed::PartKind::Sphere;
            const double d = s->GetRadius() * 2.0;
            part.size = ed::Vec3d{d, d, d};
            return true;
        }
        case ChCollisionShape::Type::CYLINDER: {
            // Chrono の円柱は Z 軸、部品の円柱は X 軸: Z を X へ回す。
            auto c = std::static_pointer_cast<ChCollisionShapeCylinder>(shape);
            part.kind = ed::PartKind::Cylinder;
            part.size = ed::Vec3d{c->GetHeight(), c->GetRadius() * 2.0, c->GetRadius() * 2.0};
            const ChQuaternion<> zToX = QuatFromAngleY(CH_PI_2);
            part.rotation = eulerOf(frame.GetRot() * zToX);
            return true;
        }
        case ChCollisionShape::Type::CONVEXHULL: {
            auto h = std::static_pointer_cast<ChCollisionShapeConvexHull>(shape);
            ChVector3d lo(1e9, 1e9, 1e9), hi(-1e9, -1e9, -1e9);
            for (const auto& p : h->GetPoints()) {
                lo = ChVector3d(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()),
                                std::min(lo.z(), p.z()));
                hi = ChVector3d(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()),
                                std::max(hi.z(), p.z()));
            }
            if (hi.x() < lo.x()) return false;
            part.kind = ed::PartKind::Box;
            part.size = toVec(hi - lo);
            part.position = toVec(frame.GetPos() + frame.GetRot().Rotate((lo + hi) * 0.5));
            note = "convex hull approximated by its bounding box";
            return true;
        }
        case ChCollisionShape::Type::TRIANGLEMESH: {
            auto t = std::static_pointer_cast<ChCollisionShapeTriangleMesh>(shape);
            auto mesh = t->GetMesh();
            if (!mesh) return false;
            ChVector3d lo(1e9, 1e9, 1e9), hi(-1e9, -1e9, -1e9);
            for (const auto& p : mesh->GetCoordsVertices()) {
                lo = ChVector3d(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()),
                                std::min(lo.z(), p.z()));
                hi = ChVector3d(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()),
                                std::max(hi.z(), p.z()));
            }
            if (hi.x() < lo.x()) return false;
            part.kind = ed::PartKind::Box;
            part.size = toVec(hi - lo);
            part.position = toVec(frame.GetPos() + frame.GetRot().Rotate((lo + hi) * 0.5));
            note = "triangle mesh approximated by its bounding box";
            return true;
        }
        default:
            return false;
    }
}

// 系の中身を文書へ。
void systemToDocument(ChSystem& sys, const std::string& stem, ed::SceneDocument& out,
                      std::vector<std::string>& warnings) {
    // ボディ → BodyDesc（と、形が複数ならプレハブ）。ChBodyFrame* → 番号の
    // 対応表はリンクの端点を引くため（ChBody は多重継承なので ChBody* と
    // ChBodyFrame* の値は違う - 必ず ChBodyFrame* に揃える）。
    std::map<const ChBodyFrame*, int> indexOf;
    const auto bodies = bodiesOf(&sys, 0);
    int unnamed = 0;
    for (const auto& body : bodies) {
        ed::BodyDesc d;
        d.name = ed::sanitizeEventName(nameOf(*body));
        if (d.name.empty()) d.name = stem + "_" + std::to_string(unnamed++);
        d.position = toVec(body->GetPos());
        d.rotation = eulerOf(body->GetRot());
        d.mass = body->GetMass();
        d.fixed = body->IsFixed();
        d.color = ed::Color3{0.62f, 0.66f, 0.72f};

        std::vector<ed::PartDesc> parts;
        if (auto model = body->GetCollisionModel()) {
            for (const auto& inst : shapeInstances(model.get(), 0)) {
                ed::PartDesc part;
                std::string note;
                if (!shapeToPart(inst.first, inst.second, part, note)) {
                    warnings.push_back("body '" + d.name + "': a collision shape of an "
                                       "unsupported type was dropped");
                    continue;
                }
                if (!note.empty()) warnings.push_back("body '" + d.name + "': " + note);
                part.name = "shape" + std::to_string(parts.size());
                part.color = d.color;
                parts.push_back(ed::clampPart(part));
            }
        }
        const bool single = parts.size() == 1 && parts[0].kind != ed::PartKind::Cylinder &&
                            parts[0].position.x == 0.0 && parts[0].position.y == 0.0 &&
                            parts[0].position.z == 0.0 && parts[0].rotation.x == 0.0 &&
                            parts[0].rotation.y == 0.0 && parts[0].rotation.z == 0.0;
        if (single) {
            d.shape = parts[0].kind == ed::PartKind::Sphere ? ed::ShapeKind::Sphere
                                                             : ed::ShapeKind::Box;
            d.collision = d.shape;
            d.size = parts[0].size;
        } else if (parts.empty()) {
            // 形の無いボディ（URDF の仮想リンクなど）: 小さな球で見えるようにする。
            d.shape = ed::ShapeKind::Sphere;
            d.collision = ed::ShapeKind::Sphere;
            d.size = ed::Vec3d{0.05, 0.05, 0.05};
            warnings.push_back("body '" + d.name + "' has no collision shape - drawn as a "
                               "5 cm sphere");
        } else {
            // 複数 / ずれた形: geom の無いフレーム + collide 部品のプレハブ。
            ed::PrefabDesc pf;
            pf.name = ed::sanitizeEventName(d.name + "_shapes");
            pf.parts = parts;
            out.prefabs.push_back(pf);
            d.shape = ed::ShapeKind::None;
            d.collision = ed::ShapeKind::None;
            d.prefab = pf.name;
            // 外接箱（表示用の寸法）。
            ed::Vec3d lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
            for (const auto& p : parts) {
                const double h[3] = {p.size.x * 0.5, p.size.y * 0.5, p.size.z * 0.5};
                const double c[3] = {p.position.x, p.position.y, p.position.z};
                double* L[3] = {&lo.x, &lo.y, &lo.z};
                double* H[3] = {&hi.x, &hi.y, &hi.z};
                for (int a = 0; a < 3; ++a) {
                    *L[a] = std::min(*L[a], c[a] - h[a]);
                    *H[a] = std::max(*H[a], c[a] + h[a]);
                }
            }
            d.size = ed::Vec3d{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
        }
        indexOf[static_cast<const ChBodyFrame*>(body.get())] = int(out.bodies.size());
        out.bodies.push_back(ed::clampBody(d));
    }

    // リンク → JointDesc。
    const double toDeg = 180.0 / CH_PI;
    for (const auto& link : linksOf(&sys, 0)) {
        auto lk = std::dynamic_pointer_cast<ChLink>(link);
        if (!lk) continue;  // 節点の拘束など（ボディ同士ではない）
        ed::JointDesc j;
        j.name = ed::sanitizeEventName(nameOf(*link));
        auto bodyIndex = [&](const ChBodyFrame* bf) {
            const auto it = indexOf.find(bf);
            return it == indexOf.end() ? -1 : it->second;
        };
        j.bodyA = bodyIndex(lk->GetBody1());
        j.bodyB = bodyIndex(lk->GetBody2());
        if (j.bodyA < 0 && j.bodyB < 0) continue;
        ChFrame<> frame;
        const bool haveFrame = linkFrame(lk.get(), frame, 0);
        if (haveFrame) {
            j.anchor = toVec(frame.GetPos());
            j.axis = toVec(frame.GetRot().GetZaxis());  // 回転 / 直動は Z 軸
        } else {
            const ChBodyFrame* bf = j.bodyB >= 0 ? lk->GetBody2() : lk->GetBody1();
            j.anchor = toVec(bf->GetPos());
            j.axis = ed::Vec3d{0.0, 1.0, 0.0};
            warnings.push_back("joint '" + j.name + "': this Chrono has no link frame "
                               "accessor - anchor set to the body centre");
        }
        bool known = true;
        double lo = 0.0, hi = 0.0;
        if (auto m = std::dynamic_pointer_cast<ChLinkMotorRotation>(lk)) {
            j.kind = ed::JointKind::Revolute;
            const double v = m->GetMotorFunction() ? functionValue(m->GetMotorFunction().get(), 0.0, 0) : 0.0;
            if (std::dynamic_pointer_cast<ChLinkMotorRotationSpeed>(lk)) {
                j.motor = ed::MotorMode::Speed;
                j.motorTarget = v * toDeg;
            } else if (std::dynamic_pointer_cast<ChLinkMotorRotationAngle>(lk)) {
                j.motor = ed::MotorMode::Position;
                j.motorTarget = v * toDeg;
            } else {
                j.motor = ed::MotorMode::Force;
                j.motorTarget = v;
            }
        } else if (auto m = std::dynamic_pointer_cast<ChLinkMotorLinear>(lk)) {
            j.kind = ed::JointKind::Prismatic;
            if (haveFrame) j.axis = toVec(frame.GetRot().GetXaxis());  // 直動モータは X
            const double v = m->GetMotorFunction() ? functionValue(m->GetMotorFunction().get(), 0.0, 0) : 0.0;
            if (std::dynamic_pointer_cast<ChLinkMotorLinearSpeed>(lk)) {
                j.motor = ed::MotorMode::Speed;
            } else if (std::dynamic_pointer_cast<ChLinkMotorLinearPosition>(lk)) {
                j.motor = ed::MotorMode::Position;
            } else {
                j.motor = ed::MotorMode::Force;
            }
            j.motorTarget = v;
        } else if (auto r = std::dynamic_pointer_cast<ChLinkLockRevolute>(lk)) {
            j.kind = ed::JointKind::Revolute;
            if (limitRz(r.get(), lo, hi, 0)) {
                j.limited = true;
                j.limitLo = lo * toDeg;
                j.limitHi = hi * toDeg;
            }
        } else if (auto p = std::dynamic_pointer_cast<ChLinkLockPrismatic>(lk)) {
            j.kind = ed::JointKind::Prismatic;
            if (limitZ(p.get(), lo, hi, 0)) {
                j.limited = true;
                j.limitLo = lo;
                j.limitHi = hi;
            }
        } else if (std::dynamic_pointer_cast<ChLinkLockSpherical>(lk)) {
            j.kind = ed::JointKind::Spherical;
        } else if (std::dynamic_pointer_cast<ChLinkLockCylindrical>(lk)) {
            j.kind = ed::JointKind::Cylindrical;
        } else if (std::dynamic_pointer_cast<ChLinkLockPlanar>(lk)) {
            j.kind = ed::JointKind::Planar;
        } else if (std::dynamic_pointer_cast<ChLinkLockLock>(lk)) {
            j.kind = ed::JointKind::Fixed;
        } else if (std::dynamic_pointer_cast<ChLinkUniversal>(lk)) {
            j.kind = ed::JointKind::Universal;
        } else if (auto d = std::dynamic_pointer_cast<ChLinkDistance>(lk)) {
            j.kind = ed::JointKind::Distance;
            j.distance = d->GetImposedDistance();
        } else if (auto s = std::dynamic_pointer_cast<ChLinkTSDA>(lk)) {
            j.kind = ed::JointKind::Spring;
            j.stiffness = s->GetSpringCoefficient();
            j.damping = s->GetDampingCoefficient();
            j.distance = s->GetRestLength();
            j.anchor = toVec(s->GetPoint1Abs());
        } else {
            known = false;
        }
        if (!known) {
            warnings.push_back("joint '" + j.name + "' has an unsupported link type - dropped");
            continue;
        }
        out.joints.push_back(ed::clampJoint(j));
    }
}

std::string lowerExt(const std::string& file) {
    const std::size_t dot = file.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = file.substr(dot + 1);
    for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

}  // namespace
#endif  // WIZ_HAVE_PARSERS

bool importModel(const std::string& file, editor::SceneDocument& out, std::string& error,
                 std::vector<std::string>& warnings) {
    out = editor::SceneDocument{};
    warnings.clear();
#ifdef WIZ_HAVE_PARSERS
    if (!editor::assetFileAllowed(file)) {
        error = "assets-relative paths only";
        return false;
    }
    const std::string path = assetPath(file);
    const std::string ext = lowerExt(file);
    std::string stem = file;
    {
        const std::size_t slash = stem.find_last_of("/\\");
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        const std::size_t dot = stem.rfind('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        stem = editor::sanitizeEventName(stem);
        if (stem.empty()) stem = "import";
    }
    try {
        // 一時的な系（何も解かない。パーサの置き場）。
        ChSystemNSC sys;
        if (ext == "urdf") {
            parsers::ChParserURDF parser(path);
            // URDF は Z-up が普通なので Y-up へ（X まわり -90°）。
            parser.SetRootInitPose(ChFrame<>(ChVector3d(0, 0, 0), QuatFromAngleX(-CH_PI_2)));
            parser.PopulateSystem(sys);
        } else if (ext == "osim") {
#ifdef WIZ_HAVE_PARSER_OPENSIM
            parsers::ChParserOpenSim parser;
            parser.Parse(sys, path);
#else
            error = "this Chrono::Parsers has no OpenSim parser";
            return false;
#endif
        } else if (ext == "adm") {
#ifdef WIZ_HAVE_PARSER_ADAMS
            parsers::ChParserAdams parser;
            parser.Parse(sys, path);
#else
            error = "this Chrono::Parsers has no ADAMS parser";
            return false;
#endif
        } else {
            error = "unknown model format '" + ext + "' (urdf / osim / adm)";
            return false;
        }
        systemToDocument(sys, stem, out, warnings);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (out.bodies.empty()) {
        error = "no bodies were read from '" + file + "'";
        return false;
    }
    out.model = stem;
    return true;
#else
    (void)file;
    error = "Chrono::Parsers is not built in (configure with -DWIZ_WITH_CHRONO_PARSERS=ON)";
    return false;
#endif
}

}  // namespace wizengine
