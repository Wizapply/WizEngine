#pragma once

// Every scene parameter in one place: object grid and materials, cameras,
// lights, models, grab feel, streaming, physics solver. scene.cpp reads
// these; nothing else includes this header. The section markers below are
// unchanged from when this block lived at the top of scene.cpp.
//
// Everything here is a compile-time value or a small inline factory - editing
// a number and rebuilding is the whole workflow. Runtime-tunable values
// (solver iterations, substeps, ...) start from these and are then driven
// from the browser via PhysicsControlComponent.

#include <vector>

#include <math/vec3.h>

#include "BoxController.h"
#include "CameraObject.h"
#include "LightObject.h"
#include "PhysicsWorld.h"    // PhysicsBackend
#include "scene_math.h"      // kPi, used by the volume calculation

// ---- Scene parameters (everything lives here) ---------------------------
constexpr int kNx = 8;                // boxes along X
constexpr int kNy = 6;                // boxes along Y (stacked height)
constexpr int kNz = 8;                // boxes along Z   -> 8*8*8 = 512
// Collision shape of the dynamic objects. Sphere suits round models (an
// apple); Box suits crates. kBoxSize is the box edge OR the sphere diameter,
// so the object occupies the same space either way. ConvexHull collides as
// the convex hull of kBoxModelPath's mesh (kBoxModelScale applied), so the
// physics silhouette matches the drawn model - requires kBoxModelPath, and
// falls back to Box (with a warning) when the mesh cannot be read. Author
// hull models with the origin near their centre: Chrono re-centres the hull
// on its barycentre, and a far-off origin renders offset from the collision.
enum class BodyShape { Box, Sphere, ConvexHull };
constexpr BodyShape kBodyShape = BodyShape::Sphere;
// Rolling/spinning resistance - only meaningful for spheres, which otherwise
// roll across a flat floor forever. Raise if the fruit never settles.
// (Chrono only solves these when the solver runs in spinning mode, which
// PhysicsWorld enables for the multicore backend.)
constexpr float kRollingFriction = 0.005f;
constexpr float kSpinningFriction = 0.005f;

// Contact surface. Friction is the main dial for HOW MUCH SPIN appears: it is
// what turns sliding into rolling, so a higher value spins objects up faster
// on landing. Restitution is bounciness; keep it near 0 for stacks that settle.
constexpr float kFriction = 0.6f;
constexpr float kRestitution = 0.0f;

// Velocity damping, a stand-in for air resistance: velocity decays by
// exp(-k dt) each step, so k is "how many e-foldings per second". Angular
// damping is what actually stops a sphere that is rolling on the spot; raise
// both if things drift too long, lower them for a slippery, lively scene.
constexpr double kLinearDamping = 0.15;   // 1/s
constexpr double kAngularDamping = 0.60;  // 1/s

constexpr double kBoxSize = 0.1;      // box edge length / sphere diameter
constexpr double kSpacing = 0.15;     // centre-to-centre gap (> kBoxSize)
constexpr double kBaseY = 1.0;        // height of the lowest layer
// Mass per object, converted to the density Chrono wants. Doing it this way
// keeps the mass fixed when the shape or size changes: a sphere of diameter
// kBoxSize has a bit over half the volume of the cube it fits in, so the same
// density would give a much lighter fruit.
constexpr double kMassPerBody = 0.2;  // kg
// (ConvexHull uses the box volume as its estimate here; the actual mass is
// density x the real hull volume, so it lands near kMassPerBody for a model
// that roughly fills its kBoxSize cube.)
constexpr double kBodyVolume =
    (kBodyShape == BodyShape::Sphere)
        ? (4.0 / 3.0) * scenemath::kPi * (kBoxSize * 0.5) * (kBoxSize * 0.5) *
              (kBoxSize * 0.5)
        : kBoxSize * kBoxSize * kBoxSize;
constexpr double kDensity = kMassPerBody / kBodyVolume;  // kg/m^3
// Box colour (linear RGB). Shading comes from the lights, so keep it a plain
// base colour rather than a pre-shaded one.
constexpr float kBoxR = 0.80f, kBoxG = 0.36f, kBoxB = 0.18f;

constexpr double kGroundSize = 20.0;  // physics ground box (X and Z)
constexpr float kGroundHalf = 8.0f;   // visible ground half-extent (16 x 16)
// Ground texture: image under assets/ (copied next to the executable by the
// build). Set to "" to use the generated checkerboard.
constexpr const char* kGroundTexture = "textures/ground.png";
constexpr float kGroundTile = 2.0f;   // metres covered by one texture repeat
// Tint multiplied onto the texture (white = image colours as-is).
constexpr float kGroundTintR = 1.0f, kGroundTintG = 1.0f, kGroundTintB = 1.0f;

// --- Performance knobs ---------------------------------------------------
// Substeps multiply the physics cost directly: 1 is cheapest, 2-4 settles
// stacks better. Solver iterations are the other big cost; lower them when
// running many boxes (jitter comes back, so tune against the perf overlay).
// ---- Environment lighting -------------------------------------------------
// The HDR panorama that lights the scene. Put a Radiance .hdr file in
// assets/ and name it here; it is converted into a cubemap on the GPU at load
// time, so swapping the file only needs a restart, not a rebuild. Empty =
// flat ambient light only.
//
// Worth setting whenever glTF models are used: glTF defaults metallicFactor to
// 1.0, and metal has no diffuse colour - with nothing to reflect it renders
// black in shadow. An environment gives it something to reflect.
constexpr const char* kEnvironmentHdr = "studio.hdr";  // e.g. "studio.hdr"
constexpr float kEnvironmentIntensity = 30000.0f;  // as the flat ambient was

// ---- Cameras -------------------------------------------------------------
// One entry per camera. Each gets its own browser page on its own port
// (kHttpPort + index, see main.cpp), its own Filament view and its own video
// stream, all looking at the same scene. Add or remove entries freely; the
// first one is the default.
inline std::vector<CameraObject::Config> cameraConfigs() {
    CameraObject::Config a;  // default three-quarter view
    a.azimuth = 0.66;
    a.elevation = 0.34;
    a.radius = 12.0;
    a.targetX = 0.0;
    a.targetY = 1.0;
    a.targetZ = 0.0;

    CameraObject::Config b = a;  // opposite side, lower
    b.azimuth = 0.66 + scenemath::kPi;  // opposite side
    b.elevation = 0.18;
    b.radius = 8.0;

    CameraObject::Config c = a;  // near top-down
    c.elevation = 1.15;
    c.radius = 10.0;

    return {a, b, c};
}

// ---- Lights --------------------------------------------------------------
// One entry per light; add or remove entries freely. These are the direct
// lights of the scene (the ambient/IBL is separate - see kEnvironmentHdr).
// Each becomes a LightObject reachable as scene.light(i), so position,
// direction, colour and intensity can also be changed at runtime, e.g. from a
// SceneComponent:  scene.light(0).setDirection({1, -1, 0});
//
// Intensity units: lux for Directional (sun ~100k, overcast ~10k), lumens for
// Point/Spot (a 60W-ish bulb ~800 lm - point lights need surprisingly large
// values to compete with a sun-lit scene).
inline std::vector<LightObject::Config> lightConfigs() {
    LightObject::Config key;  // warm main light, casts the shadows
    key.type = LightObject::Type::Directional;
    key.color = {1.0f, 0.97f, 0.92f};
    key.intensity = 70000.0f;
    key.direction = {-0.5f, -1.0f, -0.35f};
    key.castShadows = true;

    LightObject::Config fill;  // cool light from the other side, no shadows.
    fill.type = LightObject::Type::Directional;  // Lifts shadowed ground out
    fill.color = {0.85f, 0.90f, 1.0f};           // of pure black.
    fill.intensity = 40000.0f;
    fill.direction = {0.6f, -0.5f, 0.45f};
    fill.castShadows = false;

    // Example of a positioned light - uncomment to try:
    // LightObject::Config lamp;
    // lamp.type = LightObject::Type::Point;
    // lamp.color = {1.0f, 0.6f, 0.3f};
    // lamp.intensity = 500000.0f;  // lumens
    // lamp.position = {0.0f, 4.0f, 0.0f};
    // lamp.falloffRadius = 25.0f;
    // return {key, fill, lamp};

    return {key, fill};
}

// ---- Models --------------------------------------------------------------
// Optional glTF / GLB model loaded at startup, looked up next to the .filamat
// files (i.e. the build dir) unless an absolute path is given. Leave empty for
// none. The model is decoration only - it has no collision shape.
constexpr const char* kModelPath = "";  // e.g. "model.glb"

// Use a glTF/GLB model for the dynamic objects instead of the built-in cube.
// Empty = plain boxes. The physics shape stays a kBoxSize cube either way.
// If the file cannot be loaded the scene falls back to boxes.
constexpr const char* kBoxModelPath = "apple2.glb";  // e.g. "crate.glb"
// Straight multiplier applied to the model's own units. The console prints the
// model's measured size at startup, so pick a value from that: to make a model
// that measures 0.01 span 0.3 m, use 30. Nothing is fitted automatically.
constexpr float kBoxModelScale = 1.0f;
// How strongly the grabbed object is washed towards white so it stands out.
// 0 = no change, 1 = strongly whitened. Works with opaque models.
constexpr float kSelectedWhiten = 0.7f;
constexpr float kModelX = 0.0f, kModelY = 0.0f, kModelZ = 0.0f;
constexpr float kModelYawDegrees = 0.0f;
constexpr float kModelScale = 1.0f;
// Give the model above a static triangle-mesh collision at the same pose, so
// dynamic objects pile against it instead of passing through. Exact but for
// FIXED geometry only. Read failures degrade to "decoration only" with a
// warning rather than aborting the run.
constexpr bool kModelCollision = false;
// Collision proxy: a separate low-poly mesh used ONLY for collision, while
// kModelPath stays the pretty one on screen. Empty = collide with kModelPath
// itself. Strongly recommended for dense models: the multicore collision
// system treats every triangle as its own shape, so a high-poly collision
// mesh stalls the physics thread (hundreds of ms per step reads as a freeze).
// A few hundred to a few thousand triangles is the healthy range - in
// Blender: duplicate, Decimate modifier, export. Same pose/scale is applied.
constexpr const char* kModelCollisionPath = "";  // e.g. "model_collision.glb"

// ---- Grab control --------------------------------------------------------
// Grab an object with the left mouse button and drag to push it in the
// camera's screen plane.
inline BoxController::Config boxControllerConfig() {
    BoxController::Config c;
    c.stiffness = 80.0;         // pull towards the mouse target (1/s^2)
    c.damping = 12.0;           // ~2*sqrt(stiffness) = no overshoot
    c.maxAcceleration = 60.0;   // m/s^2 cap (gravity is 9.81)
    return c;
}

// Highlight colour per camera (linear RGB). Reused cyclically when there are
// more cameras than entries.
inline const std::vector<filament::math::float3>& cameraColors() {
    static const std::vector<filament::math::float3> colors = {
        {0.20f, 0.65f, 0.95f},  // camera 0: blue
        {0.30f, 0.85f, 0.35f},  // camera 1: green
        {0.95f, 0.55f, 0.20f},  // camera 2: orange
        {0.85f, 0.30f, 0.75f},  // camera 3: magenta
    };
    return colors;
}

// ---- Streaming ----------------------------------------------------------
// Codec, bitrate and GPU colour conversion are ENGINE settings, not scene
// content: they live in main.cpp (kDefaultCodec & friends) and can be
// overridden at launch with --codec / --encoder, and per camera from the
// browser's System > Stream section.

// Physics backend. Multicore parallelises the solver and collision detection
// (much faster with thousands of bodies) but does not support sleeping. It
// needs a Chrono built with the MULTICORE module and CMake -DWIZ_USE_MULTICORE=ON;
// without that this falls back to Core automatically.
constexpr PhysicsBackend kBackend = PhysicsBackend::Multicore;

// In web mode, do nothing at all while no browser holds the viewer session:
// no physics steps, no rendering, no encoding. Set false to keep simulating in
// the background.
constexpr bool kIdleWhenUnwatched = true;

// Physics rate, independent of the 60 fps render loop. 30 Hz halves the
// physics cost; the renderer simply draws the latest pose twice.
constexpr int kPhysicsHz = 30;

constexpr int kSubsteps = 2;
constexpr int kSolverIterations = 60;

// Contact tolerances. Chrono's default envelope (0.03 m) is huge next to a
// 0.5 m box and creates far more contacts than necessary; shrinking it is one
// of the cheapest ways to speed up big scenes.
constexpr double kCollisionEnvelope = 0.002;  // metres
constexpr double kCollisionMargin = 0.002;    // metres

// Interpenetration control. Raise kContactRecovery if boxes visibly sink into
// each other (too high makes settled stacks pop); lower kSolverTolerance to
// make the solver converge harder at the cost of speed.
constexpr double kContactRecovery = 0.2;   // m/s
constexpr double kSolverTolerance = 1e-3;
// Chrono's per-body sleeping (ChBody::SetSleepingAllowed + thresholds).
// Settled bodies leave the solver entirely, so a resting pile costs almost
// nothing. Limits are deliberately tighter than
// Chrono's defaults (0.6 s / 0.1 m/s / 0.04 rad/s): loose limits let a box that
// slows down mid-fall sleep and hang in the air once its support moves away.
// Set kSleepingEnabled false if that ever shows up.
constexpr bool kSleepingEnabled = true;
constexpr float kSleepSeconds = 1.0f;
constexpr float kSleepMinLinVel = 0.02f;  // m/s
constexpr float kSleepMinAngVel = 0.02f;  // rad/s
