#include "SceneDocument.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace wizengine {
namespace editor {
namespace {

constexpr double kPi = 3.14159265358979323846;
double toDegrees(double radians) { return radians * 180.0 / kPi; }
double toRadians(double degrees) { return degrees * kPi / 180.0; }

// ---- 警告の集積 -------------------------------------------------------------
// 「読めるけれど意図と違うかもしれない」内容を文で集める。読み取りを止めない
// （既定値へ落として続行する）ので、代わりにここへ痕跡を残す。out が無ければ
// 何もしない。壊れた文書ほど警告が湧くので、件数に上限を置く。
class Warn {
public:
    explicit Warn(std::vector<std::string>* out) : out_(out) {}
    void operator()(std::string msg) {
        if (!out_) return;
        if (out_->size() >= kMax) {
            if (!overflowNoted_) {
                out_->push_back("(further warnings omitted)");
                overflowNoted_ = true;
            }
            return;
        }
        out_->push_back(std::move(msg));
    }

private:
    static constexpr std::size_t kMax = 40;
    std::vector<std::string>* out_;
    bool overflowNoted_ = false;
};

// 名前が語彙に無いか。FromName 系は未知の名前で fallback をそのまま返すので、
// 別々の fallback で 2 回呼んで結果が食い違えば「知らない名前」と分かる
// （語彙の文字列一覧をここへ複製して同期漏れを作らないための判定）。
template <typename T, typename F>
bool unknownName(const F& fromName, const std::string& s, T a, T b) {
    return fromName(s, a) != fromName(s, b);
}

// 親の子要素のうち「知らない名前」を警告する。節を増やしたら known へ足す
// （SceneDocument.h の「拡張の手順」）。
void warnUnknownChildren(const xml::Element& parent,
                         std::initializer_list<const char*> known,
                         const char* where, Warn& warn) {
    for (const auto& c : parent.children()) {
        bool ok = false;
        for (const char* k : known) {
            if (c.name() == k) { ok = true; break; }
        }
        if (!ok) {
            warn(std::string("unsupported <") + c.name() + "> in " + where +
                 " - ignored");
        }
    }
}

// ---- 属性の読み書き（Vec3 と色）--------------------------------------------
void setVec3(xml::Element& e, const char* key, const Vec3d& v) {
    const double a[3] = {v.x, v.y, v.z};
    e.setNumbers(key, a, 3);
}
Vec3d getVec3(const xml::Element& e, const char* key, const Vec3d& fallback) {
    double a[3] = {fallback.x, fallback.y, fallback.z};
    e.numbers(key, a, 3);
    return Vec3d{a[0], a[1], a[2]};
}
// 色は MuJoCo と同じ rgba="r g b a"（リニア値）。a は今は使わないが、
// MJCF を見慣れた目に自然な形にしておく。
void setColor(xml::Element& e, const char* key, const Color3& c) {
    const double a[4] = {c.r, c.g, c.b, 1.0};
    e.setNumbers(key, a, 4, 6);  // float 由来なので 6 桁で十分
}
Color3 getColor(const xml::Element& e, const char* key, const Color3& fallback) {
    double a[4] = {fallback.r, fallback.g, fallback.b, 1.0};
    if (e.numbers(key, a, 4) < 3) {
        // ブラウザ API と同じ "#rrggbb" 表記も受ける（手書きしやすい）。
        const std::string s = e.attr(key);
        if (s.size() == 7 && s[0] == '#') return colorFromHex(s, fallback);
        return fallback;
    }
    Color3 c;
    c.r = float(a[0]);
    c.g = float(a[1]);
    c.b = float(a[2]);
    return c;
}

// ---- 種類の名前（MJCF の語彙に寄せる）--------------------------------------
const char* geomTypeName(ShapeKind s) {
    switch (s) {
        case ShapeKind::Sphere: return "sphere";
        case ShapeKind::Model: return "mesh";
        case ShapeKind::Box: break;
    }
    return "box";
}
ShapeKind geomTypeFromName(const std::string& s, ShapeKind fallback) {
    if (s == "box") return ShapeKind::Box;
    if (s == "sphere") return ShapeKind::Sphere;
    if (s == "mesh" || s == "model") return ShapeKind::Model;
    return fallback;
}

// 拘束の種類。MuJoCo の joint / equality の名前を採る（weld, hinge, ball,
// slide, distance）。旧 JSON の名前（fixed, revolute, ...）でも読める。
const char* jointTypeName(JointKind k) {
    switch (k) {
        case JointKind::Fixed: return "weld";
        case JointKind::Spherical: return "ball";
        case JointKind::Prismatic: return "slide";
        case JointKind::Distance: return "distance";
        case JointKind::Revolute: break;
    }
    return "hinge";
}
JointKind jointTypeFromName(const std::string& s, JointKind fallback) {
    if (s == "weld") return JointKind::Fixed;
    if (s == "slide") return JointKind::Prismatic;
    return jointFromName(s, fallback);  // fixed / hinge / ball / rod ...
}

const char* lightTypeName(LightKind k) {
    switch (k) {
        case LightKind::Sun: return "directional";
        case LightKind::Spot: return "spot";
        case LightKind::Point: break;
    }
    return "point";
}

// ---- body の参照（名前 or 番号、-1 = 地面）---------------------------------
bool looksNumeric(const std::string& s) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' &&
            c != '+') {
            return false;
        }
    }
    return true;
}

// 保存側: 名前が一意で、数字と紛れないときだけ名前で書く。
std::string bodyRefText(const std::vector<BodyDesc>& bodies, int index) {
    if (index < 0 || std::size_t(index) >= bodies.size()) return "world";
    const std::string& name = bodies[std::size_t(index)].name;
    if (!name.empty() && !looksNumeric(name)) {
        int count = 0;
        for (const auto& b : bodies) {
            if (b.name == name) ++count;
        }
        if (count == 1) return name;
    }
    return std::to_string(index);
}

// 読み込み側: 数字ならその番号、"world"（と空・負数）は地面、それ以外は
// 名前引き。見つからない参照は黙って地面にしない - 打ち間違えたジョイントが
// 「なぜか固定される」のは追いにくいので、警告を残す。
int bodyRefIndex(const std::vector<BodyDesc>& bodies, const std::string& text,
                 const char* attr, Warn& warn) {
    if (text.empty() || text == "world" || text == "ground") return -1;
    if (looksNumeric(text)) {
        const int index = std::atoi(text.c_str());
        if (index < 0) return -1;  // -1 は明示的な地面
        if (std::size_t(index) >= bodies.size()) {
            warn(std::string(attr) + "=\"" + text + "\" is out of range (" +
                 std::to_string(bodies.size()) +
                 " objects) - reading as world");
            return -1;
        }
        return index;
    }
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        if (bodies[i].name == text) return int(i);
    }
    warn(std::string(attr) + "=\"" + text +
         "\" does not name an object - reading as world (ground)");
    return -1;
}

// ---- 各節の書き出し ---------------------------------------------------------
xml::Element optionElement(const SimSettings& s) {
    xml::Element o("option");
    const double gravity[3] = {0.0, s.gravity, 0.0};
    o.setNumbers("gravity", gravity, 3);
    o.setInt("rate", s.hz);  // MuJoCo の timestep に相当（こちらは Hz）
    o.setInt("substeps", s.substeps);
    o.setInt("iterations", s.iterations);
    o.setNumber("envelope", s.envelope);
    o.setNumber("recovery", s.recovery);
    o.setNumber("friction", s.friction, 6);
    o.setNumber("restitution", s.restitution, 6);
    o.setNumber("lineardamping", s.linearDamping);
    o.setNumber("angulardamping", s.angularDamping);
    o.setBool("sleeping", s.sleeping);
    return o;
}

SimSettings optionFromXml(const xml::Element& o, SimSettings base) {
    double gravity[3] = {0.0, base.gravity, 0.0};
    const std::size_t got = o.numbers("gravity", gravity, 3);
    if (got >= 3) base.gravity = gravity[1];
    else if (got == 1) base.gravity = gravity[0];  // スカラー表記も受ける
    if (o.has("rate")) {
        base.hz = o.integer("rate", base.hz);
    } else if (o.has("timestep")) {
        // MuJoCo 流の timestep（秒）で書かれていたら Hz に直す。
        const double dt = o.number("timestep", 1.0 / double(base.hz));
        if (dt > 1e-6) base.hz = int(1.0 / dt + 0.5);
    }
    base.substeps = o.integer("substeps", base.substeps);
    base.iterations = o.integer("iterations", base.iterations);
    base.envelope = o.number("envelope", base.envelope);
    base.recovery = o.number("recovery", base.recovery);
    base.friction = float(o.number("friction", base.friction));
    base.restitution = float(o.number("restitution", base.restitution));
    base.linearDamping = o.number("lineardamping", base.linearDamping);
    base.angularDamping = o.number("angulardamping", base.angularDamping);
    base.sleeping = o.boolean("sleeping", base.sleeping);
    return base;
}

xml::Element bodyElement(const BodyDesc& b) {
    xml::Element body("body");
    if (!b.name.empty()) body.set("name", b.name);
    setVec3(body, "pos", b.position);
    setVec3(body, "euler", b.rotation);
    body.setBool("fixed", b.fixed);

    xml::Element geom("geom");
    geom.set("type", geomTypeName(b.shape));
    if (b.shape == ShapeKind::Model && !b.mesh.empty()) {
        geom.set("mesh", b.mesh);
    }
    // MuJoCo と同じ「半分の寸法」。box は 3 つ、sphere / mesh は半径 1 つ。
    if (b.shape == ShapeKind::Box) {
        const double half[3] = {b.size.x * 0.5, b.size.y * 0.5, b.size.z * 0.5};
        geom.setNumbers("size", half, 3);
    } else {
        geom.setNumber("size", b.size.x * 0.5);
    }
    geom.setNumber("mass", b.mass);
    setColor(geom, "rgba", b.color);
    // 当たり判定が見た目と違うときだけ書く（既存シーンの「見た目は glTF・
    // 当たりは球」のような組み合わせ）。
    if (b.collision != b.shape) {
        geom.set("collision", geomTypeName(b.collision));
    }
    body.append(std::move(geom));
    return body;
}

BodyDesc bodyFromXml(const xml::Element& e,
                     const std::vector<MeshAssetDesc>& meshes, Warn& warn) {
    BodyDesc b;
    b.name = e.attr("name");
    b.position = getVec3(e, "pos", b.position);
    b.rotation = getVec3(e, "euler", b.rotation);
    b.fixed = e.boolean("fixed", b.fixed);

    // 構造の検査。MJCF の入れ子 <body> は親からの相対姿勢なので、姿勢を
    // 合成せずに取り込むと物が別の場所に出る - 黙って捨てず、必ず伝える。
    const std::string label =
        b.name.empty() ? std::string("(unnamed)") : b.name;
    std::size_t geoms = 0;
    for (const auto& c : e.children()) {
        if (c.name() == "geom") {
            ++geoms;
        } else if (c.name() == "body") {
            warn("<body name=\"" + label + "\">: nested <body> is not "
                 "supported - ignored (place bodies directly under "
                 "worldbody)");
        } else {
            warn("<body name=\"" + label + "\">: unsupported <" + c.name() +
                 "> - ignored");
        }
    }
    if (geoms > 1) {
        warn("<body name=\"" + label +
             "\">: extra <geom> elements are ignored (one shape per body)");
    }

    // MJCF は 1 つの body に複数 geom を書けるが、こちらの剛体は 1 形状。
    // 先頭の geom だけを見る。
    if (const xml::Element* g = e.first("geom")) {
        if (g->has("type") &&
            unknownName(geomTypeFromName, g->attr("type"), ShapeKind::Box,
                        ShapeKind::Sphere)) {
            warn("<body name=\"" + label + "\">: geom type=\"" +
                 g->attr("type") + "\" is unknown - reading as box");
        }
        b.shape = geomTypeFromName(g->attr("type", "box"), b.shape);
        if (g->has("collision") &&
            unknownName(geomTypeFromName, g->attr("collision"), ShapeKind::Box,
                        ShapeKind::Sphere)) {
            warn("<body name=\"" + label + "\">: collision=\"" +
                 g->attr("collision") +
                 "\" is unknown - using the visual shape");
        }
        b.collision = geomTypeFromName(g->attr("collision"), b.shape);
        // mesh geom はアセット参照が要る。無い・見つからないは球へ倒す
        // （読める文書は必ず描ける、を保つ。旧 JSON 由来の「名無しの model」
        // も createObject 側で同じ倒し方をする）。
        b.mesh = g->attr("mesh");
        if (b.shape == ShapeKind::Model) {
            bool found = false;
            for (const auto& m : meshes) {
                if (m.name == b.mesh) { found = true; break; }
            }
            if (b.mesh.empty()) {
                warn("<body name=\"" + label + "\">: geom type=\"mesh\" "
                     "needs mesh=\"<asset name>\" - reading as a sphere");
            } else if (!found) {
                warn("<body name=\"" + label + "\">: mesh=\"" + b.mesh +
                     "\" is not declared in <asset> - reading as a sphere");
            }
            if (b.mesh.empty() || !found) {
                b.shape = ShapeKind::Sphere;
                b.mesh.clear();
                if (b.collision == ShapeKind::Model) {
                    b.collision = ShapeKind::Sphere;
                }
            }
        }
        if (b.shape == ShapeKind::Box) {
            double half[3] = {b.size.x * 0.5, b.size.y * 0.5, b.size.z * 0.5};
            const std::size_t got = g->numbers("size", half, 3);
            if (got == 1) half[1] = half[2] = half[0];  // 立方体の省略記法
            b.size = Vec3d{half[0] * 2.0, half[1] * 2.0, half[2] * 2.0};
        } else {
            const double d = g->number("size", b.size.x * 0.5) * 2.0;
            b.size = Vec3d{d, d, d};
        }
        b.mass = g->number("mass", b.mass);
        b.color = getColor(*g, "rgba", b.color);
    } else {
        warn("<body name=\"" + label +
             "\"> has no <geom> - using a default box");
    }
    return clampBody(b);
}

xml::Element lightElement(const LightDesc& l) {
    xml::Element e("light");
    if (!l.name.empty()) e.set("name", l.name);
    e.set("type", lightTypeName(l.kind));
    setVec3(e, "pos", l.position);
    // 向きはオイラー角（度）。ゼロ = 真下 (0,-1,0) を向く、が全体の約束。
    setVec3(e, "euler", l.rotation);
    setColor(e, "diffuse", l.color);
    e.setNumber("intensity", l.intensity);
    e.setNumber("falloff", l.falloff);
    e.setNumber("inner", l.spotInnerDeg);
    e.setNumber("cutoff", l.spotOuterDeg);  // MuJoCo の cutoff = 円錐の外側
    e.setBool("castshadow", l.shadows);
    return e;
}

LightDesc lightFromXml(const xml::Element& e, Warn& warn) {
    LightDesc l;
    l.name = e.attr("name");
    if (e.has("type") &&
        unknownName(lightKindFromName, e.attr("type"), LightKind::Sun,
                    LightKind::Point)) {
        warn("<light> type=\"" + e.attr("type") +
             "\" is unknown - reading as point");
    }
    // directional="true" 表記（MJCF）も受ける。
    l.kind = lightKindFromName(e.attr("type", "point"), l.kind);
    if (e.boolean("directional", false)) l.kind = LightKind::Sun;
    l.position = getVec3(e, "pos", l.position);
    l.rotation = getVec3(e, "euler", l.rotation);
    l.color = getColor(e, "diffuse", l.color);
    l.intensity = e.number("intensity", l.intensity);
    l.falloff = e.number("falloff", l.falloff);
    l.spotInnerDeg = e.number("inner", l.spotInnerDeg);
    l.spotOuterDeg = e.number("cutoff", l.spotOuterDeg);
    l.shadows = e.boolean("castshadow", l.shadows);
    return clampLight(l);
}

xml::Element cameraElement(const CameraPose& c, std::size_t slot) {
    xml::Element e("camera");
    e.set("name", "cam" + std::to_string(slot));
    setVec3(e, "target", c.target);
    // オービット表現のまま持つ（実体の CameraObject と同じ）。角度は度。
    // 角度は 6 桁で十分（0.000001 度 = 2e-8 ラジアン）。既定の 10 桁だと
    // ラジアンからの変換で出た端数がそのまま並んで読みにくい。
    e.setNumber("azimuth", toDegrees(c.azimuth), 6);
    e.setNumber("elevation", toDegrees(c.elevation), 6);
    e.setNumber("radius", c.radius);
    e.setBool("active", c.active);
    return e;
}

CameraPose cameraFromXml(const xml::Element& e) {
    CameraPose c;
    c.target = getVec3(e, "target", c.target);
    c.azimuth = toRadians(e.number("azimuth", toDegrees(c.azimuth)));
    c.elevation = toRadians(e.number("elevation", toDegrees(c.elevation)));
    c.radius = e.number("radius", c.radius);
    c.active = e.boolean("active", c.active);
    return c;
}

xml::Element jointElement(const JointDesc& j,
                          const std::vector<BodyDesc>& bodies) {
    xml::Element e("joint");
    if (!j.name.empty()) e.set("name", j.name);
    e.set("type", jointTypeName(j.kind));
    e.set("body1", bodyRefText(bodies, j.bodyA));
    e.set("body2", bodyRefText(bodies, j.bodyB));
    setVec3(e, "anchor", j.anchor);
    setVec3(e, "axis", j.axis);
    if (j.kind == JointKind::Distance) e.setNumber("distance", j.distance);
    return e;
}

JointDesc jointFromXml(const xml::Element& e,
                       const std::vector<BodyDesc>& bodies, Warn& warn) {
    JointDesc j;
    j.name = e.attr("name");
    const std::string label =
        j.name.empty() ? std::string("<joint>")
                       : "<joint name=\"" + j.name + "\">";
    if (e.has("type") &&
        unknownName(jointTypeFromName, e.attr("type"), JointKind::Fixed,
                    JointKind::Revolute)) {
        warn(label + " type=\"" + e.attr("type") +
             "\" is unknown - reading as hinge");
    }
    j.kind = jointTypeFromName(e.attr("type", "hinge"), j.kind);
    j.bodyA = bodyRefIndex(bodies, e.attr("body1", "world"), "body1", warn);
    j.bodyB = bodyRefIndex(bodies, e.attr("body2", "world"), "body2", warn);
    j.anchor = getVec3(e, "anchor", j.anchor);
    j.axis = getVec3(e, "axis", j.axis);
    // 零ベクトルの軸は正規化できず、Chrono に渡る前にここで止める
    // （hinge / slide は軸が意味を持つ。他の種類でも害は無い）。
    const double len2 =
        j.axis.x * j.axis.x + j.axis.y * j.axis.y + j.axis.z * j.axis.z;
    if (len2 < 1e-12) {
        warn(label + " has a zero-length axis - using 0 1 0");
        j.axis = Vec3d{0.0, 1.0, 0.0};
    }
    j.distance = e.number("distance", j.distance);
    return j;
}

// イベントグラフ（MuJoCo には無い WizEngine の拡張）。種類ごとに意味のある
// 属性だけを書く - 使わない欄まで並べると、手で読むときに嘘の情報になる。
xml::Element nodeElement(const NodeDesc& n) {
    xml::Element e("node");
    e.setInt("id", n.id);
    e.set("type", nodeKindName(n.kind));
    const double pos[2] = {n.x, n.y};
    e.setNumbers("pos", pos, 2);  // ノードエディタのキャンバス座標 (px)

    const NodeTargetKind tk = nodeTargetKind(n.kind);
    if (tk != NodeTargetKind::None) e.setInt("target", n.target);
    if (nodeOtherIsObject(n.kind)) e.setInt("other", n.other);
    if (n.kind == NodeKind::OnTimer) e.setNumber("seconds", n.seconds);
    if (n.kind == NodeKind::SetColor || n.kind == NodeKind::SetLightColor) {
        setColor(e, "rgba", n.color);
    }
    if (n.kind == NodeKind::ApplyImpulse) setVec3(e, "velocity", n.vec);
    if (n.kind == NodeKind::SetFixed ||
        n.kind == NodeKind::SetLightIntensity) {
        e.setNumber("value", n.value);
    }
    return e;
}

NodeDesc nodeFromXml(const xml::Element& e, Warn& warn) {
    NodeDesc n;
    n.id = e.integer("id", 0);  // 0 = 未指定（呼び出し側が採番する）
    if (e.has("type") &&
        unknownName(nodeKindFromName, e.attr("type"), NodeKind::OnCollision,
                    NodeKind::OnSimStart)) {
        warn("<node> type=\"" + e.attr("type") +
             "\" is unknown - reading as onCollision");
    }
    n.kind = nodeKindFromName(e.attr("type", "onCollision"), n.kind);
    double pos[2] = {n.x, n.y};
    e.numbers("pos", pos, 2);
    n.x = pos[0];
    n.y = pos[1];
    n.target = e.integer("target", n.target);
    n.other = e.integer("other", n.other);
    n.seconds = e.number("seconds", n.seconds);
    n.color = getColor(e, "rgba", n.color);
    n.vec = getVec3(e, "velocity", n.vec);
    n.value = e.number("value", n.value);
    return clampNode(n);
}

}  // namespace

xml::Element toXml(const SceneDocument& doc) {
    xml::Element root("wizengine");
    if (!doc.model.empty()) root.set("model", doc.model);
    root.setInt("version", kSceneDocVersion);
    root.append(optionElement(doc.sim));

    if (!doc.meshes.empty()) {
        xml::Element asset("asset");
        for (const auto& m : doc.meshes) {
            xml::Element e("mesh");
            e.set("name", m.name);
            e.set("file", m.file);
            e.setNumber("scale", m.scale);
            asset.append(std::move(e));
        }
        root.append(std::move(asset));
    }

    xml::Element world("worldbody");
    if (doc.hasEnvironment) {
        xml::Element e("environment");
        e.set("hdr", doc.environment.hdr);
        e.setNumber("intensity", doc.environment.intensity);
        world.append(std::move(e));
    }
    if (doc.hasGround) {
        xml::Element g("ground");
        g.setNumber("size", doc.ground.half);
        g.setNumber("visual", doc.ground.visualHalf);
        g.set("texture", doc.ground.texture);
        g.setNumber("tile", doc.ground.tile);
        setColor(g, "rgba", doc.ground.tint);
        world.append(std::move(g));
    }
    for (const auto& l : doc.lights) world.append(lightElement(l));
    for (std::size_t i = 0; i < doc.cameras.size(); ++i) {
        world.append(cameraElement(doc.cameras[i], i));
    }
    for (const auto& b : doc.bodies) world.append(bodyElement(b));
    root.append(std::move(world));

    if (!doc.joints.empty()) {
        xml::Element eq("equality");
        for (const auto& j : doc.joints) eq.append(jointElement(j, doc.bodies));
        root.append(std::move(eq));
    }

    if (!doc.nodes.empty() || !doc.wires.empty()) {
        xml::Element ev("events");
        for (const auto& n : doc.nodes) ev.append(nodeElement(n));
        for (const auto& w : doc.wires) {
            xml::Element e("wire");
            e.setInt("from", w.from);
            e.setInt("to", w.to);
            ev.append(std::move(e));
        }
        root.append(std::move(ev));
    }
    return root;
}

std::string toXmlText(const SceneDocument& doc) {
    return xml::write(toXml(doc));
}

SceneDocument fromXml(const xml::Element& root,
                      std::vector<std::string>* warnings) {
    Warn warn(warnings);
    SceneDocument doc;
    doc.model = root.attr("model");

    // 新しい版の文書は読める範囲で読む（属性を足すだけなら版は上がらない
    // 約束なので、ここに来るのは意味の変わる変更があったときだけ）。
    const int version = root.integer("version", kSceneDocVersion);
    if (version > kSceneDocVersion) {
        warn("document version " + std::to_string(version) +
             " is newer than this build (" +
             std::to_string(kSceneDocVersion) +
             ") - reading what is understood");
    }

    warnUnknownChildren(root,
                        {"option", "asset", "worldbody", "equality", "events"},
                        "<wizengine>", warn);

    if (const xml::Element* o = root.first("option")) {
        doc.sim = clampSim(optionFromXml(*o, doc.sim));
        doc.hasSim = true;
    }

    if (const xml::Element* asset = root.first("asset")) {
        warnUnknownChildren(*asset, {"mesh"}, "<asset>", warn);
        for (const xml::Element* me : asset->all("mesh")) {
            MeshAssetDesc m;
            m.name = me->attr("name");
            m.file = me->attr("file");
            m.scale = me->number("scale", 1.0);
            if (m.scale <= 1e-9) m.scale = 1.0;
            if (m.name.empty()) {
                warn("<mesh file=\"" + m.file +
                     "\"> has no name - ignored");
                continue;
            }
            if (!assetFileAllowed(m.file)) {
                warn("<mesh name=\"" + m.name + "\"> file=\"" + m.file +
                     "\" is not allowed (assets-relative paths only) - "
                     "ignored");
                continue;
            }
            bool dup = false;
            for (const auto& prev : doc.meshes) {
                if (prev.name == m.name) { dup = true; break; }
            }
            if (dup) {
                warn("<mesh name=\"" + m.name + "\" file=\"" + m.file +
                     "\"> duplicates an earlier name - ignored");
                continue;
            }
            doc.meshes.push_back(std::move(m));
        }
    }

    if (const xml::Element* world = root.first("worldbody")) {
        warnUnknownChildren(*world,
                            {"environment", "ground", "light", "camera", "body"},
                            "<worldbody>", warn);

        // 環境光と地面は worldbody 直下の単一要素。2 個目以降は警告して無視。
        const auto envs = world->all("environment");
        if (!envs.empty()) {
            doc.hasEnvironment = true;
            const xml::Element& e = *envs[0];
            if (e.has("hdr")) {
                const std::string hdr = e.attr("hdr");
                if (hdr.empty() || assetFileAllowed(hdr)) {
                    doc.environment.hdr = hdr;
                } else {
                    warn("<environment> hdr=\"" + hdr +
                         "\" is not allowed (assets-relative paths only) - "
                         "loading no environment");
                    doc.environment.hdr.clear();
                }
            }
            doc.environment.intensity =
                e.number("intensity", doc.environment.intensity);
            doc.environment = clampEnvironment(doc.environment);
            if (envs.size() > 1) {
                warn("multiple <environment> elements - using the first");
            }
        }
        const auto grounds = world->all("ground");
        if (!grounds.empty()) {
            doc.hasGround = true;
            const xml::Element& g = *grounds[0];
            doc.ground.half = g.number("size", doc.ground.half);
            doc.ground.visualHalf = g.number("visual", doc.ground.visualHalf);
            if (g.has("texture")) {
                const std::string tex = g.attr("texture");
                if (tex.empty() || assetFileAllowed(tex)) {
                    doc.ground.texture = tex;
                } else {
                    warn("<ground> texture=\"" + tex +
                         "\" is not allowed (assets-relative paths only) - "
                         "using the checkerboard");
                    doc.ground.texture.clear();
                }
            }
            doc.ground.tile = g.number("tile", doc.ground.tile);
            doc.ground.tint = getColor(g, "rgba", doc.ground.tint);
            doc.ground = clampGround(doc.ground);
            if (grounds.size() > 1) {
                warn("multiple <ground> elements - using the first");
            }
        }
        // ライト・カメラを 1 つも書かない文書は「指定なし」＝初期構成で開く
        // （手書きの最小 XML が真っ暗にならないように）。
        for (const xml::Element* l : world->all("light")) {
            doc.lights.push_back(lightFromXml(*l, warn));
        }
        doc.hasLights = !doc.lights.empty();
        for (const xml::Element* c : world->all("camera")) {
            doc.cameras.push_back(cameraFromXml(*c));
        }
        doc.hasCameras = !doc.cameras.empty();
        // body は worldbody の直下だけを見る（MJCF の入れ子 body は
        // 親からの相対姿勢なので、姿勢を合成せずに平らへ落とすと物が
        // 別の場所に出てしまう。こちらの剛体一覧は元から平ら。入れ子が
        // 書かれていたら bodyFromXml が警告する）。
        for (const xml::Element* b : world->all("body")) {
            doc.bodies.push_back(bodyFromXml(*b, doc.meshes, warn));
        }
    }

    if (const xml::Element* eq = root.first("equality")) {
        warnUnknownChildren(*eq, {"joint"}, "<equality>", warn);
        for (const xml::Element* j : eq->all("joint")) {
            doc.joints.push_back(jointFromXml(*j, doc.bodies, warn));
        }
    }

    if (const xml::Element* ev = root.first("events")) {
        warnUnknownChildren(*ev, {"node", "wire"}, "<events>", warn);

        // ノード。id は一意が前提（ワイヤーが id で指すため）。重複は後の方を
        // 捨て、未指定（id 属性なし）は空き番号を振る - 手書きの XML で id を
        // 省けるように。
        std::set<int> usedIds;
        std::vector<NodeDesc> pending;
        for (const xml::Element* n : ev->all("node")) {
            NodeDesc nd = nodeFromXml(*n, warn);
            const std::string label =
                nd.id > 0 ? "<node id=\"" + std::to_string(nd.id) + "\">"
                          : std::string("<node>");

            // 対象番号の範囲。文書の中で完結して検証できるものはここで
            // 落としておく（読み込み後に base ぶんずれてからでは遅い）。
            const NodeTargetKind tk = nodeTargetKind(nd.kind);
            if (tk == NodeTargetKind::Object && nd.target >= 0 &&
                std::size_t(nd.target) >= doc.bodies.size()) {
                warn(label + " target=" + std::to_string(nd.target) +
                     " is out of range (objects) - cleared");
                nd.target = -1;
            }
            if (tk == NodeTargetKind::Light && doc.hasLights &&
                nd.target >= 0 &&
                std::size_t(nd.target) >= doc.lights.size()) {
                warn(label + " target=" + std::to_string(nd.target) +
                     " is out of range (lights) - cleared");
                nd.target = -1;
            }
            if (nodeOtherIsObject(nd.kind) && nd.other >= 0 &&
                std::size_t(nd.other) >= doc.bodies.size()) {
                warn(label + " other=" + std::to_string(nd.other) +
                     " is out of range (objects)");
                nd.other = (nd.kind == NodeKind::OnCollision) ? -2 : -1;
            }

            if (nd.id > 0) {
                if (!usedIds.insert(nd.id).second) {
                    warn(label + " duplicates an earlier node id - ignored");
                    continue;
                }
            }
            pending.push_back(nd);
        }
        int nextId = usedIds.empty() ? 1 : (*usedIds.rbegin() + 1);
        for (auto& nd : pending) {
            if (nd.id <= 0) nd.id = nextId++;
            doc.nodes.push_back(nd);
        }

        // ワイヤー。エディタ経由なら addGraphWire が検証するが、文書からの
        // 読み込みはここが唯一の関所: 存在する id か・向きは
        // トリガー → アクションか・重複していないか。
        std::map<int, NodeKind> kindById;
        for (const auto& nd : doc.nodes) kindById[nd.id] = nd.kind;
        std::set<std::pair<int, int>> seen;
        for (const xml::Element* w : ev->all("wire")) {
            WireDesc wire;
            wire.from = w->integer("from", -1);
            wire.to = w->integer("to", -1);
            const std::string label = "<wire from=\"" +
                                      std::to_string(wire.from) + "\" to=\"" +
                                      std::to_string(wire.to) + "\">";
            const auto f = kindById.find(wire.from);
            const auto t = kindById.find(wire.to);
            if (f == kindById.end() || t == kindById.end()) {
                warn(label + " points at a missing node - ignored");
                continue;
            }
            if (!nodeIsTrigger(f->second) || nodeIsTrigger(t->second)) {
                warn(label + " has the wrong direction (from=trigger, "
                     "to=action) - ignored");
                continue;
            }
            if (!seen.insert({wire.from, wire.to}).second) {
                warn(label + " duplicates an earlier wire - ignored");
                continue;
            }
            doc.wires.push_back(wire);
        }
    }
    return doc;
}

bool parseXml(const std::string& text, SceneDocument& out, std::string& error,
              std::vector<std::string>* warnings) {
    xml::Element root;
    if (!xml::parse(text, root, error)) return false;
    out = fromXml(root, warnings);
    return true;
}

SceneDocument fromLegacyJson(const nlohmann::json& doc) {
    SceneDocument out;
    if (!doc.is_object()) return out;
    if (doc.contains("sim")) {
        out.sim = clampSim(simFromJson(doc["sim"], out.sim));
        out.hasSim = true;
    }
    if (doc.contains("lights") && doc["lights"].is_array()) {
        out.hasLights = true;
        for (const auto& l : doc["lights"]) {
            out.lights.push_back(clampLight(lightFromJson(l, LightDesc{})));
        }
    }
    if (doc.contains("cameras") && doc["cameras"].is_array()) {
        out.hasCameras = true;
        for (const auto& c : doc["cameras"]) {
            out.cameras.push_back(cameraPoseFromJson(c, CameraPose{}));
        }
    }
    if (doc.contains("objects") && doc["objects"].is_array()) {
        for (const auto& b : doc["objects"]) {
            out.bodies.push_back(clampBody(bodyFromJson(b, BodyDesc{})));
        }
    }
    if (doc.contains("joints") && doc["joints"].is_array()) {
        for (const auto& j : doc["joints"]) {
            out.joints.push_back(jointFromJson(j, JointDesc{}));
        }
    }
    if (doc.contains("nodes") && doc["nodes"].is_array()) {
        for (const auto& n : doc["nodes"]) {
            out.nodes.push_back(clampNode(nodeFromJson(n, NodeDesc{})));
        }
    }
    if (doc.contains("wires") && doc["wires"].is_array()) {
        for (const auto& w : doc["wires"]) {
            out.wires.push_back(wireFromJson(w, WireDesc{}));
        }
    }
    return out;
}

}  // namespace editor
}  // namespace wizengine
