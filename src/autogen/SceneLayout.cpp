#include "autogen/SceneLayout.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace wizengine {
namespace autogen {

namespace ed = wizengine::editor;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kGap = 0.01;        // 載せ直すときの隙間 (m)
constexpr double kTolerance = 0.005; // これより浅い重なりは「触れている」扱い

struct Box3 {
    bool valid = false;
    double lo[3] = {0, 0, 0};
    double hi[3] = {0, 0, 0};
};

// 局所の半寸法 h を姿勢 R = Rz*Ry*Rx で回したときの外接箱の半寸法
// （|R| h）。SceneMath と同じ回転の順序。
void rotatedHalf(const ed::Vec3d& eulerDeg, const double h[3], double out[3]) {
    const double x = eulerDeg.x * kPi / 180.0, y = eulerDeg.y * kPi / 180.0,
                 z = eulerDeg.z * kPi / 180.0;
    const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y),
                 cz = std::cos(z), sz = std::sin(z);
    const double R[3][3] = {
        {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
        {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
        {-sy, cy * sx, cy * cx},
    };
    for (int r = 0; r < 3; ++r) {
        out[r] = std::fabs(R[r][0]) * h[0] + std::fabs(R[r][1]) * h[1] +
                 std::fabs(R[r][2]) * h[2];
    }
}

// ボディの外接箱。形の分からない物（部品だけのフレーム）は valid = false
// = 重なりの判定に入れない。
Box3 bodyBox(const ed::BodyDesc& b, const MeshExtents& meshes) {
    Box3 box;
    double h[3];
    switch (b.shape) {
        case ed::ShapeKind::Box:
            h[0] = b.size.x * 0.5;
            h[1] = b.size.y * 0.5;
            h[2] = b.size.z * 0.5;
            break;
        case ed::ShapeKind::Sphere:
            h[0] = h[1] = h[2] = b.size.x * 0.5;
            break;
        case ed::ShapeKind::Model: {
            const auto it = meshes.find(b.mesh);
            if (it != meshes.end()) {
                for (int a = 0; a < 3; ++a) h[a] = it->second.half[a];
            } else {
                h[0] = h[1] = h[2] = b.size.x * 0.5;  // 当たりは球に落ちる
            }
            break;
        }
        default:
            return box;
    }
    double w[3];
    rotatedHalf(b.rotation, h, w);
    const double c[3] = {b.position.x, b.position.y, b.position.z};
    for (int a = 0; a < 3; ++a) {
        box.lo[a] = c[a] - w[a];
        box.hi[a] = c[a] + w[a];
    }
    box.valid = true;
    return box;
}

bool overlaps(const Box3& a, const Box3& b) {
    if (!a.valid || !b.valid) return false;
    for (int i = 0; i < 3; ++i) {
        if (a.hi[i] - kTolerance <= b.lo[i] || b.hi[i] - kTolerance <= a.lo[i]) return false;
    }
    return true;
}

}  // namespace

LayoutReport resolveOverlaps(ed::SceneDocument& doc, const std::vector<bool>& movable,
                             const std::vector<ed::BodyDesc>& obstacles,
                             const MeshExtents& meshes) {
    LayoutReport rep;
    // ジョイントに関わる物は動かさない（つなぎ方の幾何を保つ）。
    std::set<int> jointed;
    for (const auto& j : doc.joints) {
        if (j.bodyA >= 0) jointed.insert(j.bodyA);
        if (j.bodyB >= 0) jointed.insert(j.bodyB);
    }
    for (const auto& c : doc.cables) {
        if (c.bodyA >= 0) jointed.insert(c.bodyA);
        if (c.bodyB >= 0) jointed.insert(c.bodyB);
    }
    const bool hasFloor = doc.ground.enabled && !doc.hasTerrain &&
                          std::fabs(doc.ground.rotation.x) < 1e-6 &&
                          std::fabs(doc.ground.rotation.z) < 1e-6;
    const double floorY = doc.ground.position.y;

    // 置き終わった物（載せる先の候補）。動かさない物を先に全部入れる。
    std::vector<Box3> placed;
    for (const auto& o : obstacles) placed.push_back(bodyBox(o, meshes));
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < doc.bodies.size(); ++i) {
        const bool canMove = i < movable.size() && movable[i] && !doc.bodies[i].fixed &&
                             !jointed.count(int(i));
        if (canMove) {
            order.push_back(i);
        } else {
            placed.push_back(bodyBox(doc.bodies[i], meshes));
        }
    }
    // 低い物から置く（上に載る物が、下の物より先に持ち上がって宙に浮かない）。
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return bodyBox(doc.bodies[a], meshes).lo[1] < bodyBox(doc.bodies[b], meshes).lo[1];
    });

    for (const std::size_t i : order) {
        ed::BodyDesc& b = doc.bodies[i];
        Box3 box = bodyBox(b, meshes);
        if (!box.valid) continue;
        const double halfY = (box.hi[1] - box.lo[1]) * 0.5;
        if (hasFloor && box.lo[1] < floorY - kTolerance) {
            b.position.y = floorY + halfY + 0.001;
            box = bodyBox(b, meshes);
            ++rep.raisedToFloor;
            rep.notes.push_back("'" + b.name + "' was below the floor - raised onto it");
        }
        // 重なりが消えるまで、重なっている物の上面へ載せ直す（上限つき）。
        bool moved = false;
        for (int iter = 0; iter < 16; ++iter) {
            double top = -1e30;
            for (const auto& p : placed) {
                if (overlaps(box, p)) top = std::max(top, p.hi[1]);
            }
            if (top < -1e29) break;
            b.position.y = top + halfY + kGap;
            box = bodyBox(b, meshes);
            moved = true;
        }
        if (moved) {
            ++rep.lifted;
            rep.notes.push_back("'" + b.name + "' intersected another body - lifted to y=" +
                                std::to_string(b.position.y));
        }
        placed.push_back(box);
    }
    return rep;
}

}  // namespace autogen
}  // namespace wizengine
