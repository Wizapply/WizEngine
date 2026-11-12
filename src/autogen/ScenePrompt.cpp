#include "autogen/ScenePrompt.h"

#include <algorithm>

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

std::string buildSystemPrompt(const PromptOptions& opt) {
    std::string s = R"(You build 3D physics scenes for the Charon engine (Chrono physics + Filament PBR renderer).
You answer with exactly ONE XML document whose root element is <wizengine>. No prose, no markdown.

WORLD CONVENTIONS
- Units are meters, kilograms, seconds. Y is up, gravity is -Y. The default floor is the plane y = 0.
- All angles are in degrees. euler="x y z" is applied as R = Rz * Ry * Rx.
- A body's pos is its CENTER. Something resting on the floor has pos y = half its height.
- Colors are rgba="r g b 1" with LINEAR values 0..1.
- Do not let solid bodies intersect each other or the floor at the start: leave 1-2 cm gaps.
  Stack things by putting each one's bottom on the top of the one below.
- Keep everything within x, z in [-9, 9] (the floor is 20 m wide) unless you also enlarge <ground size>.

DOCUMENT FORMAT (MuJoCo MJCF-like; only these elements and attributes)
<wizengine model="short_name" version="4">
  <option gravity="0 -9.81 0"/>                                (optional)
  <visual>                                                     (optional)
    <quality shadow="pcss" msaa="4"/>
    <postprocess ssao="true" bloom="0.05" vignette="0.2"/>
  </visual>
  <asset>
    <!-- glTF models (see MODELS below) -->
  </asset>
  <worldbody>
    <ground size="10" visual="8" rgba="0.8 0.8 0.8 1" roughness="0.9"/>
    <light name="sun" type="sun" pos="0 8 0" euler="50 -35 0" diffuse="1 0.97 0.92 1" intensity="100000" castshadow="true"/>
    <light name="fill" type="point" pos="-3 4 3" diffuse="0.8 0.87 1 1" intensity="40000" falloff="20" castshadow="false"/>
    <camera name="cam0" target="0 0.8 0" azimuth="35" elevation="22" radius="9"/>
    <body name="crate" pos="0 0.25 0" euler="0 20 0">
      <geom type="box" size="0.25 0.25 0.25" mass="8" rgba="0.55 0.35 0.18 1" roughness="0.8"/>
    </body>
    <body name="ball" pos="1 0.15 0">
      <geom type="sphere" size="0.15" mass="0.5" rgba="0.8 0.1 0.1 1" roughness="0.3"/>
    </body>
    <body name="table_top" pos="0 0.75 2" fixed="true">
      <geom type="box" size="0.8 0.03 0.5" mass="20" rgba="0.4 0.25 0.12 1"/>
    </body>
  </worldbody>
  <equality>
    <joint name="door_hinge" type="hinge" body1="door" body2="world" anchor="1 1 0" axis="0 1 0"/>
  </equality>
</wizengine>

ELEMENT NOTES
- <geom size> is HALF the dimension: box = three half-extents, sphere = radius.
- geom attributes you may use: type (box|sphere|mesh), size, mass (kg, realistic), rgba,
  roughness (0..1), metallic (0 or 1), clearcoat (0..1), emissive (0..), friction (0..2), restitution (0..1).
- fixed="true" on <body> makes it immovable (walls, buildings, shelves, heavy furniture you do not want to tip).
- Every body needs a unique name (letters, digits, _ and - only).
- Lights: type sun | point | spot. euler "0 0 0" points STRAIGHT DOWN; tilt with x, turn with y.
  Intensity: sun 60000-110000 (lux), point/spot 20000-150000 (lumen). Spot also takes inner and cutoff (deg).
  Use 1-3 lights; one sun with castshadow="true" is usually best.
- <camera name="cam0"> is the editor camera: aim target at the middle of the scene, choose radius so the
  whole scene is visible (about 1.5x the scene width), elevation 15-35.
- Joints (optional): type weld | hinge | ball | slide | distance | spring. body1/body2 are body names or "world".
  anchor is a world point, axis a world direction. hinge/slide take range="lo hi" and
  motor="speed" target="..." (deg/s or m/s). Do not overlap jointed bodies.
- Do NOT write vehicles, terrain, soft bodies, cables, prefabs or event nodes.
)";
    if (opt.meshGeneration) {
        s += R"(
MODELS (text-to-3D generation is AVAILABLE)
- For an object that boxes and spheres cannot represent well (furniture, animals, props, statues, vehicles as
  static props), declare a model to be generated and use it from a geom:
    <asset>
      <mesh name="chair" generate="a simple wooden dining chair, four legs, no background" height="0.9"/>
    </asset>
    <body name="chair1" pos="1 0.45 0" euler="0 90 0">
      <geom type="mesh" mesh="chair" size="0.35" mass="6"/>
    </body>
- generate = an English description of ONE isolated object (shape, material, style). No scene, no ground.
- height = the real-world height in meters; the engine scales the model so that its height matches exactly.
  The body's pos y for an object on the floor is therefore height/2. geom size = a rough radius (fallback only).
- One <mesh> can be used by many bodies. Use at most )" +
             std::to_string(opt.maxMeshes) + R"( NEW generated meshes; build the rest from boxes and spheres.
  Generation is slow and costs money: prefer primitives for walls, floors, tables tops, shelves, simple blocks.
- Collision of a model is its CONVEX HULL: nothing can sit inside a hollow model (cups, boxes, cars, shelves).
  Large models (houses, trees, statues) should be fixed="true".
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
    s += R"(
OUTPUT
- Reply with the XML document only. Start with <wizengine and end with </wizengine>.
)";
    return s;
}

std::string buildUserPrompt(GenMode mode, const std::string& request,
                            const std::string& currentXml) {
    switch (mode) {
        case GenMode::Edit:
            return "Here is the CURRENT scene:\n\n" + currentXml +
                   "\n\nModify it according to this request and return the COMPLETE updated "
                   "document. Keep everything the request does not mention exactly as it is "
                   "(including <terrain>, <vehicle>, <event>, <formula>, <prefab> and any "
                   "attributes you do not understand).\n\nREQUEST:\n" +
                   request;
        case GenMode::Append:
            return "Here is the CURRENT scene, for reference only:\n\n" + currentXml +
                   "\n\nWrite a document that contains ONLY THE NEW things to add to it (new "
                   "<mesh> assets, new <body> elements and joints between new bodies). Place "
                   "them in free space so they do not intersect the existing bodies. Use new, "
                   "unique names. Lights, cameras, ground and settings are ignored in this "
                   "mode.\n\nREQUEST:\n" +
                   request;
        default:
            return "Create a new scene for this request:\n\n" + request;
    }
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
    s += "Return the complete corrected XML document only.";
    return s;
}

}  // namespace autogen
}  // namespace wizengine
