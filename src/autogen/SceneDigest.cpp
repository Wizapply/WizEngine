#include "autogen/SceneDigest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace wizengine {
namespace autogen {

namespace ed = editor;

namespace {

constexpr double kPi = 3.14159265358979323846;
// 「載っている」とみなす隙間（底と上面の高さの差）。エディタで手で置いた物は
// 数 mm ずれるので少し甘くする。
constexpr double kRestGap = 0.03;
// 「めり込んでいる」とみなす重なり（3 軸とも）。
constexpr double kOverlap = 0.02;

std::string fmt(double v, int digits = 2) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, std::fabs(v) < 0.5e-2 ? 0.0 : v);
    return buf;
}
std::string fmtVec(const Vec3d& v) {
    return "(" + fmt(v.x) + ", " + fmt(v.y) + ", " + fmt(v.z) + ")";
}

// 文書の type 名（SceneDocument の jointTypeName と同じ = LLM が XML で見る名前）。
const char* jointDocName(ed::JointKind k) {
    switch (k) {
        case ed::JointKind::Fixed: return "weld";
        case ed::JointKind::Spherical: return "ball";
        case ed::JointKind::Prismatic: return "slide";
        case ed::JointKind::Distance: return "distance";
        case ed::JointKind::Universal: return "universal";
        case ed::JointKind::Cylindrical: return "cylindrical";
        case ed::JointKind::Planar: return "planar";
        case ed::JointKind::PointLine: return "pointline";
        case ed::JointKind::PointPlane: return "pointplane";
        case ed::JointKind::Gear: return "gear";
        case ed::JointKind::Screw: return "screw";
        case ed::JointKind::Spring: return "spring";
        case ed::JointKind::Bushing: return "bushing";
        case ed::JointKind::Revolute: break;
    }
    return "hinge";
}

// オイラー角（度）→ 回転行列。SceneMath と同じ R = Rz * Ry * Rx。
void rotationMatrix(const Vec3d& deg, double m[3][3]) {
    const double x = deg.x * kPi / 180.0, y = deg.y * kPi / 180.0, z = deg.z * kPi / 180.0;
    const double cx = std::cos(x), sx = std::sin(x);
    const double cy = std::cos(y), sy = std::sin(y);
    const double cz = std::cos(z), sz = std::sin(z);
    m[0][0] = cz * cy;
    m[0][1] = cz * sy * sx - sz * cx;
    m[0][2] = cz * sy * cx + sz * sx;
    m[1][0] = sz * cy;
    m[1][1] = sz * sy * sx + cz * cx;
    m[1][2] = sz * sy * cx - cz * sx;
    m[2][0] = -sy;
    m[2][1] = cy * sx;
    m[2][2] = cy * cx;
}

// 物の外接箱（ワールド、設計値の姿勢）。形の無い物（プレハブだけの物）は
// 大きさが分からないので点として扱う（known = false）。
struct Box {
    Vec3d lo, hi;
    bool known = true;
};

Box bodyBox(const ed::BodyDesc& b) {
    Vec3d half{0.0, 0.0, 0.0};
    bool known = true;
    switch (b.shape) {
        case ed::ShapeKind::Box:
        case ed::ShapeKind::Trimesh: {
            // 回転した箱の外接箱: 各軸の半寸法 = Σ |R_ij| h_j。
            double m[3][3];
            rotationMatrix(b.rotation, m);
            const double h[3] = {b.size.x * 0.5, b.size.y * 0.5, b.size.z * 0.5};
            double e[3];
            for (int i = 0; i < 3; ++i) {
                e[i] = std::fabs(m[i][0]) * h[0] + std::fabs(m[i][1]) * h[1] +
                       std::fabs(m[i][2]) * h[2];
            }
            half = {e[0], e[1], e[2]};
            break;
        }
        case ed::ShapeKind::Sphere:
        case ed::ShapeKind::Model: {
            const double r = b.size.x * 0.5;  // Sphere / Model は size.x が直径
            half = {r, r, r};
            break;
        }
        case ed::ShapeKind::None:
            known = false;
            break;
    }
    Box box;
    box.lo = {b.position.x - half.x, b.position.y - half.y, b.position.z - half.z};
    box.hi = {b.position.x + half.x, b.position.y + half.y, b.position.z + half.z};
    box.known = known;
    return box;
}

bool overlapXZ(const Box& a, const Box& b, double margin) {
    return a.lo.x < b.hi.x - margin && b.lo.x < a.hi.x - margin &&
           a.lo.z < b.hi.z - margin && b.lo.z < a.hi.z - margin;
}
bool penetrates(const Box& a, const Box& b) {
    return a.lo.x < b.hi.x - kOverlap && b.lo.x < a.hi.x - kOverlap &&
           a.lo.y < b.hi.y - kOverlap && b.lo.y < a.hi.y - kOverlap &&
           a.lo.z < b.hi.z - kOverlap && b.lo.z < a.hi.z - kOverlap;
}

std::string shapeText(const ed::BodyDesc& b) {
    switch (b.shape) {
        case ed::ShapeKind::Box:
            return "box " + fmt(b.size.x) + " x " + fmt(b.size.y) + " x " + fmt(b.size.z) + " m";
        case ed::ShapeKind::Sphere:
            return "sphere, diameter " + fmt(b.size.x) + " m";
        case ed::ShapeKind::Model:
            return "model \"" + b.mesh + "\", about " + fmt(b.size.x) + " m across";
        case ed::ShapeKind::Trimesh:
            return "mesh collider " + fmt(b.size.x) + " x " + fmt(b.size.y) + " x " +
                   fmt(b.size.z) + " m";
        case ed::ShapeKind::None:
            return b.prefab.empty() ? std::string("frame (no geom)")
                                    : "frame drawn by prefab \"" + b.prefab + "\" (size unknown)";
    }
    return "body";
}

std::string bodyLabel(const ed::BodyDesc& b, std::size_t index) {
    return b.name.empty() ? "body #" + std::to_string(index) : "\"" + b.name + "\"";
}

// エディタカメラ（オービット）。CameraObject と同じ式で目の位置を出す。
struct ViewFrame {
    bool valid = false;
    Vec3d eye, fwd, right, up;  // fwd = 視線、right / up = 画面の右 / 上
    double tanHalfV = std::tan(22.5 * kPi / 180.0);  // 縦の画角 45°
    double aspect = 16.0 / 9.0;
};

Vec3d sub(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3d cross(const Vec3d& a, const Vec3d& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3d normalized(const Vec3d& v) {
    const double l = std::sqrt(dot(v, v));
    return l > 1e-12 ? Vec3d{v.x / l, v.y / l, v.z / l} : Vec3d{0.0, 0.0, 0.0};
}

ViewFrame viewFrame(const ed::CameraPose& c) {
    ViewFrame v;
    const double ce = std::cos(c.elevation);
    const Vec3d off{c.radius * ce * std::sin(c.azimuth), c.radius * std::sin(c.elevation),
                    c.radius * ce * std::cos(c.azimuth)};
    v.eye = {c.target.x + off.x, c.target.y + off.y, c.target.z + off.z};
    v.fwd = normalized(sub(c.target, v.eye));
    v.right = normalized(cross(v.fwd, {0.0, 1.0, 0.0}));
    v.up = cross(v.right, v.fwd);
    v.valid = dot(v.fwd, v.fwd) > 0.5 && dot(v.right, v.right) > 0.5;
    return v;
}

// 画面の上での位置（ユーザーの言う「左の」「手前の」に合わせるため）。
std::string viewText(const ViewFrame& v, const Vec3d& p) {
    const Vec3d d = sub(p, v.eye);
    const double depth = dot(d, v.fwd);
    const double dist = std::sqrt(dot(d, d));
    if (depth <= 0.05) return "behind the editor camera (not visible)";
    const double sx = dot(d, v.right) / (depth * v.tanHalfV * v.aspect);  // -1..1 が画面内
    const double sy = dot(d, v.up) / (depth * v.tanHalfV);
    if (std::fabs(sx) > 1.0 || std::fabs(sy) > 1.0) {
        std::string where;
        if (sy > 1.0) where = "above";
        else if (sy < -1.0) where = "below";
        if (std::fabs(sx) > 1.0) {
            if (!where.empty()) where += " and ";
            where += sx < 0 ? "left of" : "right of";
        }
        return "outside the editor view (" + where + " the frame), " + fmt(dist, 1) +
               " m from the camera";
    }
    const char* h = sx < -0.33 ? "left" : (sx > 0.33 ? "right" : "centre");
    const char* vtx = sy > 0.33 ? "upper " : (sy < -0.33 ? "lower " : "");
    return std::string("in view: ") + vtx + h + ", " + fmt(dist, 1) + " m from the camera";
}

// ワールドの方向を、エディタカメラから見た言葉に（「画面の右 = +X」など）。
std::string axisInView(const ViewFrame& v) {
    auto name = [](const Vec3d& d) {
        // 水平の成分だけを見て、いちばん近い軸の名前。
        const double ax = std::fabs(d.x), az = std::fabs(d.z);
        if (ax >= az) return std::string(d.x >= 0 ? "+X" : "-X");
        return std::string(d.z >= 0 ? "+Z" : "-Z");
    };
    const Vec3d away{v.fwd.x, 0.0, v.fwd.z};
    return "screen right ~ " + name(v.right) + ", away from the camera (into the screen) ~ " +
           name(away) + ", toward the camera ~ " + name(Vec3d{-away.x, 0.0, -away.z});
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// 名指しの語の終わり（空白・ASCII の句読点・全角の句読点と空白）。
bool isBreakAt(const std::string& s, std::size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
                          c == ':';
        return !word;
    }
    // 全角の空白・句読点・括弧（E3 80 80〜E3 80 91 = 　、。〃…「」『』【】）と
    // 全角の ！（ ） ， ． ？（EF BC 81 / 88 / 89 / 8C / 8E / 9F）。
    if (c == 0xE3 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80) {
        const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        return c2 <= 0x91;
    }
    if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBC) {
        const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        return c2 == 0x81 || c2 == 0x88 || c2 == 0x89 || c2 == 0x8C || c2 == 0x8E ||
               c2 == 0x9F;
    }
    return false;
}

// 英数字と _（名前の続きと見なす文字。"." "-" は文の句読点・つなぎにも使うので含めない）。
bool isAsciiIdentChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

bool isAsciiWordChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.';
}

}  // namespace

std::string colorName(float r, float g, float b) {
    // リニア → 表示の値（おおよそ sRGB）にしてから HSV で名前を付ける。
    auto toDisplay = [](float v) {
        return std::pow(std::clamp(double(v), 0.0, 1.0), 1.0 / 2.2);
    };
    const double R = toDisplay(r), G = toDisplay(g), B = toDisplay(b);
    const double mx = std::max({R, G, B}), mn = std::min({R, G, B});
    const double v = mx, s = mx > 1e-6 ? (mx - mn) / mx : 0.0;
    if (s < 0.18) {
        if (v > 0.9) return "white";
        if (v > 0.7) return "light gray";
        if (v > 0.45) return "gray";
        if (v > 0.2) return "dark gray";
        return "black";
    }
    double h = 0.0;
    const double d = mx - mn;
    if (mx == R) h = std::fmod((G - B) / d, 6.0);
    else if (mx == G) h = (B - R) / d + 2.0;
    else h = (R - G) / d + 4.0;
    h *= 60.0;
    if (h < 0) h += 360.0;
    std::string name;
    if (h < 15 || h >= 345) name = "red";
    else if (h < 40) name = (v < 0.65) ? "brown" : "orange";
    else if (h < 70) name = (v < 0.55) ? "olive" : "yellow";
    else if (h < 160) name = "green";
    else if (h < 200) name = "cyan";
    else if (h < 255) name = "blue";
    else if (h < 290) name = "purple";
    else name = "pink";
    if (name != "brown" && name != "olive") {
        if (v < 0.45) name = "dark " + name;
        else if (s < 0.4 && v > 0.8) name = "light " + name;
    }
    return name;
}

int nameUnnamedBodies(ed::SceneDocument& doc) {
    std::set<std::string> taken;
    for (const auto& b : doc.bodies) {
        if (!b.name.empty()) taken.insert(b.name);
    }
    int added = 0;
    for (std::size_t i = 0; i < doc.bodies.size(); ++i) {
        auto& b = doc.bodies[i];
        if (!b.name.empty()) continue;
        std::string base = "body" + std::to_string(i);
        std::string name = base;
        for (int n = 2; taken.count(name); ++n) name = base + "_" + std::to_string(n);
        taken.insert(name);
        b.name = name;
        ++added;
    }
    return added;
}

std::vector<std::string> mentionableNames(const ed::SceneDocument& doc) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& n) {
        if (!n.empty() && seen.insert(n).second) out.push_back(n);
    };
    for (const auto& b : doc.bodies) add(b.name);
    for (const auto& j : doc.joints) add(j.name);
    for (const auto& l : doc.lights) add(l.name);
    for (const auto& c : doc.cables) add(c.name);
    for (std::size_t i = 0; i < doc.cameras.size(); ++i) add("cam" + std::to_string(i));
    return out;
}

Mentions resolveMentions(const std::string& prompt, const std::vector<std::string>& names) {
    Mentions m;
    for (std::size_t i = 0; i < prompt.size(); ++i) {
        if (prompt[i] != '@') continue;
        // メールアドレス（user@host）のように英数字の直後の @ は名指しではない。
        if (i > 0 && isAsciiWordChar(static_cast<unsigned char>(prompt[i - 1]))) continue;
        const std::size_t start = i + 1;
        // 最も長く一致する名前。名前が ASCII の語で終わるなら、直後も語の続き
        // （@box10 に対する名前 box1）は一致と見なさない。
        std::string best;
        for (const auto& n : names) {
            if (n.size() <= best.size() || prompt.compare(start, n.size(), n) != 0) continue;
            const std::size_t end = start + n.size();
            const unsigned char last = static_cast<unsigned char>(n.back());
            if (end < prompt.size() && isAsciiIdentChar(last) &&
                isAsciiIdentChar(static_cast<unsigned char>(prompt[end]))) {
                continue;
            }
            best = n;
        }
        if (!best.empty()) {
            if (!contains(m.found, best)) m.found.push_back(best);
            i = start + best.size() - 1;
            continue;
        }
        // 一致しない: 区切りまでを「名前のつもりの語」として報告する。
        std::size_t end = start;
        while (end < prompt.size() && !isBreakAt(prompt, end)) ++end;
        // 区切りの無い日本語が続くと文の残りを丸ごと拾うので、長さを抑える。
        std::string token = prompt.substr(start, end - start);
        while (!token.empty() && (token.back() == '.' || token.back() == ':' ||
                                  token.back() == '-')) {
            token.pop_back();  // 文末の句読点
        }
        if (token.size() > 40) {
            std::size_t cut = 40;
            while (cut > 0 && (static_cast<unsigned char>(token[cut]) & 0xC0) == 0x80) --cut;
            token = token.substr(0, cut) + "...";
        }
        if (!token.empty() && !contains(m.unknown, token)) m.unknown.push_back(token);
        if (end > start) i = end - 1;
    }
    return m;
}

std::string describeScene(const ed::SceneDocument& doc, const DigestOptions& opt) {
    std::string s =
        "SCENE SUMMARY (computed by the engine from the document; positions are the authored "
        "editor poses, metres, +Y up):\n";

    // ---- エディタカメラ ------------------------------------------------------
    ViewFrame view;
    if (!doc.cameras.empty()) {
        const std::size_t ci =
            (opt.editorCamera >= 0 && std::size_t(opt.editorCamera) < doc.cameras.size())
                ? std::size_t(opt.editorCamera)
                : 0;
        const ed::CameraPose& cam = doc.cameras[ci];
        view = viewFrame(cam);
        if (view.valid) {
            s += "- Editor camera (what the user sees): cam" + std::to_string(ci) + " at " +
                 fmtVec(view.eye) + " looking at " + fmtVec(cam.target) + ". " +
                 axisInView(view) +
                 ". When the user says left / right / front / back / near / far they "
                 "usually mean as seen in this view.\n";
        }
    }

    // ---- 地面 ---------------------------------------------------------------
    const bool ground = doc.ground.enabled;
    const double groundY = doc.ground.position.y;
    if (ground) {
        s += "- Ground: floor surface at y = " + fmt(groundY) + ", " + fmt(doc.ground.half * 2.0) +
             " x " + fmt(doc.ground.half * 2.0) + " m (things beyond it fall forever).\n";
    } else {
        s += "- Ground: none (removed) - non-fixed bodies fall forever.\n";
    }

    // ---- 物 -----------------------------------------------------------------
    const std::size_t n = doc.bodies.size();
    std::vector<Box> boxes(n);
    for (std::size_t i = 0; i < n; ++i) boxes[i] = bodyBox(doc.bodies[i]);
    // ジョイント・ケーブルで留められている物（宙に浮いていても落ちない / 落ち方が違う）。
    std::vector<bool> jointed(n, false);
    for (const auto& j : doc.joints) {
        if (j.bodyA >= 0 && std::size_t(j.bodyA) < n) jointed[std::size_t(j.bodyA)] = true;
        if (j.bodyB >= 0 && std::size_t(j.bodyB) < n) jointed[std::size_t(j.bodyB)] = true;
    }
    for (const auto& c : doc.cables) {
        if (c.bodyA >= 0 && std::size_t(c.bodyA) < n) jointed[std::size_t(c.bodyA)] = true;
        if (c.bodyB >= 0 && std::size_t(c.bodyB) < n) jointed[std::size_t(c.bodyB)] = true;
    }

    if (n == 0) {
        s += "- Bodies: none.\n";
    } else {
        s += "- Bodies (" + std::to_string(n) + "):\n";
    }
    const std::size_t listed = std::min(n, opt.maxBodies);
    for (std::size_t i = 0; i < listed; ++i) {
        const ed::BodyDesc& b = doc.bodies[i];
        const Box& box = boxes[i];
        std::string line = "  * " + bodyLabel(b, i) + ": " + shapeText(b);
        if (b.shape != ed::ShapeKind::None) {
            line += ", " + colorName(b.color.r, b.color.g, b.color.b);
        }
        line += ", centre " + fmtVec(b.position);
        if (box.known) line += ", bottom y=" + fmt(box.lo.y) + ", top y=" + fmt(box.hi.y);
        if (std::fabs(b.rotation.x) + std::fabs(b.rotation.y) + std::fabs(b.rotation.z) > 0.5) {
            line += ", rotated " + fmtVec(b.rotation) + " deg";
        }
        line += b.fixed ? ", FIXED" : ", " + fmt(b.mass, 2) + " kg";
        if (b.hasVehicle) line += ", vehicle";
        if (b.hasSoft) line += ", soft body";
        if (!b.prefab.empty() && b.shape != ed::ShapeKind::None) {
            line += ", decorated by prefab \"" + b.prefab + "\"";
        }
        if (contains(opt.focus, b.name)) line += "  <== SELECTED BY THE USER";
        else if (contains(opt.mentioned, b.name)) line += "  <== MENTIONED BY THE USER";

        // 位置関係（形が分かる物だけ）。
        std::vector<std::string> rel;
        if (box.known) {
            std::vector<std::string> on, into;
            bool supported = false;
            if (ground && std::fabs(box.lo.y - groundY) <= kRestGap) {
                on.push_back("the ground");
                supported = true;
            }
            for (std::size_t k = 0; k < n; ++k) {
                if (k == i || !boxes[k].known) continue;
                const Box& o = boxes[k];
                if (std::fabs(box.lo.y - o.hi.y) <= kRestGap && overlapXZ(box, o, 0.0)) {
                    on.push_back(bodyLabel(doc.bodies[k], k));
                    supported = true;
                } else if (penetrates(box, o)) {
                    into.push_back(bodyLabel(doc.bodies[k], k));
                    supported = true;  // めり込んでいる物は支えにもなっている（押し出される）
                }
            }
            if (ground && box.lo.y < groundY - kRestGap) {
                rel.push_back("SUNK " + fmt(groundY - box.lo.y) + " m into the ground");
                supported = true;
            }
            if (!on.empty()) {
                std::string t = "rests on ";
                for (std::size_t k = 0; k < on.size(); ++k) t += (k ? ", " : "") + on[k];
                rel.push_back(t);
            }
            if (!into.empty()) {
                std::string t = "INTERSECTS ";
                for (std::size_t k = 0; k < into.size(); ++k) t += (k ? ", " : "") + into[k];
                rel.push_back(t);
            }
            if (!supported && !b.fixed && !jointed[i]) {
                const double h = ground ? box.lo.y - groundY : box.lo.y;
                rel.push_back("FLOATING " + fmt(h) + " m above " +
                              (ground ? std::string("the ground") : std::string("y=0")) +
                              " with nothing under it (will fall when the simulation starts)");
            }
        }
        if (jointed[i]) rel.push_back("held by a joint or cable");
        if (view.valid) rel.push_back(viewText(view, b.position));
        if (!rel.empty()) {
            line += ". ";
            for (std::size_t k = 0; k < rel.size(); ++k) line += (k ? "; " : "") + rel[k];
        }
        s += line + ".\n";
    }
    if (listed < n) {
        s += "  * ... and " + std::to_string(n - listed) +
             " more bodies (see the document; not listed here).\n";
    }

    // ---- ジョイント ----------------------------------------------------------
    auto endName = [&](int ref) {
        if (ref < 0 || std::size_t(ref) >= n) return std::string("world");
        return bodyLabel(doc.bodies[std::size_t(ref)], std::size_t(ref));
    };
    if (!doc.joints.empty()) {
        s += "- Joints (" + std::to_string(doc.joints.size()) + "):\n";
        for (std::size_t i = 0; i < doc.joints.size() && i < 80; ++i) {
            const auto& j = doc.joints[i];
            std::string line = "  * " + (j.name.empty() ? "joint #" + std::to_string(i)
                                                         : "\"" + j.name + "\"") +
                               ": " + jointDocName(j.kind) + " between " + endName(j.bodyA) +
                               " and " + endName(j.bodyB) + " at " + fmtVec(j.anchor);
            if (contains(opt.mentioned, j.name)) line += "  <== MENTIONED BY THE USER";
            s += line + "\n";
        }
        if (doc.joints.size() > 80) {
            s += "  * ... and " + std::to_string(doc.joints.size() - 80) + " more joints.\n";
        }
    }

    // ---- ライト / カメラ ------------------------------------------------------
    if (!doc.lights.empty()) {
        s += "- Lights:";
        for (std::size_t i = 0; i < doc.lights.size(); ++i) {
            const auto& l = doc.lights[i];
            s += std::string(i ? ";" : "") + " " +
                 (l.name.empty() ? "light #" + std::to_string(i) : "\"" + l.name + "\"") + " (" +
                 ed::lightKindName(l.kind) + ") at " + fmtVec(l.position);
            if (contains(opt.mentioned, l.name)) s += " <== MENTIONED";
        }
        s += "\n";
    }
    if (doc.cameras.size() > 1) {
        s += "- Cameras: cam0..cam" + std::to_string(doc.cameras.size() - 1) +
             " (cam" + std::to_string(opt.editorCamera) + " is the editor camera).\n";
    }

    // ---- ユーザーの指し示し ----------------------------------------------------
    if (!opt.focus.empty()) {
        s += "\nUSER SELECTION: the user has selected ";
        for (std::size_t i = 0; i < opt.focus.size(); ++i) {
            s += (i ? ", " : "") + std::string("\"") + opt.focus[i] + "\"";
        }
        s += " in the editor. Words like \"this\", \"it\", \"the selected one\", "
             "\"これ\", \"それ\", \"この\", \"選択した物\" refer to it unless the request names "
             "something else.\n";
    }
    if (!opt.mentioned.empty()) {
        s += "\nMENTIONS: in the request, @name refers to the element with exactly that name "
             "(";
        for (std::size_t i = 0; i < opt.mentioned.size(); ++i) {
            s += (i ? ", " : "") + std::string("@") + opt.mentioned[i];
        }
        s += "). Do not rename these elements.\n";
    }
    return s;
}

}  // namespace autogen
}  // namespace wizengine
