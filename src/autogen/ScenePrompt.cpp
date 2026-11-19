#include "autogen/ScenePrompt.h"

#include <algorithm>
#include <cstdio>

namespace wizengine {
namespace autogen {

const char* genModeName(GenMode m) {
    switch (m) {
        case GenMode::Edit: return "edit";
        case GenMode::Append: return "append";
        default: return "new";
    }
}

GenMode genModeFromName(const std::string& s, GenMode fallback) {
    if (s == "new") return GenMode::New;
    if (s == "edit") return GenMode::Edit;
    if (s == "append") return GenMode::Append;
    return fallback;
}

namespace {

// 外接箱の数字（3 桁で十分。モデル単位なので m とは限らない）。
std::string num3(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3g", v);
    return buf;
}

}  // namespace

const char* systemPromptExample() {
    // 警告 0 件で読めること（ScenePrompt.h）。数字は「置いた瞬間に重ならない」
    // ように合わせてある: 机の天板の上面 y = 0.78、コップの底 = 0.79。腕は
    // 柱の手前（z）に 3 cm 離してあるので、ヒンジで回しても柱に当たらない。
    return R"(<wizengine model="example_room" version="4">
  <option gravity="0 -9.81 0"/>
  <visual>
    <quality shadow="pcss" msaa="4"/>
    <postprocess ssao="true" bloom="0.05" vignette="0.2"/>
  </visual>
  <worldbody>
    <ground size="10" visual="8" rgba="0.8 0.8 0.8 1" roughness="0.9"/>
    <light name="sun" type="sun" pos="0 8 0" euler="50 -35 0" diffuse="1 0.97 0.92 1" intensity="90000" castshadow="true"/>
    <light name="fill" type="point" pos="-3 4 3" diffuse="0.8 0.87 1 1" intensity="40000" falloff="20" castshadow="false"/>
    <camera name="cam0" target="0 0.8 -0.5" azimuth="35" elevation="22" radius="8"/>
    <body name="crate" pos="-1.5 0.25 0" euler="0 20 0">
      <geom type="box" size="0.25 0.25 0.25" mass="8" rgba="0.55 0.35 0.18 1" roughness="0.8"/>
    </body>
    <body name="ball" pos="-0.6 0.15 0.6">
      <geom type="sphere" size="0.15" mass="0.5" rgba="0.8 0.1 0.1 1" roughness="0.3" restitution="0.6"/>
    </body>
    <body name="table_top" pos="1.5 0.75 0" fixed="true">
      <geom type="box" size="0.8 0.03 0.5" mass="20" rgba="0.4 0.25 0.12 1"/>
    </body>
    <body name="cup" pos="1.5 0.85 0">
      <geom type="box" size="0.05 0.06 0.05" mass="0.3" rgba="0.9 0.9 0.95 1" roughness="0.1" clearcoat="1"/>
    </body>
    <body name="post" pos="0 1 -2" fixed="true">
      <geom type="box" size="0.05 1 0.05" mass="10" rgba="0.42 0.45 0.5 1" roughness="0.3" metallic="1"/>
    </body>
    <body name="arm" pos="0.45 1.9 -1.88">
      <geom type="box" size="0.4 0.04 0.04" mass="2" rgba="0.9 0.6 0.1 1"/>
    </body>
  </worldbody>
  <equality>
    <joint name="arm_hinge" type="hinge" body1="arm" body2="post" anchor="0 1.9 -1.88" axis="0 0 1"/>
  </equality>
</wizengine>)";
}

std::string buildSystemPrompt(const PromptOptions& opt) {
    std::string s = R"(You build 3D physics scenes for the Charon engine (Chrono rigid-body physics + Filament PBR renderer,
viewed and edited in a web browser). You answer with exactly ONE XML document whose root element is <wizengine>.
No prose, no markdown.

HOW THE ENGINE USES YOUR DOCUMENT
- Your document replaces (or extends) the scene in EDITOR mode: nothing moves yet. When the user presses
  Simulate, physics starts from EXACTLY the poses you wrote; Stop puts every body back to those poses.
  So the document is the starting state: nothing may intersect, and anything that is not fixed will fall
  under gravity and collide.
- fixed="true" bodies never move (walls, shelves, buildings, table tops, posts). All other bodies are dynamic.
- During simulation the user can grab dynamic bodies with the mouse and drag them (this is attached
  automatically). Light and mass-realistic objects are fun to push around; make heavy furniture fixed.
- One body = exactly ONE <geom> (one box, one sphere or one model). Build complex objects from several bodies:
  make them all fixed, or connect moving parts with joints (weld = glued together).
- The user looks at the scene through the EDITOR CAMERA, written as <camera name="@CAM@">. Other cameras
  (cam0, cam1, ...) are extra viewpoints shown on other browser pages.
- The engine reads your document strictly. If something is wrong (unknown element, missing body name,
  bad attribute value), it sends you the list of problems and asks for a corrected document.
- When you edit or extend an existing scene you also get a SCENE SUMMARY computed by the engine: sizes,
  colours, what rests on what, what floats or intersects, and where each body appears in the user's view.
  Use it to identify what the user is talking about ("the red box on the left", "the cup on the table").
  "@name" in a request names the element with exactly that name; the summary also tells you which body
  the user has SELECTED ("this", "it", "これ" mean that body). Keep existing names unchanged.

WORLD CONVENTIONS
- Units are meters, kilograms, seconds, newtons. Y is up, gravity is -Y. The floor is the plane y = 0.
- All angles in the document are in degrees. euler="x y z" is applied as R = Rz * Ry * Rx.
- A body's pos is its CENTER. Something resting on the floor has pos y = half its height (+0.01).
- Colors are rgba="r g b 1" with LINEAR values 0..1 (sRGB 0.5 grey is about 0.21 linear).
- Leave 1-2 cm gaps between bodies at the start. Stack things by putting each one's bottom 1 cm above the
  top of the one below. Joined bodies also collide with each other, so keep their shapes apart too.
- Keep everything within x, z in [-9, 9] (the floor is 20 m wide) unless you also enlarge <ground size>.

EXAMPLE DOCUMENT (MuJoCo MJCF-like; this exact text is valid)
)";
    // 見本のカメラもエディタカメラの名前で見せる（cam0 のままだと N ≠ 0 のとき
    // LLM がそれを真似て別のスロットに書く）。
    {
        std::string ex = systemPromptExample();
        const std::string from = "name=\"cam0\"";
        const auto at = ex.find(from);
        if (at != std::string::npos) ex.replace(at, from.size(), "name=\"@CAM@\"");
        s += ex;
    }
    s += R"(

ELEMENT NOTES (omitted attributes take engine defaults, so write only what matters)
- <body name pos euler fixed>: name is required and unique (letters, digits, _ and - only).
  Bodies go DIRECTLY under <worldbody>; nested <body> elements are not supported.
- <geom type size mass rgba ...>: type is box | sphere | mesh (no cylinder, capsule, plane or ellipsoid -
  approximate those with boxes and spheres). size is HALF the dimension: box = three half-extents
  "hx hy hz", sphere = radius, mesh = rough radius used only as a fallback.
  mass in kg (realistic: a 50 cm wooden crate ~8, a football 0.45, a chair 6).
  Optional: roughness (0.02..1), metallic (0 or 1), clearcoat (0..1), emissive (0..10, glow),
  friction (0..2), restitution (0..1, bounciness).
- <light name type pos euler diffuse intensity castshadow>: type sun | point | spot.
  euler "0 0 0" points STRAIGHT DOWN; tilt with x (e.g. 50), turn with y. pos only matters for point/spot.
  Intensity: sun 50000-110000 (lux), point/spot 20000-150000 (lumen); falloff = reach in m (point/spot);
  spot also takes inner and cutoff (cone angles in degrees, inner < cutoff < 89).
  Use 1-3 lights; exactly one sun with castshadow="true" is usually best.
- <camera name="camN" target azimuth elevation radius>: an orbit camera around target. azimuth = degrees
  around Y, elevation = degrees above the horizon (15-35 looks good), radius = distance in m (about 1.5x the
  scene width so everything is visible). The number in the name is the camera slot. @CAM@ is the editor
  camera (what the user sees); always write it. Write other slots only if the request asks for more
  viewpoints, and keep existing cameras unchanged in edit mode.
- <ground size visual rgba roughness texture tile>: size = half-width of the physical floor, visual =
  half-width of the visible floor (<= size). rgba tints the floor texture. Write at most one <ground>.
- <option gravity="x y z">: change only for space / moon scenes (moon: "0 -1.62 0").
- <visual>: only <quality shadow msaa>, <postprocess ssao bloom vignette> and
  <exposure aperture shutter sensitivity>; omit it for a normal look.
- Joints go in <equality>: <joint name type body1 body2 anchor axis .../>.
  type: weld | hinge | ball | slide | distance | spring.
  body1 moves relative to body2. body1/body2 are body NAMES defined in <worldbody>, or "world" for the ground;
  body1 and body2 must differ. anchor = world point (where the parts meet), axis = world direction
  (hinge: rotation axis, slide: sliding direction; not all zeros).
  hinge / slide may take ONE of:
    range="lo hi"  (hinge: degrees, slide: meters), or
    motor="speed|position|force" target="..."  (hinge: deg/s | deg | N*m, slide: m/s | m | N).
  A joint cannot have both range and motor (range would be ignored).
  spring: stiffness (N/m), damping (N*s/m), distance = rest length in m (0 = the starting distance).
  distance: keeps the starting distance between the two bodies (a rigid rod / rope that cannot stretch).
)";

    // ---- 使えるファイル（実在するものだけ）----
    const AssetInventory& inv = opt.assets;
    s += "\nFILES THAT EXIST (paths are relative to assets/; never invent other file names)\n";
    if (inv.hdrs.empty()) {
        s += "- No HDR environment maps. Do not write <environment>.\n";
    } else {
        s += "- <environment hdr=\"...\" intensity=\"30000\" skybox=\"true|false\"/> (image-based light; skybox shows\n"
             "  it as the background). Available HDR files:";
        for (const auto& h : inv.hdrs) s += " " + h;
        s += "\n  Omit <environment> to keep the default lighting.\n";
    }
    if (inv.textures.empty()) {
        s += "- No ground textures. Use texture=\"\" (checker pattern) or omit it.\n";
    } else {
        s += "- Ground textures (texture=\"\" = checker pattern, omitted = the default texture):";
        for (const auto& t : inv.textures) s += " " + t;
        s += "\n";
    }

    if (opt.meshGeneration) {
        s += R"(
MODELS (text-to-3D generation is AVAILABLE)
- For an object that boxes and spheres cannot represent well (furniture, animals, props, statues, vehicles as
  static props), declare a model to be generated and use it from a geom:
    <asset>
      <mesh name="chair" generate="a simple wooden dining chair, four legs, no background" height="0.9"/>
    </asset>
    <body name="chair1" pos="1 0.46 0" euler="0 90 0">
      <geom type="mesh" mesh="chair" size="0.35" mass="6"/>
    </body>
- generate = an English description of ONE isolated object (shape, material, style). No scene, no ground.
- height = the real-world height in meters; the engine scales the model so that its height matches exactly.
  The body's pos y for an object on the floor is therefore height/2 (+0.01). geom size = a rough radius.
- One <mesh> can be used by many bodies. Use at most )" +
             std::to_string(opt.maxMeshes) + R"( NEW generated meshes; build the rest from boxes and spheres.
  Generation is slow and costs money: prefer primitives for walls, floors, table tops, shelves, simple blocks.
- Collision of a model is its CONVEX HULL: nothing can sit inside a hollow model (cups, boxes, cars, shelves).
  Large models (houses, trees, statues) should be fixed="true".
- Do not write file= or scale= on a mesh that has generate=; the engine fills them in.
)";
        if (!opt.library.empty()) {
            s += R"(- These models were generated before and are FREE to reuse. Copy the generate text EXACTLY to reuse one:
)";
            for (const auto& e : opt.library) {
                s += "    * \"" + e.prompt + "\"\n";
            }
        }
    } else {
        s += R"(
MODELS
- Text-to-3D generation is NOT available. Do not write generate=. Build everything from boxes and spheres
  (combine several bodies, optionally fixed or welded, to suggest complex shapes).
- You may reuse <mesh> elements that already exist in the current scene, unchanged.
)";
    }
    if (!inv.models.empty()) {
        s += R"(- Existing model files you may declare with <mesh name="..." file="..." scale="..."/> and use from
  <geom type="mesh" mesh="...">. Size is the bounding box at scale 1 (x y z, model units);
  choose scale = wanted height / listed y size. Use them only when they match the request.
)";
        for (const auto& m : inv.models) {
            s += "    * " + m.file + "  size " + num3(m.size[0]) + " " + num3(m.size[1]) + " " +
                 num3(m.size[2]) + "\n";
        }
    }

    s += R"(
DO NOT WRITE
- <vehicle>, <terrain>, <soft>, <cable>, <prefab>, <emitter>, <event>/<events>, <formula>, <control>,
  <include>, <default>, <site>, <tendon>, <actuator>, <sensor> or any element not shown above.
  (These exist in the engine but need data you cannot produce; in edit mode keep existing ones unchanged.)

CHECK BEFORE ANSWERING
1. Every body has a unique name and exactly one <geom>; no body is nested in another.
2. Every joint's body1/body2 names a body you wrote (or "world"); no joint has both range and motor.
3. Every geom type="mesh" refers to a <mesh name> declared in <asset>.
4. Nothing intersects the floor or another body at the start (check y = half height, stacked heights).
5. @CAM@ sees the whole scene; there is one shadow-casting sun.

OUTPUT
- Reply with the XML document only. Start with <wizengine and end with </wizengine>.
)";
    // エディタカメラの名前を差し込む（config.json の scene.editorCamera）。
    const std::string cam = "cam" + std::to_string(opt.editorCamera < 0 ? 0 : opt.editorCamera);
    for (std::size_t at = s.find("@CAM@"); at != std::string::npos; at = s.find("@CAM@", at)) {
        s.replace(at, 5, cam);
        at += cam.size();
    }
    return s;
}

std::string buildUserPrompt(GenMode mode, const std::string& request,
                            const std::string& currentXml, const std::string& digest,
                            const std::string& history) {
    // 履歴は依頼の前に置く（今のシーンと要約の後 = 「流れ → 今 → 依頼」の順に読める）。
    const std::string summary = (digest.empty() ? std::string() : "\n\n" + digest) +
                                (history.empty() ? std::string() : "\n\n" + history);
    switch (mode) {
        case GenMode::Edit:
            return "Here is the CURRENT scene:\n\n" + currentXml + summary +
                   "\n\nModify it according to this request and return the COMPLETE updated "
                   "document. Keep everything the request does not mention exactly as it is "
                   "(including <terrain>, <vehicle>, <event>, <formula>, <prefab> and any "
                   "attributes you do not understand).\n\nREQUEST:\n" +
                   request;
        case GenMode::Append:
            return "Here is the CURRENT scene, for reference only:\n\n" + currentXml + summary +
                   "\n\nWrite a document that contains ONLY THE NEW things to add to it (new "
                   "<mesh> assets, new <body> elements and joints between new bodies). Place "
                   "them in free space so they do not intersect the existing bodies. Use new, "
                   "unique names. Lights, cameras, ground and settings are ignored in this "
                   "mode.\n\nREQUEST:\n" +
                   request;
        default:
            return (history.empty() ? std::string()
                                    : history +
                                          "\n(The new scene REPLACES the current one; use the "
                                          "history only to understand the request.)\n\n") +
                   "Create a new scene for this request:\n\n" + request;
    }
}

const char* const kNoChanges = "NO_CHANGES";

std::string buildReviewPrompt(const std::string& request, const std::string& currentXml,
                              const std::vector<std::string>& views, const std::string& digest) {
    std::string s =
        "VISUAL CHECK. Your document has been applied to the engine. The attached images are "
        "LOW-RESOLUTION renders of the scene as it is now (editor mode: nothing has moved yet, "
        "physics has not started). Images, in order:\n";
    for (std::size_t i = 0; i < views.size(); ++i) {
        s += "  " + std::to_string(i + 1) + ". " + views[i] + "\n";
    }
    s += "\nThe original request was:\n" + request +
         "\n\nThe complete current scene document is:\n\n" + currentXml +
         (digest.empty() ? std::string() : "\n\n" + digest) +
         "\n\nCompare the images with the request. Look for: missing or extra objects, wrong "
         "sizes or proportions, objects floating in the air or sunk into the floor or into each "
         "other, wrong colours or materials, objects outside the editor camera view, and a scene "
         "that is too dark or too bright. Small details do not matter at this resolution.\n"
         "- If the scene already matches the request well, reply with exactly " +
         std::string(kNoChanges) +
         " and nothing else.\n"
         "- Otherwise reply with the COMPLETE corrected document (same rules as before). Change "
         "only what is needed and keep every other element and attribute exactly as it is "
         "(including <terrain>, <vehicle>, <event>, <formula>, <prefab> and attributes you do "
         "not understand).";
    return s;
}

bool replySaysNoChanges(const std::string& reply) {
    return reply.find("<wizengine") == std::string::npos &&
           reply.find(kNoChanges) != std::string::npos;
}

bool extractSceneXml(const std::string& reply, std::string& xml, std::string& error) {
    const std::string closeTag = "</wizengine>";
    const auto end = reply.rfind(closeTag);
    if (end == std::string::npos) {
        // 自己閉じの <wizengine .../> だけ、は中身が無いので文書として扱わない。
        error = reply.find("<wizengine") == std::string::npos
                    ? "the reply contains no <wizengine> document"
                    : "the <wizengine> document is not closed (the reply may be truncated)";
        return false;
    }
    const auto start = reply.rfind("<wizengine", end);
    if (start == std::string::npos) {
        error = "found </wizengine> without an opening <wizengine>";
        return false;
    }
    xml = reply.substr(start, end + closeTag.size() - start);
    return true;
}

std::string repairPrompt(const std::string& problem, const std::vector<std::string>& items) {
    std::string s = problem + "\n";
    const std::size_t n = std::min<std::size_t>(items.size(), 30);
    for (std::size_t i = 0; i < n; ++i) s += "- " + items[i] + "\n";
    if (items.size() > n) s += "- ... and " + std::to_string(items.size() - n) + " more\n";
    s += "Fix exactly these problems and keep everything else unchanged. "
         "Return the complete corrected XML document only.";
    return s;
}

}  // namespace autogen
}  // namespace wizengine
