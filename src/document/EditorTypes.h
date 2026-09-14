#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "vehicle/VehicleTypes.h"

// エディタモードが編集する「シーン文書」の型。
//
// ここにあるのは値だけ（Chrono も Filament も出てこない）ので、物理・描画・
// HTTP のどのスレッドからも安全にコピーできる。実体の生成は Scene が行い、
// この記述子はその「設計図」にあたる:
//
//   BodyDesc  … 1個の剛体（形・大きさ・置いた場所・質量・色）
//   JointDesc … 2個の剛体をつなぐ拘束
//   SimSettings … シミュレート側の設定（重力・摩擦・レート等）
//
// 保存/読み込みは全部この型の JSON 化で済ませる（assets/scenes/*.json）。
namespace wizengine {
namespace editor {

// エディタ（配置・設計）とシミュレート（実行）の2モード。
enum class AppMode { Editor, Simulate };

inline const char* modeName(AppMode m) {
    return m == AppMode::Editor ? "editor" : "simulate";
}
inline AppMode modeFromName(const std::string& s, AppMode fallback) {
    if (s == "editor") return AppMode::Editor;
    if (s == "simulate" || s == "sim" || s == "play") return AppMode::Simulate;
    return fallback;
}

// 剛体の形。Model は glTF モデル（シーン文書の <asset> 節で相対パスを宣言し、
// BodyDesc::mesh が名前で参照する）。Box / Sphere は組み込みメッシュ。
// None = geom を持たない（MJCF の geom 無し body）。見た目も当たり判定も
// 付いているプレハブの部品だけで、ボディ自身は「部品の集合の原点 + 質量」の
// フレーム。階段（全体が収まる箱の中心を原点にした部品の集合）が使う。
// Trimesh は**当たり判定だけ**の値（<geom type="mesh" collision="trimesh">）:
// glTF の三角形をそのまま Chrono の静的メッシュ（Bullet の三角メッシュ）に
// する。凸包では表せない凹形状（器・トンネル・地形）に使う。見た目の
// shape には使えない（clampBody が Model へ倒す）。動く物には重いので
// 固定の物に向く。
enum class ShapeKind { Box, Sphere, Model, None, Trimesh };

inline const char* shapeName(ShapeKind s) {
    switch (s) {
        case ShapeKind::Sphere: return "sphere";
        case ShapeKind::Model: return "model";
        case ShapeKind::None: return "none";
        case ShapeKind::Trimesh: return "trimesh";
        case ShapeKind::Box: break;
    }
    return "box";
}
inline ShapeKind shapeFromName(const std::string& s, ShapeKind fallback) {
    if (s == "trimesh") return ShapeKind::Trimesh;
    if (s == "box") return ShapeKind::Box;
    if (s == "sphere") return ShapeKind::Sphere;
    if (s == "model") return ShapeKind::Model;
    if (s == "none") return ShapeKind::None;
    return fallback;
}

// 拘束の種類。Chrono の対応クラスは PhysicsWorld::addJoint を参照。
//   Fixed / Revolute / Spherical / Prismatic / Distance … 従来の 5 種
//   Universal   … 自在継手（十字軸。axis = シャフトの向き）
//   Cylindrical … 軸まわりの回転 + 軸方向のスライド
//   Planar      … 平面上の移動と面内の回転（axis = 面の法線）
//   PointLine   … 点を線に載せる（axis = 線の向き。回転は自由）
//   PointPlane  … 点を面に載せる（axis = 面の法線。回転は自由）
//   Gear        … 2 本のシャフトを歯車で結ぶ（ratio。anchor2 / axis2 が 2 本目）
//   Screw       … ねじ（axis まわりの回転が pitch [m/回転] の前進になる）
//   Spring      … 2 点間のばね・ダンパ（拘束ではなく力。stiffness / damping）
//   Bushing     … ゴムブッシュ（ChLoadBodyBodyBushingMate。並進 3 軸の
//                  stiffness / damping と回転 3 軸の rotStiffness / rotDamping。
//                  拘束ではなく荷重 = ChLoad なので Core バックエンド専用）
enum class JointKind {
    Fixed, Revolute, Spherical, Prismatic, Distance,
    Universal, Cylindrical, Planar, PointLine, PointPlane, Gear, Screw, Spring,
    Bushing
};

inline const char* jointName(JointKind k) {
    switch (k) {
        case JointKind::Fixed: return "fixed";
        case JointKind::Spherical: return "spherical";
        case JointKind::Prismatic: return "prismatic";
        case JointKind::Distance: return "distance";
        case JointKind::Universal: return "universal";
        case JointKind::Cylindrical: return "cylindrical";
        case JointKind::Planar: return "planar";
        case JointKind::PointLine: return "pointLine";
        case JointKind::PointPlane: return "pointPlane";
        case JointKind::Gear: return "gear";
        case JointKind::Screw: return "screw";
        case JointKind::Spring: return "spring";
        case JointKind::Bushing: return "bushing";
        case JointKind::Revolute: break;
    }
    return "revolute";
}
inline JointKind jointFromName(const std::string& s, JointKind fallback) {
    if (s == "fixed") return JointKind::Fixed;
    if (s == "revolute" || s == "hinge") return JointKind::Revolute;
    if (s == "spherical" || s == "ball") return JointKind::Spherical;
    if (s == "prismatic" || s == "slider") return JointKind::Prismatic;
    if (s == "distance" || s == "rod") return JointKind::Distance;
    if (s == "universal") return JointKind::Universal;
    if (s == "cylindrical") return JointKind::Cylindrical;
    if (s == "planar" || s == "plane") return JointKind::Planar;
    if (s == "pointLine" || s == "pointline") return JointKind::PointLine;
    if (s == "pointPlane" || s == "pointplane") return JointKind::PointPlane;
    if (s == "gear") return JointKind::Gear;
    if (s == "screw") return JointKind::Screw;
    if (s == "spring") return JointKind::Spring;
    if (s == "bushing") return JointKind::Bushing;
    return fallback;
}

// ジョイントの駆動（モータ）。Revolute と Prismatic にだけ付く。Chrono の
// ChLinkMotorRotation* / ChLinkMotorLinear* で、拘束ごと置き換わる
// （リミットは付かない = 速度・角度モータはトルク無制限の拘束型）。
//   Speed    … 目標の角速度 (deg/s) / 速度 (m/s)
//   Position … 目標の角度 (deg) / 位置 (m)（開始姿勢からの相対）
//   Force    … 一定のトルク (N·m) / 力 (N)
enum class MotorMode { None, Speed, Position, Force };

inline const char* motorModeName(MotorMode m) {
    switch (m) {
        case MotorMode::Speed: return "speed";
        case MotorMode::Position: return "position";
        case MotorMode::Force: return "force";
        case MotorMode::None: break;
    }
    return "none";
}
inline MotorMode motorModeFromName(const std::string& s, MotorMode fallback) {
    if (s == "none" || s.empty()) return MotorMode::None;
    if (s == "speed" || s == "velocity") return MotorMode::Speed;
    if (s == "position" || s == "angle") return MotorMode::Position;
    if (s == "force" || s == "torque") return MotorMode::Force;
    return fallback;
}

// 選択中のオブジェクトに出すギズモ（Unity の W / E / R に相当）。
enum class GizmoMode { Translate, Rotate, Scale };
// 軸の向きをワールドに合わせるか、オブジェクト自身の向きに合わせるか。
enum class GizmoSpace { World, Local };

inline const char* gizmoModeName(GizmoMode m) {
    switch (m) {
        case GizmoMode::Rotate: return "rotate";
        case GizmoMode::Scale: return "scale";
        case GizmoMode::Translate: break;
    }
    return "translate";
}
inline GizmoMode gizmoModeFromName(const std::string& s, GizmoMode fallback) {
    if (s == "translate" || s == "move") return GizmoMode::Translate;
    if (s == "rotate") return GizmoMode::Rotate;
    if (s == "scale") return GizmoMode::Scale;
    return fallback;
}
inline const char* gizmoSpaceName(GizmoSpace s) {
    return s == GizmoSpace::Local ? "local" : "world";
}
inline GizmoSpace gizmoSpaceFromName(const std::string& s, GizmoSpace fallback) {
    if (s == "world" || s == "global") return GizmoSpace::World;
    if (s == "local" || s == "self") return GizmoSpace::Local;
    return fallback;
}

// ギズモの設定。ブラウザの Inspector タブから変える。
struct GizmoSettings {
    GizmoMode mode = GizmoMode::Translate;
    GizmoSpace space = GizmoSpace::World;
    bool snap = false;
    double moveStep = 0.25;   // m
    double rotateStep = 15.0; // 度
    double scaleStep = 0.1;   // 倍率ではなく寸法の刻み (m)
    // Y=0 の格子グリッド（エディタ中の置き場の目印）。
    bool grid = true;
    double gridStep = 1.0;    // 格子の間隔 (m)
};

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct Color3 {
    float r = 0.80f, g = 0.36f, b = 0.18f;
};

// glTF モデルのアセット宣言（シーン文書の <asset><mesh .../>）。file は
// assets/ からの相対パス（".." や絶対パスは読み込みで弾く）。scale はモデル
// 単位からメートルへの素の倍率で、当たり判定の寸法（BodyDesc::size）とは
// 独立 - 見た目はアーティストの出力そのまま、当たりはエディタで決める。
struct MeshAssetDesc {
    std::string name;
    std::string file;
    double scale = 1.0;
};

// 1個の剛体の設計値。position/rotation は「エディタで置いた姿勢」で、
// シミュレートを止めるとここに戻る（＝オーサリング状態は壊れない）。
// ---- プレハブ（見た目の部品の階層）------------------------------------------
// Unity の prefab に相当する「部品の集合」。部品は物理ボディではなく、付け先の
// オブジェクト（車体）に固定されて動く見た目の子。socket を書いた部品は
// 車両が動かす（"wheel:<軸>:<L|R>" = その車輪の姿勢に付いていく）。
// 文書では <asset> の <prefab name> と、<body> の <prefab name/>（付け先）。
// collide を立てた部品（箱 / 球、車体に固定のものだけ）は、付け先のボディの
// 当たり形状に**足される**（Chrono の複合形状 = 1 ボディに複数の形）。
// 質量・慣性は元の <geom> のまま。階段のように「固定の箱を並べた物」を
// 1 つのオブジェクトにする用途（builtinStairsPrefab）。
enum class PartKind { Box, Sphere, Cylinder, Mesh };

inline const char* partKindName(PartKind k) {
    switch (k) {
        case PartKind::Sphere: return "sphere";
        case PartKind::Cylinder: return "cylinder";
        case PartKind::Mesh: return "mesh";
        case PartKind::Box: break;
    }
    return "box";
}
inline PartKind partKindFromName(const std::string& s, PartKind fallback) {
    if (s == "box") return PartKind::Box;
    if (s == "sphere") return PartKind::Sphere;
    if (s == "cylinder") return PartKind::Cylinder;
    if (s == "mesh" || s == "model") return PartKind::Mesh;
    return fallback;
}

// ---- 材質（フォトリアル描画のための PBR パラメータ）--------------------------
// 色（baseColor）が「何色か」なら、ここにあるのは「どう光るか」。値は
// Filament の lit マテリアルのパラメータそのままで、文書では <geom> /
// <part> の属性（rgba の隣）に書く。既定値は従来の見た目（roughness 0.75 の
// 誘電体）と同じなので、書いていない文書の見た目は変わらない。
//
//   roughness   0 = 鏡のようにくっきり映る、1 = つや消し。金属でも誘電体でも
//               「反射のぼけ具合」を決める、いちばん効く値
//   metallic    0 = 誘電体（プラ・木・石・塗装）、1 = 金属（鉄・アルミ）。
//               中間の値に物理的な意味は無い（層構造の近似としてのみ）
//   reflectance 誘電体の垂直反射率。0.5 = 4%（ほとんどの物質）、水 0.35、
//               宝石 0.7 くらい。metallic=1 では使われない
//   clearCoat   透明な上塗りの層（車の塗装・ニス・濡れた表面）。0 = 無し
//   emissive    自己発光（baseColor の何倍を足すか）。ブルームの光源になる
struct MaterialDesc {
    double roughness = 0.75;
    double metallic = 0.0;
    double reflectance = 0.5;
    double clearCoat = 0.0;
    double clearCoatRoughness = 0.03;
    double emissive = 0.0;
};

inline MaterialDesc clampMaterial(MaterialDesc m) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    // roughness の下限が 0 でないのは Filament と同じ理由: 完全な 0 は
    // ハイライトが 1 ピクセルになって派手にちらつく。
    m.roughness = cl(m.roughness, 0.02, 1.0);
    m.metallic = cl(m.metallic, 0.0, 1.0);
    m.reflectance = cl(m.reflectance, 0.0, 1.0);
    m.clearCoat = cl(m.clearCoat, 0.0, 1.0);
    m.clearCoatRoughness = cl(m.clearCoatRoughness, 0.0, 1.0);
    m.emissive = cl(m.emissive, 0.0, 100.0);
    return m;
}

inline bool operator==(const MaterialDesc& a, const MaterialDesc& b) {
    return a.roughness == b.roughness && a.metallic == b.metallic &&
           a.reflectance == b.reflectance && a.clearCoat == b.clearCoat &&
           a.clearCoatRoughness == b.clearCoatRoughness &&
           a.emissive == b.emissive;
}
inline bool operator!=(const MaterialDesc& a, const MaterialDesc& b) {
    return !(a == b);
}

struct PartDesc {
    std::string name;
    PartKind kind = PartKind::Box;
    std::string mesh;             // Mesh のとき: <asset> の <mesh> の名前
    Vec3d position{0.0, 0.0, 0.0};  // 親（車体 / ソケット）ローカル
    Vec3d rotation{0.0, 0.0, 0.0};  // オイラー角（度）
    // Box: 各辺の長さ。Sphere: 直径（x）。Cylinder: 長さ（x、軸は X）と
    // 直径（y）。Mesh: 倍率（x。モデルの <mesh scale> にさらに掛かる）。
    Vec3d size{0.5, 0.5, 0.5};
    Color3 color;
    MaterialDesc material;        // 材質（PBR）。車の塗装（clearCoat）など
    std::string socket;           // "" = 車体に固定。"wheel:0:L" など
    // 当たり判定を持つ（付け先のボディの複合形状に足す）。箱 / 球で socket が
    // 空のものだけ有効（clampPart が他を false に落とす）。
    bool collide = false;
};

struct PrefabDesc {
    std::string name;
    std::vector<PartDesc> parts;
};

// "wheel:<軸>:<L|R>" を読む。違う書式なら false。
inline bool parseWheelSocket(const std::string& socket, int& axle, int& side) {
    if (socket.rfind("wheel:", 0) != 0) return false;
    const std::size_t sep = socket.find(':', 6);
    if (sep == std::string::npos || sep + 1 >= socket.size()) return false;
    const std::string a = socket.substr(6, sep - 6);
    if (a.empty()) return false;
    for (const char c : a) {
        if (c < '0' || c > '9') return false;
    }
    axle = std::atoi(a.c_str());
    const char sc = socket[sep + 1];
    if (sc == 'L' || sc == 'l') side = -1;
    else if (sc == 'R' || sc == 'r') side = 1;
    else return false;
    return true;
}

// ---- ソフトボディ -----------------------------------------------------------
// 質点ばね方式のソフトボディ（<body> の中の <soft> 節）。hasSoft の
// オブジェクトは剛体 1 個ではなく、形（箱 / 球）を 1 軸 resolution 個の
// 粒子（Chrono の小さな剛体）で埋め、隣どうしをばねで結んだ集合になる。
// 質量は粒子へ等分、当たり判定は粒子の球、見た目は表面粒子を結んだ
// 変形メッシュ。格子の作り方は scene/SoftLattice.h、ばねの解き方は
// PhysicsWorld::addSoftBody。
struct SoftDesc {
    int resolution = 4;          // 1 軸あたりの粒子数（2〜8。粒子は n^3 個）
    double stiffness = 4000.0;   // 硬さ = 弾性率相当 (Pa)。k = stiffness × 格子間隔
    double damping = 0.3;        // ばねの減衰比（1 = 臨界減衰）
    double shear = 1.0;          // せん断ばね（対角線）の倍率。0 で無し
    double bend = 0.5;           // 曲げばね（1 個おき）の倍率。0 で無し
    int iterations = 2;          // 1 ステップあたりのばね反復（ガウス・ザイデル）
};

// ソフトボディの常識的な範囲。粒子数は n^3 で効くので上限を低めに置く
// （8 で 512 粒子）。
inline SoftDesc clampSoft(SoftDesc s) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    if (s.resolution < 2) s.resolution = 2;
    if (s.resolution > 8) s.resolution = 8;
    s.stiffness = cl(s.stiffness, 1.0, 1.0e7);
    s.damping = cl(s.damping, 0.0, 5.0);
    s.shear = cl(s.shear, 0.0, 4.0);
    s.bend = cl(s.bend, 0.0, 4.0);
    if (s.iterations < 1) s.iterations = 1;
    if (s.iterations > 10) s.iterations = 10;
    return s;
}

inline bool operator==(const SoftDesc& a, const SoftDesc& b) {
    return a.resolution == b.resolution && a.stiffness == b.stiffness &&
           a.damping == b.damping && a.shear == b.shear && a.bend == b.bend &&
           a.iterations == b.iterations;
}
inline bool operator!=(const SoftDesc& a, const SoftDesc& b) { return !(a == b); }

// ---- 接触の物性（ボディごと）-----------------------------------------------
// 負の値は「シーンの設定（<option friction restitution>）を使う」。既定は
// 全部シーン任せなので、触っていない物のファイルには何も出ない
// （材質 MaterialDesc と同じ流儀）。cohesion（粘着、N）だけは既定 0 =
// 無し。Chrono の ChContactMaterialNSC をボディごとに 1 個持つ。
struct SurfaceDesc {
    float friction = -1.0f;     // 滑り摩擦係数（-1 = シーン設定）
    float restitution = -1.0f;  // 反発係数（-1 = シーン設定）
    float rolling = -1.0f;      // 転がり摩擦（-1 = シーン既定 kRollingFriction）
    float cohesion = 0.0f;      // 粘着力 (N)。0 = 無し
    // SMC（ペナルティ法）のときだけ効く: ヤング率 (Pa) とポアソン比
    // （-1 = シーン設定 SimSettings::young / poisson）。NSC では無視。
    float young = -1.0f;
    float poisson = -1.0f;
};
inline SurfaceDesc clampSurface(SurfaceDesc s) {
    auto cl = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    if (s.friction >= 0.0f) s.friction = cl(s.friction, 0.0f, 2.0f);
    else s.friction = -1.0f;
    if (s.restitution >= 0.0f) s.restitution = cl(s.restitution, 0.0f, 1.0f);
    else s.restitution = -1.0f;
    if (s.rolling >= 0.0f) s.rolling = cl(s.rolling, 0.0f, 1.0f);
    else s.rolling = -1.0f;
    s.cohesion = cl(s.cohesion, 0.0f, 100000.0f);
    if (s.young >= 0.0f) s.young = cl(s.young, 1000.0f, 1.0e12f);
    else s.young = -1.0f;
    if (s.poisson >= 0.0f) s.poisson = cl(s.poisson, 0.0f, 0.49f);
    else s.poisson = -1.0f;
    return s;
}
inline bool operator==(const SurfaceDesc& a, const SurfaceDesc& b) {
    return a.friction == b.friction && a.restitution == b.restitution &&
           a.rolling == b.rolling && a.cohesion == b.cohesion &&
           a.young == b.young && a.poisson == b.poisson;
}
inline bool operator!=(const SurfaceDesc& a, const SurfaceDesc& b) { return !(a == b); }
// 「どれか 1 つでもシーン設定と違う値を持つか」= 専用の接触材質が要るか。
inline bool surfaceIsCustom(const SurfaceDesc& s) {
    return s.friction >= 0.0f || s.restitution >= 0.0f || s.rolling >= 0.0f ||
           s.cohesion != 0.0f || s.young >= 0.0f || s.poisson >= 0.0f;
}

// 衝突レイヤ。ボディは 0〜7 のレイヤに属し（既定 0 = 地面もここ）、
// nocollide に挙げたレイヤの物とは当たらない（Chrono の衝突ファミリ 0〜7。
// 8〜15 はソフトボディの粒子が使う）。同士討ちしない群れ、素通りする
// 飾りなどに。Bullet は片側が拒めば当たらないので、片方に書けば足りる。
constexpr int kCollisionLayers = 8;

struct BodyDesc {
    std::string name;
    ShapeKind shape = ShapeKind::Box;
    // shape=Model のとき、どの glTF モデルで描くか（シーン文書の <asset> 節に
    // ある <mesh> の名前）。空や未知の名前は球へフォールバックする。
    std::string mesh;
    // 当たり判定の形。ふつうは shape と同じだが、既存シーンのように
    //「見た目は glTF モデル・当たりは球」という組み合わせがあるので分けて
    // 持つ。collision=Model は「モデルの凸包」の意味で、読めなければ球。
    ShapeKind collision = ShapeKind::Box;
    // Box は各辺の長さ、Sphere と Model は size.x を直径として使う。
    Vec3d size{0.5, 0.5, 0.5};
    Vec3d position{0.0, 1.0, 0.0};
    Vec3d rotation{0.0, 0.0, 0.0};  // オイラー角（度, X→Y→Z の順）
    double mass = 1.0;              // kg。密度は体積から逆算する
    bool fixed = false;             // true = 動かない土台
    Color3 color;
    // 材質（PBR）。色と違って「どう光るか」だけを決める。
    MaterialDesc material;
    // このオブジェクトに付いているイベントアセットの名前（文書では
    // <body> の中の <event name="..."/>）。順番はインスペクタの並び順で、
    // 同じ名前は 1 回だけ。番号ではなく名前で持つので、保存でオブジェクトを
    // 詰めても付け替えが要らない。
    std::vector<std::string> events;
    // 車両（<body> の中の <vehicle> 節）。hasVehicle のオブジェクトは
    // シミュレート中に VehicleComponent が「車体」として扱い、サスと
    // タイヤの力を掛ける。設計値は src/vehicle/VehicleTypes.h。
    bool hasVehicle = false;
    wizengine::vehicle::VehicleDesc vehicle;
    // 付いているプレハブ（<asset> の <prefab> の名前）。空 = 無し（車両なら
    // 組み込みのクルマの見た目、それ以外は形状そのもの）。
    std::string prefab;
    // ソフトボディ（<body> の中の <soft> 節）。hasSoft なら剛体ではなく
    // 粒子の格子として作られる（上の SoftDesc）。形と大きさ（size）は
    // 格子の外形、mass は全粒子の合計。
    bool hasSoft = false;
    SoftDesc soft;
    // 接触の物性（摩擦・反発・転がり・粘着）。既定はシーン設定に従う。
    SurfaceDesc surface;
    // 衝突レイヤ（0〜7）と、当たらないレイヤの一覧（文書では nocollide="1 3"）。
    int layer = 0;
    std::vector<int> nocollide;
    // 重力を受けるか（false = 無重力。Chrono の SetUseGravity / SetNoGravity）。
    bool gravity = true;
    // 初速。シミュレート開始（と Reset）で置いた姿勢に戻すときに与える。
    // 速度は m/s、角速度は deg/s（ワールド軸まわり）。
    Vec3d velocity{0.0, 0.0, 0.0};
    Vec3d angularVelocity{0.0, 0.0, 0.0};
    // 定常荷重（ワールド座標。力 N は重心へ、トルク N·m はそのまま）。
    // 風・推力・浮力の代わり。Chrono の ChLoadBodyForce / ChLoadBodyTorque
    // （荷重コンテナ）なので Core バックエンド専用 - Multicore で使うと
    // Scene が Core へ自動で切り替える。0 = 無し。
    Vec3d force{0.0, 0.0, 0.0};
    Vec3d torque{0.0, 0.0, 0.0};

    // 形状から体積を出す。箱・球は密度 = mass / volume を Chrono に渡すので、
    // 形や大きさを変えても質量は指定どおりに保たれる。見た目ではなく
    // **当たり判定の形**で計算する（質量は物理側の量なので）。凸包
    // （collision=Model）は密度を使わず mass をそのまま渡す（凸包の体積は
    // モデルの大きさ次第で size と無関係 = 密度から出すと桁違いになる）。
    // ここの値は凸包が読めず球へ倒れたときの見積もりにだけ効く。
    double volume() const {
        if (collision == ShapeKind::Sphere) {
            const double r = size.x * 0.5;
            return (4.0 / 3.0) * 3.14159265358979323846 * r * r * r;
        }
        const double v = size.x * size.y * size.z;
        return v > 1e-9 ? v : 1e-9;
    }
    double density() const {
        const double v = volume();
        return (mass > 0.0 && v > 1e-9) ? mass / v : 1000.0;
    }
    // 選択判定・描画で使う代表半径（Sphere/Model）と半サイズ（Box）。
    double radius() const { return size.x * 0.5; }
};

// 2つの剛体（または剛体と地面）をつなぐ拘束。anchor / axis はワールド座標で
// 持つ: エディタ上で「ここを軸に回す」と指定した位置と向きそのもの。
// bodyA / bodyB は オブジェクト番号。-1 は「地面（ワールド）」。
struct JointDesc {
    std::string name;
    JointKind kind = JointKind::Revolute;
    int bodyA = -1;
    int bodyB = -1;
    Vec3d anchor{0.0, 1.0, 0.0};
    Vec3d axis{0.0, 1.0, 0.0};
    // Distance: 保つ距離。Spring: 自然長。0 = 現在の間隔をそのまま使う。
    double distance = 0.0;
    // 可動範囲（文書では range="lo hi"。書けば limited）。Revolute /
    // Cylindrical / Universal は角度 (deg)、Prismatic は距離 (m)。他の種類と
    // モータ付きでは無視（作るときに警告）。
    bool limited = false;
    double limitLo = -45.0;
    double limitHi = 45.0;
    // 駆動（Revolute / Prismatic のみ。上の MotorMode）。target の単位は
    // モードと種類で決まる: 角速度 deg/s・角度 deg・トルク N·m、または
    // 速度 m/s・位置 m・力 N。実行中はイベントの setMotor で書き換えられる。
    MotorMode motor = MotorMode::None;
    double motorTarget = 0.0;
    // ばね・ダンパ。Spring 種類では 2 点間のばね（k N/m, c N·s/m）。Revolute に
    // 書けば回転ばね（k N·m/rad, c N·m·s/rad、自然角 = 開始姿勢）、Prismatic
    // に書けば軸方向のばね（両体の中心間、自然長 = 開始時の距離）。0 = 無し。
    double stiffness = 0.0;
    double damping = 0.0;
    // 破断: 拘束の反力（力の大きさ、N）がこれを超えたステップで外れる。
    // 0 = 切れない。外れると onJointBreak トリガーが発火する。
    double breakForce = 0.0;
    // Gear: 変速比（シャフト 2 の角速度 / シャフト 1 の角速度）。Screw:
    // 1 回転あたりの前進 (m)。
    double ratio = 1.0;
    double pitch = 0.01;
    // Gear の 2 本目のシャフト（ワールド座標）。axis2 が零ベクトルなら axis と
    // 同じ向き、anchor2 が零ベクトルなら B の中心を使う。
    Vec3d anchor2{0.0, 0.0, 0.0};
    Vec3d axis2{0.0, 0.0, 0.0};
    // Bushing: 回転 3 軸の剛性 (N·m/rad) と減衰 (N·m·s/rad)。並進は
    // stiffness / damping（N/m, N·s/m）を 3 軸に同じ値で使う。
    double rotStiffness = 0.0;
    double rotDamping = 0.0;
};

// ジョイントの値を常識的な範囲へ。リミットは lo <= hi に並べ替える。
inline JointDesc clampJoint(JointDesc j) {
    auto cl = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
    if (j.limitLo > j.limitHi) std::swap(j.limitLo, j.limitHi);
    j.limitLo = cl(j.limitLo, -100000.0, 100000.0);
    j.limitHi = cl(j.limitHi, -100000.0, 100000.0);
    j.motorTarget = cl(j.motorTarget, -1000000.0, 1000000.0);
    j.stiffness = cl(j.stiffness, 0.0, 1.0e9);
    j.damping = cl(j.damping, 0.0, 1.0e9);
    j.breakForce = cl(j.breakForce, 0.0, 1.0e9);
    j.rotStiffness = cl(j.rotStiffness, 0.0, 1.0e9);
    j.rotDamping = cl(j.rotDamping, 0.0, 1.0e9);
    if (!std::isfinite(j.ratio) || j.ratio == 0.0) j.ratio = 1.0;
    j.ratio = cl(j.ratio, -1000.0, 1000.0);
    j.pitch = cl(j.pitch, -10.0, 10.0);
    if (j.pitch == 0.0) j.pitch = 0.01;
    j.distance = cl(j.distance, 0.0, 1000.0);
    // モータは回転 / 直動にしか付かない。
    if (j.kind != JointKind::Revolute && j.kind != JointKind::Prismatic) {
        j.motor = MotorMode::None;
    }
    return j;
}

// ---- ライト -----------------------------------------------------------------
// Sun は平行光（位置は届く光に影響せず、アイコンの置き場でしかない）、Point は
// 全方向、Spot は向いた先への円錐。向きは rotation（オイラー角・度）で持ち、
// 実際の方向ベクトルは「回転ゼロ = 真下 (0,-1,0)」を回したもの
// （scenemath::lightDirection）。kind / shadows / falloff / 円錐角は Filament の
// ライト実体を決めるので、変更は実体の作り直し（rebuild）として扱う。
enum class LightKind { Sun, Point, Spot };

inline const char* lightKindName(LightKind k) {
    switch (k) {
        case LightKind::Sun: return "sun";
        case LightKind::Spot: return "spot";
        case LightKind::Point: break;
    }
    return "point";
}
inline LightKind lightKindFromName(const std::string& s, LightKind fallback) {
    if (s == "sun" || s == "directional") return LightKind::Sun;
    if (s == "point") return LightKind::Point;
    if (s == "spot") return LightKind::Spot;
    return fallback;
}

// 1 灯の設計値。intensity の単位は Sun がルクス（太陽 ~10万）、Point / Spot が
// ルーメン（60W 電球 ~800 lm。太陽下のシーンでは驚くほど大きな値が要る）。
struct LightDesc {
    std::string name;
    LightKind kind = LightKind::Point;
    Vec3d position{0.0, 3.0, 0.0};
    Vec3d rotation{0.0, 0.0, 0.0};  // オイラー角（度）。ゼロ = 真下を向く
    Color3 color{1.0f, 1.0f, 1.0f};
    double intensity = 300000.0;
    double falloff = 25.0;          // Point/Spot の届く距離 (m)
    double spotInnerDeg = 25.0;     // Spot: 全力の円錐（半頂角・度）
    double spotOuterDeg = 35.0;     // Spot: 減衰しきる円錐（半頂角・度）
    bool shadows = false;
};

// ---- 地面と環境光 -----------------------------------------------------------
// どちらもシーンの一部（文書の <worldbody> の <ground/> と <environment/>）。
// 節を書かない文書はこの既定値で開く（ライト・カメラと同じ扱い）。

// 地面。物理の床（当たり判定の箱）と見える地面（テクスチャ付きの板）は
// 半分の広さを別々に持つ - 作業の目安になる床は見えている範囲より広く
// 効いていてほしいため。texture は assets/ からの相対パス（空 = 市松模様）。
struct GroundDesc {
    double half = 10.0;        // 物理の床の半分の広さ (m)。MuJoCo と同じ半寸法
    double visualHalf = 8.0;   // 見える地面の半分の広さ (m)
    std::string texture = "textures/ground.png";
    double tile = 2.0;         // テクスチャ 1 リピートが覆うメートル
    Color3 tint{1.0f, 1.0f, 1.0f};  // テクスチャに乗す色（白 = 画像のまま）
    // 地面の材質。既定は従来どおりのつや消し。濡れたアスファルトなら
    // roughness 0.2 前後にすると、環境と物が映り込む（SSR と相性が良い）。
    double roughness = 0.9;
    double metallic = 0.0;
};

// 環境光（IBL）。assets/ の Radiance .hdr を GPU 上でキューブマップ化して
// 使う。空 = 環境マップ無し（一様な弱いアンビエントのみ）。
struct EnvironmentDesc {
    std::string hdr = "studio.hdr";
    double intensity = 30000.0;
    // 背景としても出すか。false = 従来どおり無地の背景（HDR は光としてだけ
    // 使う）。true にすると、映り込んでいる環境がそのまま背景に見える
    // ＝ 写真らしさがいちばん安く上がるスイッチ。
    bool skybox = false;
};

// ---- 描画品質（シーン文書の <visual>）---------------------------------------
// 「どう撮るか」をシーンの一部として持つ。ライトと材質が「何がどう光るか」
// なら、こちらはカメラとフィルム（露出・トーンマップ）と後処理（AA・AO・
// ブルーム・反射）の設定で、Filament の View / Camera / ColorGrading に
// そのまま渡る値。既定値は**これまでの見た目そのまま**（後処理はどれも
// 切ってあり、露出も Filament の既定と同じ f/16・1/125・ISO100）なので、
// <visual> を書いていない文書の絵は変わらない。
//
// 実際の適用は Renderer::setRenderSettings（RENDER スレッド）。ここは値の
// 入れ物で、Chrono も Filament も出てこない。
struct RenderDesc {
    // ---- <quality>: 影とアンチエイリアス
    int shadowMap = 1024;          // 影のテクスチャの一辺（512〜4096）
    int cascades = 1;              // 平行光のカスケード分割（1〜4）
    std::string shadow = "pcf";    // pcf / dpcf / pcss / vsm
    bool contactShadows = false;   // 接地部の細かい影（スクリーン空間）
    int msaa = 1;                  // 1 = 無効、2 / 4 / 8
    bool taa = false;              // テンポラル AA（静止画は綺麗、速い動きは残像）
    bool fxaa = true;              // 後処理の AA（軽い）
    // ---- <postprocess>
    bool postProcess = true;       // false = トーンマップも AA も通さない生の絵
    bool ssao = false;             // アンビエントオクルージョン（接地影・隅の陰り）
    double ssaoIntensity = 1.0;
    double bloom = 0.0;            // 0 = 無効。明るい所のにじみ（0.05〜0.2 程度）
    bool ssr = false;              // スクリーン空間反射（床の映り込み）
    double dof = 0.0;              // 被写界深度: 合焦距離 m（0 = 無効）
    double dofBlur = 1.0;          // ぼけの強さ（錯乱円の倍率）
    double vignette = 0.0;         // 0 = 無効。周辺光量落ち（0.2〜0.4 が自然）
    // ---- <exposure>: 物理カメラの露出（写真と同じ 3 つの値）
    double aperture = 16.0;        // F 値（小さいほど明るい）
    double shutter = 125.0;        // シャッター速度の分母（1/125 秒）
    double sensitivity = 100.0;    // ISO
    // ---- <grading>: フィルム（トーンマップ）と色
    std::string tonemap = "aceslegacy";  // aceslegacy / aces / filmic / agx /
                                         // pbrneutral / linear
    double contrast = 1.0;
    double saturation = 1.0;
    double temperature = 0.0;      // -1（青く）〜 +1（暖かく）
    double tint = 0.0;             // -1（緑）〜 +1（マゼンタ）
};

inline RenderDesc clampRender(RenderDesc r) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    auto cli = [](int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    // 影のテクスチャは 2 のべき乗に丸める（Filament は任意の値も受けるが、
    // 端数のサイズは意味が無いうえに気付きにくい）。
    r.shadowMap = cli(r.shadowMap, 256, 4096);
    int pow2 = 256;
    while (pow2 * 2 <= r.shadowMap) pow2 *= 2;
    r.shadowMap = pow2;
    r.cascades = cli(r.cascades, 1, 4);
    if (r.shadow != "pcf" && r.shadow != "dpcf" && r.shadow != "pcss" &&
        r.shadow != "vsm") {
        r.shadow = "pcf";
    }
    // MSAA は 1 / 2 / 4 / 8 のみ。
    if (r.msaa < 2) r.msaa = 1;
    else if (r.msaa < 4) r.msaa = 2;
    else if (r.msaa < 8) r.msaa = 4;
    else r.msaa = 8;
    r.ssaoIntensity = cl(r.ssaoIntensity, 0.0, 4.0);
    r.bloom = cl(r.bloom, 0.0, 1.0);
    r.dof = cl(r.dof, 0.0, 1000.0);
    r.dofBlur = cl(r.dofBlur, 0.0, 8.0);
    r.vignette = cl(r.vignette, 0.0, 1.0);
    r.aperture = cl(r.aperture, 0.5, 64.0);
    r.shutter = cl(r.shutter, 1.0, 16000.0);
    r.sensitivity = cl(r.sensitivity, 10.0, 204800.0);
    if (r.tonemap != "aceslegacy" && r.tonemap != "aces" &&
        r.tonemap != "filmic" && r.tonemap != "agx" &&
        r.tonemap != "pbrneutral" && r.tonemap != "linear") {
        r.tonemap = "aceslegacy";
    }
    r.contrast = cl(r.contrast, 0.0, 2.0);
    r.saturation = cl(r.saturation, 0.0, 2.0);
    r.temperature = cl(r.temperature, -1.0, 1.0);
    r.tint = cl(r.tint, -1.0, 1.0);
    return r;
}

// 使い分けの分かっている 3 つの組み合わせ。ブラウザの「Draft / Standard /
// Photo」ボタンは名前だけを送り、中身の定義は**ここ 1 か所**が持つ
// （UI とサーバーで別々に持つと必ずずれる）。押したあと個別の値を
// 上書きできるのは edit.render が部分更新だから。
inline RenderDesc renderPreset(const std::string& name) {
    RenderDesc r;
    if (name == "draft") {
        // 速さ優先。大量の剛体を回しながら見るとき。
        r.shadowMap = 512;
        r.fxaa = true;
        return r;
    }
    if (name == "photo") {
        // 写実優先。1 フレームの絵の質が欲しいとき（見た目の確認・記録用の
        // 動画）。MSAA 4x を軸にして、TAA は既定では入れない - 速く動く
        // 剛体で残像が出るため（静止画なら taa="true" の方が綺麗）。
        r.shadowMap = 2048;
        r.cascades = 3;
        r.shadow = "pcss";
        r.contactShadows = true;
        r.msaa = 4;
        r.fxaa = true;
        r.ssao = true;
        r.ssaoIntensity = 1.2;
        r.bloom = 0.08;
        r.ssr = true;
        r.vignette = 0.25;
        r.tonemap = "aces";
        r.contrast = 1.05;
        r.saturation = 1.02;
        return r;
    }
    return r;  // "standard"（＝既定値。これまでの見た目）
}

// ---- カメラ -----------------------------------------------------------------
// 1 台ぶんの姿勢。実体（CameraObject）と同じオービット表現のまま保存する。
// UI が見せる「位置・向き」へは Scene.cpp が変換する（eye = target + radius *
// 軌道ベクトル、pitch/yaw ⇄ elevation/azimuth）。active=false のスロットは
// 「削除された」カメラ: エンドポイントは起動時に kMaxCameras ぶん作られるので、
// 実行中の追加・削除はこのフラグの上げ下げになる。
struct CameraPose {
    double azimuth = 0.66;    // ラジアン（Y 軸まわり）
    double elevation = 0.34;  // ラジアン（水平から上向き）
    double radius = 12.0;     // 注視点までの距離 (m)
    Vec3d target{0.0, 1.0, 0.0};
    bool active = true;
};

// ---- イベントグラフ（ノードベースのイベント設計）---------------------------
// Node-RED 風の「トリガー → アクション」グラフ。ノードは値だけの設計図で、
// 実行（トリガー判定とアクション適用）はシミュレート中に物理スレッドが行う
// （Scene::runEventGraph）。アクションが変えた色や強さは「実行時の上書き」で、
// desc（設計値）は書き換えない - シミュレートを止めると全部元に戻る。
// 姿勢が desc へ戻るのと同じ原則。
enum class NodeKind {
    // トリガー（右の出力ポートから発火）
    OnCollision,  // 対象オブジェクトが何かに「新しく」触れた（接触の立ち上がり）
    OnSimStart,   // シミュレート開始の最初のステップ
    OnTimer,      // seconds ごとに繰り返し
    OnGrab,       // マウスで掴んでいる間、毎ステップ（掴んだ物が文脈に乗る）
    OnJointBreak, // ジョイント（target、-1 = どれでも）が破断した（breakForce）
    // アクション（左の入力ポートで受ける）
    SetColor,        // オブジェクトの色を color へ（実行時のみ）
    ApplyImpulse,    // オブジェクトに速度変化 vec (m/s) を与える
    SetFixed,        // value != 0 で固定、0 で解除（実行時のみ）
    GrabPull,        // 掴んでいる物をカーソルへ引き寄せる（value = 強さ倍率）
    SetLightColor,   // ライトの色を color へ（実行時のみ）
    SetLightIntensity,  // ライトの強さを value へ（実行時のみ）
    CameraLookAt,    // カメラ target の注視点をオブジェクト other へ向ける
    SetVelocity,     // オブジェクトの速度を vec (m/s) に（value != 0 なら加算）
    SetMotor,        // ジョイント target のモータ目標値を value に（実行時のみ）
};

inline const char* nodeKindName(NodeKind k) {
    switch (k) {
        case NodeKind::OnSimStart: return "onStart";
        case NodeKind::OnTimer: return "onTimer";
        case NodeKind::OnGrab: return "onGrab";
        case NodeKind::SetColor: return "setColor";
        case NodeKind::GrabPull: return "grabPull";
        case NodeKind::ApplyImpulse: return "impulse";
        case NodeKind::SetFixed: return "setFixed";
        case NodeKind::SetLightColor: return "lightColor";
        case NodeKind::SetLightIntensity: return "lightIntensity";
        case NodeKind::CameraLookAt: return "cameraLookAt";
        case NodeKind::OnJointBreak: return "onJointBreak";
        case NodeKind::SetVelocity: return "setVelocity";
        case NodeKind::SetMotor: return "setMotor";
        case NodeKind::OnCollision: break;
    }
    return "onCollision";
}
inline NodeKind nodeKindFromName(const std::string& s, NodeKind fallback) {
    if (s == "onCollision" || s == "collision") return NodeKind::OnCollision;
    if (s == "onStart" || s == "start") return NodeKind::OnSimStart;
    if (s == "onTimer" || s == "timer") return NodeKind::OnTimer;
    if (s == "onGrab" || s == "grab") return NodeKind::OnGrab;
    if (s == "setColor" || s == "color") return NodeKind::SetColor;
    if (s == "grabPull" || s == "pull") return NodeKind::GrabPull;
    if (s == "impulse" || s == "push") return NodeKind::ApplyImpulse;
    if (s == "setFixed" || s == "fixed") return NodeKind::SetFixed;
    if (s == "lightColor") return NodeKind::SetLightColor;
    if (s == "lightIntensity") return NodeKind::SetLightIntensity;
    if (s == "cameraLookAt" || s == "lookAt") return NodeKind::CameraLookAt;
    if (s == "onJointBreak" || s == "jointBreak") return NodeKind::OnJointBreak;
    if (s == "setVelocity" || s == "velocity") return NodeKind::SetVelocity;
    if (s == "setMotor" || s == "motor") return NodeKind::SetMotor;
    return fallback;
}

// トリガーかアクションか。ワイヤーは「トリガー → アクション」の向きだけ。
inline bool nodeIsTrigger(NodeKind k) {
    return k == NodeKind::OnCollision || k == NodeKind::OnSimStart ||
           k == NodeKind::OnTimer || k == NodeKind::OnGrab ||
           k == NodeKind::OnJointBreak;
}

// ノードの target 欄が指す種別。番号の検証・削除時の掃除・保存時の詰め替えは
// 全部これで分岐する（object と light は保存で番号が詰まるため。joint は
// 端点の消えたものが保存で落ちるので、これも詰め替えが要る）。
enum class NodeTargetKind { None, Object, Light, Camera, Joint };

inline NodeTargetKind nodeTargetKind(NodeKind k) {
    switch (k) {
        case NodeKind::OnCollision:
        case NodeKind::OnGrab:
        case NodeKind::SetColor:
        case NodeKind::ApplyImpulse:
        case NodeKind::SetFixed:
        case NodeKind::GrabPull: return NodeTargetKind::Object;
        case NodeKind::SetLightColor:
        case NodeKind::SetLightIntensity: return NodeTargetKind::Light;
        case NodeKind::CameraLookAt: return NodeTargetKind::Camera;
        case NodeKind::SetVelocity: return NodeTargetKind::Object;
        case NodeKind::OnJointBreak:
        case NodeKind::SetMotor: return NodeTargetKind::Joint;
        case NodeKind::OnSimStart:
        case NodeKind::OnTimer: break;
    }
    return NodeTargetKind::None;
}

// other 欄がオブジェクト番号を指すか（OnCollision の相手フィルタと
// CameraLookAt の注視先）。削除時の掃除と保存時の詰め替えに使う。
inline bool nodeOtherIsObject(NodeKind k) {
    return k == NodeKind::OnCollision || k == NodeKind::CameraLookAt;
}

// 1 個のノード。id はグラフ内で一意（削除しても再利用しない - ワイヤーが
// 別のノードを指し直してしまうため）。使わない欄は既定値のまま持つ:
// 種類ごとに構造体を分けるより、UI・JSON・実行の全部が単純になる。
struct NodeDesc {
    int id = 0;
    NodeKind kind = NodeKind::OnCollision;
    double x = 40.0, y = 40.0;  // ノードエディタのキャンバス座標 (px)
    // 対象番号。nodeTargetKind(kind) の種別を指す。-1 は「明示しない」で、
    // 意味は**そのノードが走っている文脈**で決まる（EventAssetDesc の項を
    // 参照）: オブジェクトに付いたアセットなら「自分」、トリガーが対象を
    // 渡してきたなら「その物」（OnGrab が掴んだ物・OnCollision が触れた物）、
    // どちらも無ければ OnCollision は「どのオブジェクトでも」、アクションは
    // 「未設定（何もしない）」。
    int target = -1;
    // OnCollision: 相手のフィルタ（-2 = 何でも, -1 = 地面, n = オブジェクト）。
    // CameraLookAt: 注視するオブジェクト番号。
    int other = -2;
    double seconds = 1.0;                // OnTimer の間隔
    Color3 color{0.0f, 0.0f, 0.0f};      // SetColor / SetLightColor（既定 = 黒）
    // ApplyImpulse の速度変化 / SetVelocity の速度 (m/s)。OnCollision では
    // 「求める接触の向き」（対象から見て相手の側へ向く法線。零 = 問わない。
    // 非零なら法線とのなす角 60° 以内のときだけ発火 = 「上から乗ったら」）。
    Vec3d vec{0.0, 5.0, 0.0};
    // 種類ごとの数値: SetFixed(0/1) / SetLightIntensity / GrabPull(倍率) /
    // OnCollision(最小接触力 N、0 = 何でも) / SetVelocity(0 = 上書き, 1 = 加算) /
    // SetMotor(モータの目標値。単位はジョイントの motor と同じ)。
    double value = 0.0;
};

// トリガーの出力からアクションの入力へ 1 本。多対多を許す（1 トリガーで
// 複数アクション、複数トリガーから同じアクション）。
struct WireDesc {
    int from = -1;  // トリガーノードの id
    int to = -1;    // アクションノードの id
};

// ---- イベントアセット -------------------------------------------------------
// ノードとワイヤーをひとまとめにした「スクリプト」。Unity のスクリプト資産と
// 同じ位置づけで、シーンのアセット（文書の <asset><event>）として名前で持ち、
// オブジェクトかワールドに**付ける**ことで初めて動く。1 つのオブジェクトに
// 何本でも付けられ、同じアセットを複数のオブジェクトに付け回せる。
//
//   * ノード id はアセットの中で一意（ワイヤーが id で指すため、削除しても
//     再利用しない）。別のアセットとは番号が重なってよい。
//   * 対象を書かない（target = -1）ノードは「付いている相手」に働く。だから
//     同じアセットを別のオブジェクトに付けると、そのオブジェクトに対して
//     同じことをする。番号を明示すれば今までどおり特定の相手に働く。
//   * 名前はファイル名と同じ流儀（英数字と _ -）に正規化する。XML の
//     参照（<event name="...">）に使うため。
struct EventAssetDesc {
    std::string name;
    std::vector<NodeDesc> nodes;
    std::vector<WireDesc> wires;
};

// アセット名の正規化（英数字と _ - だけ・64 文字まで）。空になったら不正な
// 名前（呼び出し側で弾く）。シーン名（EditorState::sanitizeSceneName）と
// 同じ規則だが、あちらはファイル名、こちらは文書内の参照名。
inline std::string sanitizeEventName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) out.push_back(c);
        if (out.size() >= 64) break;
    }
    return out;
}

// 付け先。オブジェクト番号か、ワールド（シーン全体）。ワールドに付けた
// アセットは「自分」を持たないので、対象を書かないノードはトリガーが渡して
// きた物にだけ働く（既定のマウス操作スクリプトがこれ）。
constexpr int kEventOwnerWorld = -1;

// シミュレート側の設定。ここの値はエディタで編集し、シミュレート開始時に
// PhysicsWorld へ流し込む（実行中の変更も反映される）。
// 積分器（ChTimestepper::Type）。NSC（相補性）の接触と組める 4 つだけ:
//   euler       … EULER_IMPLICIT_LINEARIZED（既定・最速）
//   projected   … EULER_IMPLICIT_PROJECTED（拘束の位置誤差を射影で消す）
//   implicit    … EULER_IMPLICIT（非線形反復。硬いばねに強いが重い）
//   trapezoidal … TRAPEZOIDAL_LINEARIZED（2 次精度）
//   hht         … HHT（2 次精度・数値減衰。FEA・SMC 向け。Core のみ）
//   newmark     … Newmark（同上。Core のみ）
// HHT / Newmark は滑らかな系（SMC・FEA）向け。NSC の接触と組むと硬い
// 接触で跳ねやすいので、ケーブルや SMC と一緒に使う。Multicore は自前の
// ステッパなので、これらを頼まれた文書は Scene が Core へ切り替える。
inline bool integratorNameValid(const std::string& s) {
    return s == "euler" || s == "projected" || s == "implicit" || s == "trapezoidal" ||
           s == "hht" || s == "newmark";
}
inline bool integratorNeedsCore(const std::string& s) {
    return s == "hht" || s == "newmark";
}
// ソルバ（Core バックエンドのみ。Multicore は APGD 固定）:
//   反復（VI）: bb … Barzilai-Borwein（既定）、apgd、psor、jacobi。
//               FEA の剛性行列は扱えない（bb は例外を投げ、他は無視する）
//   剛性 + 接触の VI: admm（内側に直接法。FEA のケーブルがある系の既定）、
//               pminres（KKT を射影 MINRES で解く。Chrono 9 では非推奨扱い）
//   線形（LS）: minres（反復）、直接法の sparselu / sparseqr（Eigen、Core
//               組み込み）、pardiso（Chrono::PardisoMKL）、mumps（Chrono::MUMPS）
//   線形ソルバは片側拘束（NSC の接触・可動範囲）を解けないので contact="smc"
//   か接触の無い系で使う。NSC のまま頼まれたら bb に戻して警告する。
inline bool solverNameValid(const std::string& s) {
    return s == "bb" || s == "apgd" || s == "psor" || s == "jacobi" || s == "minres" ||
           s == "admm" || s == "pminres" ||
           s == "sparselu" || s == "sparseqr" || s == "pardiso" || s == "mumps";
}
// FEA の剛性行列を扱える VI ソルバ（ケーブルのある NSC の系で使えるもの）。
inline bool solverHandlesStiffness(const std::string& s) {
    return s == "admm" || s == "pminres" || s == "minres" || s == "sparselu" ||
           s == "sparseqr" || s == "pardiso" || s == "mumps";
}
inline bool solverIsDirect(const std::string& s) {
    return s == "sparselu" || s == "sparseqr" || s == "pardiso" || s == "mumps";
}
// 線形（LS）ソルバ = NSC の接触を解けないもの（直接法 + minres）。
inline bool solverIsLinear(const std::string& s) {
    return solverIsDirect(s) || s == "minres";
}
// Multicore が持たないソルバ（= 頼まれたら Core へ切り替える）。反復の
// bb / apgd / psor / jacobi は Multicore では APGD で代用する（切替不要）。
inline bool solverNeedsCore(const std::string& s) {
    return s == "minres" || s == "admm" || s == "pminres" || solverIsDirect(s);
}
// 接触の解き方: nsc（相補性 = 硬い接触。既定）/ smc（ペナルティ法 =
// ヤング率で決まる柔らかい接触。粒状体・FEA 向け。Core / Multicore とも可）。
inline bool contactNameValid(const std::string& s) {
    return s == "nsc" || s == "smc";
}
// 2 つの材質から接触の摩擦・反発をどう合成するか（Chrono の
// ChContactMaterialCompositionStrategy）: min（既定）/ average / max。
inline bool combineNameValid(const std::string& s) {
    return s == "min" || s == "average" || s == "max";
}

struct SimSettings {
    double gravity = -9.81;  // m/s^2（Y 成分。斜面・無重力は X / Z と組む）
    double gravityX = 0.0;   // m/s^2（X 成分）
    double gravityZ = 0.0;   // m/s^2（Z 成分）
    int hz = 60;             // 物理更新レート
    int substeps = 2;
    int iterations = 60;
    double envelope = 0.002;  // 接触エンベロープ (m)
    double recovery = 0.2;    // めり込み解消速度 (m/s)
    float friction = 0.6f;
    float restitution = 0.0f;
    double linearDamping = 0.15;   // 1/s
    double angularDamping = 0.60;  // 1/s
    bool sleeping = true;
    std::string integrator = "euler";  // 上の integratorNameValid
    std::string solver = "bb";         // 上の solverNameValid（Core のみ）
    std::string combine = "min";       // 上の combineNameValid
    std::string contact = "nsc";       // 上の contactNameValid
    // SMC の材質の既定（<geom young poisson> が -1 のボディに使う）。
    double young = 2.0e7;    // Pa（ゴム〜木の中間。剛い物ほど小さな dt が要る）
    double poisson = 0.3;
    // モーダル解析: シミュレート開始時に系の固有振動数をこの本数だけ求める
    // （0 = しない）。Chrono::Modal モジュールと Core バックエンドが要る。
    int modal = 0;
};

// ---- ケーブル（FEA）--------------------------------------------------------
// Chrono の FEA モジュール（ANCF ケーブル要素）で作るロープ・ワイヤ。
// 2 点の間に segments 本の梁要素を直線に並べ、端を物か地面に留める。
// 剛体ではないので GameObject ではなく Scene のケーブル一覧が持つ（ジョイント
// と同じく「シミュレート開始で作り、停止で捨てる」）。Multicore は FEA を
// 扱えないので、ケーブルのある文書は Scene が Core へ切り替える。
//   bodyA / bodyB … 端を留める相手。オブジェクト番号、-1 = 地面（固定点）、
//                   -2 = 何にも留めない（自由端）
//   anchorA / anchorB … 端の位置（ワールド座標）。留め先の物とはこの点で
//                   拘束される（ChLinkNodeFrame）
//   segments … 要素数（2〜64）。diameter … 直径 (m)。density … kg/m^3。
//   young … ヤング率 (Pa)。1e7 で柔らかいロープ、1e9 で鋼線に近い。
//   damping … Rayleigh 減衰（0.001〜0.1）。collide … 節点の球で接触するか。
struct CableDesc {
    std::string name;
    int bodyA = -1;
    int bodyB = -1;
    Vec3d anchorA{0.0, 2.0, 0.0};
    Vec3d anchorB{1.0, 2.0, 0.0};
    int segments = 16;
    double diameter = 0.02;
    double density = 1000.0;
    double young = 1.0e7;
    double damping = 0.01;
    bool collide = true;
    Color3 color{0.85f, 0.75f, 0.45f};
};
constexpr int kCableFreeEnd = -2;
inline CableDesc clampCable(CableDesc c) {
    auto cl = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
    if (c.segments < 2) c.segments = 2;
    if (c.segments > 64) c.segments = 64;
    c.diameter = cl(c.diameter, 0.001, 1.0);
    c.density = cl(c.density, 1.0, 20000.0);
    c.young = cl(c.young, 1.0e4, 1.0e12);
    c.damping = cl(c.damping, 0.0, 1.0);
    if (c.bodyA < kCableFreeEnd) c.bodyA = -1;
    if (c.bodyB < kCableFreeEnd) c.bodyB = -1;
    return c;
}

// ---- JSON 変換 ------------------------------------------------------------
// nlohmann の ADL 版（to_json/from_json）ではなく明示的な関数にしてある。
// 「どのキーが出るか」がそのまま保存フォーマットとブラウザ API になるので、
// 一箇所で読めるほうがよい。

// 型が違っても投げない数値・整数の取り出し。/input は誰でも叩けるので、
// 文字列などを混ぜたリクエストで nlohmann の value() が type_error を投げ、
// 処理スレッドごと落ちる - それを既定値へ落として続行するための口。
inline double jsonNumber(const nlohmann::json& j, const char* key,
                         double fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}
inline int jsonInt(const nlohmann::json& j, const char* key, int fallback) {
    if (!j.is_object()) return fallback;
    const auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? int(it->get<double>())
                                              : fallback;
}
// 文字列の配列（イベントアセット名の一覧）。文字列以外の要素は捨てる
// - /input は誰でも叩けるので、型を信じない、の同じ流儀。
inline std::vector<std::string> stringList(const nlohmann::json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (e.is_string()) out.push_back(e.get<std::string>());
    }
    return out;
}

inline nlohmann::json toJson(const Vec3d& v) {
    return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
}
inline Vec3d vec3FromJson(const nlohmann::json& j, const Vec3d& fallback) {
    if (!j.is_object()) return fallback;
    Vec3d v;
    v.x = jsonNumber(j, "x", fallback.x);
    v.y = jsonNumber(j, "y", fallback.y);
    v.z = jsonNumber(j, "z", fallback.z);
    return v;
}

// 色は UI 側の <input type="color"> に合わせて "#rrggbb"。sRGB ではなく
// リニア値をそのまま 0-255 に写す（マテリアルがリニアを受け取るため）。
inline std::string colorToHex(const Color3& c) {
    auto ch = [](float v) {
        const int i = int(v * 255.0f + 0.5f);
        return i < 0 ? 0 : (i > 255 ? 255 : i);
    };
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", ch(c.r), ch(c.g), ch(c.b));
    return std::string(buf);
}
inline Color3 colorFromHex(const std::string& hex, const Color3& fallback) {
    if (hex.size() != 7 || hex[0] != '#') return fallback;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = nib(hex[std::size_t(i) + 1]);
        if (v[i] < 0) return fallback;
    }
    Color3 c;
    c.r = float(v[0] * 16 + v[1]) / 255.0f;
    c.g = float(v[2] * 16 + v[3]) / 255.0f;
    c.b = float(v[4] * 16 + v[5]) / 255.0f;
    return c;
}

inline nlohmann::json toJson(const MeshAssetDesc& m) {
    nlohmann::json j;
    j["name"] = m.name;
    j["file"] = m.file;
    j["scale"] = m.scale;
    return j;
}

// ソフトボディの設定。enabled = BodyDesc::hasSoft をここに同居させる
// （ブラウザの Inspector は 1 節でまとめて送るため）。
inline nlohmann::json toJson(const SoftDesc& s, bool enabled) {
    nlohmann::json j;
    j["enabled"] = enabled;
    j["res"] = s.resolution;
    j["stiffness"] = s.stiffness;
    j["damping"] = s.damping;
    j["shear"] = s.shear;
    j["bend"] = s.bend;
    j["iterations"] = s.iterations;
    return j;
}
// 送られてきたキーだけ上書き（部分更新）。enabled は呼び出し側が受け取る。
inline SoftDesc softFromJson(const nlohmann::json& j, const SoftDesc& base) {
    SoftDesc s = base;
    if (!j.is_object()) return s;
    s.resolution = jsonInt(j, "res", s.resolution);
    s.stiffness = jsonNumber(j, "stiffness", s.stiffness);
    s.damping = jsonNumber(j, "damping", s.damping);
    s.shear = jsonNumber(j, "shear", s.shear);
    s.bend = jsonNumber(j, "bend", s.bend);
    s.iterations = jsonInt(j, "iterations", s.iterations);
    return clampSoft(s);
}

// 材質（PBR）。キーは XML の属性名と同じ。ブラウザの Inspector と
// <geom> / <part> の両方がこの名前を使う。
inline nlohmann::json toJson(const MaterialDesc& m) {
    nlohmann::json j;
    j["roughness"] = m.roughness;
    j["metallic"] = m.metallic;
    j["reflectance"] = m.reflectance;
    j["clearcoat"] = m.clearCoat;
    j["clearcoatRoughness"] = m.clearCoatRoughness;
    j["emissive"] = m.emissive;
    return j;
}

// 送られてきたキーだけ上書き（部分更新）。
inline MaterialDesc materialFromJson(const nlohmann::json& j,
                                     const MaterialDesc& base) {
    MaterialDesc m = base;
    if (!j.is_object()) return m;
    m.roughness = jsonNumber(j, "roughness", m.roughness);
    m.metallic = jsonNumber(j, "metallic", m.metallic);
    m.reflectance = jsonNumber(j, "reflectance", m.reflectance);
    m.clearCoat = jsonNumber(j, "clearcoat", m.clearCoat);
    m.clearCoatRoughness =
        jsonNumber(j, "clearcoatRoughness", m.clearCoatRoughness);
    m.emissive = jsonNumber(j, "emissive", m.emissive);
    return clampMaterial(m);
}

inline nlohmann::json toJson(const BodyDesc& b) {
    nlohmann::json j;
    j["name"] = b.name;
    j["shape"] = shapeName(b.shape);
    if (b.shape == ShapeKind::Model) j["mesh"] = b.mesh;
    j["collision"] = shapeName(b.collision);
    j["size"] = toJson(b.size);
    j["position"] = toJson(b.position);
    j["rotation"] = toJson(b.rotation);
    j["mass"] = b.mass;
    j["fixed"] = b.fixed;
    j["color"] = colorToHex(b.color);
    j["material"] = toJson(b.material);  // 材質（PBR）
    j["events"] = b.events;  // 付いているイベントアセット名
    j["vehicle"] = b.hasVehicle;  // 車両か（中身の編集は XML で）
    j["prefab"] = b.prefab;
    j["soft"] = toJson(b.soft, b.hasSoft);  // ソフトボディ（Inspector で編集）
    // 接触の物性・衝突レイヤ・重力・初速（Inspector の「物性」節）。
    j["surface"] = {{"friction", b.surface.friction},
                    {"restitution", b.surface.restitution},
                    {"rolling", b.surface.rolling},
                    {"cohesion", b.surface.cohesion},
                    {"young", b.surface.young},
                    {"poisson", b.surface.poisson}};
    j["layer"] = b.layer;
    j["nocollide"] = b.nocollide;
    j["gravity"] = b.gravity;
    j["velocity"] = toJson(b.velocity);
    j["angularVelocity"] = toJson(b.angularVelocity);
    j["force"] = toJson(b.force);
    j["torque"] = toJson(b.torque);
    return j;
}

inline SurfaceDesc surfaceFromJson(const nlohmann::json& j, const SurfaceDesc& base) {
    SurfaceDesc s = base;
    if (!j.is_object()) return s;
    s.friction = float(jsonNumber(j, "friction", s.friction));
    s.restitution = float(jsonNumber(j, "restitution", s.restitution));
    s.rolling = float(jsonNumber(j, "rolling", s.rolling));
    s.cohesion = float(jsonNumber(j, "cohesion", s.cohesion));
    s.young = float(jsonNumber(j, "young", s.young));
    s.poisson = float(jsonNumber(j, "poisson", s.poisson));
    return clampSurface(s);
}

inline BodyDesc bodyFromJson(const nlohmann::json& j, const BodyDesc& base) {
    BodyDesc b = base;
    if (!j.is_object()) return b;
    if (j.contains("name") && j["name"].is_string()) b.name = j["name"];
    if (j.contains("shape") && j["shape"].is_string()) {
        b.shape = shapeFromName(j["shape"], b.shape);
        // collision の指定が無いときは見た目に合わせる（エディタで置いた物は
        // 常にこれ。分けて持つのは既存シーンの都合だけなので）。
        if (!j.contains("collision")) b.collision = b.shape;
    }
    if (j.contains("collision") && j["collision"].is_string()) {
        b.collision = shapeFromName(j["collision"], b.collision);
    }
    if (j.contains("mesh") && j["mesh"].is_string()) b.mesh = j["mesh"];
    b.size = vec3FromJson(j.value("size", nlohmann::json()), b.size);
    b.position = vec3FromJson(j.value("position", nlohmann::json()), b.position);
    b.rotation = vec3FromJson(j.value("rotation", nlohmann::json()), b.rotation);
    b.mass = j.value("mass", b.mass);
    b.fixed = j.value("fixed", b.fixed);
    if (j.contains("color") && j["color"].is_string()) {
        b.color = colorFromHex(j["color"], b.color);
    }
    if (j.contains("material")) {
        b.material = materialFromJson(j["material"], b.material);
    }
    // 付けるプレハブ（名前）。置いた時点で付ける用途（🪜 Stairs など）。
    if (j.contains("prefab") && j["prefab"].is_string()) {
        b.prefab = sanitizeEventName(j["prefab"].get<std::string>());
    }
    // イベントアセットの付け外しは専用のコマンド（edit.event.attach /
    // detach）で行うので、ここでは配列がまるごと来たときだけ受ける。
    if (j.contains("events") && j["events"].is_array()) {
        b.events = stringList(j["events"]);
    }
    // ソフトボディ。`"soft": true` の短縮形（有効化だけ）も受ける。
    if (j.contains("soft")) {
        const nlohmann::json& s = j["soft"];
        if (s.is_boolean()) {
            b.hasSoft = s.get<bool>();
        } else if (s.is_object()) {
            const auto en = s.find("enabled");
            if (en != s.end() && en->is_boolean()) b.hasSoft = en->get<bool>();
            b.soft = softFromJson(s, b.soft);
        }
    }
    if (j.contains("surface")) b.surface = surfaceFromJson(j["surface"], b.surface);
    b.layer = jsonInt(j, "layer", b.layer);
    if (j.contains("nocollide") && j["nocollide"].is_array()) {
        b.nocollide.clear();
        for (const auto& v : j["nocollide"]) {
            if (v.is_number_integer()) b.nocollide.push_back(v.get<int>());
        }
    }
    if (j.contains("gravity") && j["gravity"].is_boolean()) b.gravity = j["gravity"];
    b.velocity = vec3FromJson(j.value("velocity", nlohmann::json()), b.velocity);
    b.angularVelocity =
        vec3FromJson(j.value("angularVelocity", nlohmann::json()), b.angularVelocity);
    b.force = vec3FromJson(j.value("force", nlohmann::json()), b.force);
    b.torque = vec3FromJson(j.value("torque", nlohmann::json()), b.torque);
    return b;
}

inline nlohmann::json toJson(const JointDesc& jt) {
    nlohmann::json j;
    j["name"] = jt.name;
    j["kind"] = jointName(jt.kind);
    j["a"] = jt.bodyA;
    j["b"] = jt.bodyB;
    j["anchor"] = toJson(jt.anchor);
    j["axis"] = toJson(jt.axis);
    j["distance"] = jt.distance;
    j["limited"] = jt.limited;
    j["limitLo"] = jt.limitLo;
    j["limitHi"] = jt.limitHi;
    j["motor"] = motorModeName(jt.motor);
    j["motorTarget"] = jt.motorTarget;
    j["stiffness"] = jt.stiffness;
    j["damping"] = jt.damping;
    j["breakForce"] = jt.breakForce;
    j["ratio"] = jt.ratio;
    j["pitch"] = jt.pitch;
    j["anchor2"] = toJson(jt.anchor2);
    j["axis2"] = toJson(jt.axis2);
    j["rotStiffness"] = jt.rotStiffness;
    j["rotDamping"] = jt.rotDamping;
    return j;
}

inline JointDesc jointFromJson(const nlohmann::json& j, const JointDesc& base) {
    JointDesc jt = base;
    if (!j.is_object()) return jt;
    if (j.contains("name") && j["name"].is_string()) jt.name = j["name"];
    if (j.contains("kind") && j["kind"].is_string()) {
        jt.kind = jointFromName(j["kind"], jt.kind);
    }
    jt.bodyA = j.value("a", jt.bodyA);
    jt.bodyB = j.value("b", jt.bodyB);
    jt.anchor = vec3FromJson(j.value("anchor", nlohmann::json()), jt.anchor);
    jt.axis = vec3FromJson(j.value("axis", nlohmann::json()), jt.axis);
    jt.distance = jsonNumber(j, "distance", jt.distance);
    if (j.contains("limited") && j["limited"].is_boolean()) jt.limited = j["limited"];
    jt.limitLo = jsonNumber(j, "limitLo", jt.limitLo);
    jt.limitHi = jsonNumber(j, "limitHi", jt.limitHi);
    if (j.contains("motor") && j["motor"].is_string()) {
        jt.motor = motorModeFromName(j["motor"], jt.motor);
    }
    jt.motorTarget = jsonNumber(j, "motorTarget", jt.motorTarget);
    jt.stiffness = jsonNumber(j, "stiffness", jt.stiffness);
    jt.damping = jsonNumber(j, "damping", jt.damping);
    jt.breakForce = jsonNumber(j, "breakForce", jt.breakForce);
    jt.ratio = jsonNumber(j, "ratio", jt.ratio);
    jt.pitch = jsonNumber(j, "pitch", jt.pitch);
    jt.anchor2 = vec3FromJson(j.value("anchor2", nlohmann::json()), jt.anchor2);
    jt.axis2 = vec3FromJson(j.value("axis2", nlohmann::json()), jt.axis2);
    jt.rotStiffness = jsonNumber(j, "rotStiffness", jt.rotStiffness);
    jt.rotDamping = jsonNumber(j, "rotDamping", jt.rotDamping);
    return clampJoint(jt);
}

inline nlohmann::json toJson(const CableDesc& c) {
    nlohmann::json j;
    j["name"] = c.name;
    j["a"] = c.bodyA;
    j["b"] = c.bodyB;
    j["anchorA"] = toJson(c.anchorA);
    j["anchorB"] = toJson(c.anchorB);
    j["segments"] = c.segments;
    j["diameter"] = c.diameter;
    j["density"] = c.density;
    j["young"] = c.young;
    j["damping"] = c.damping;
    j["collide"] = c.collide;
    j["color"] = colorToHex(c.color);
    return j;
}
inline CableDesc cableFromJson(const nlohmann::json& j, const CableDesc& base) {
    CableDesc c = base;
    if (!j.is_object()) return c;
    if (j.contains("name") && j["name"].is_string()) c.name = j["name"];
    c.bodyA = jsonInt(j, "a", c.bodyA);
    c.bodyB = jsonInt(j, "b", c.bodyB);
    c.anchorA = vec3FromJson(j.value("anchorA", nlohmann::json()), c.anchorA);
    c.anchorB = vec3FromJson(j.value("anchorB", nlohmann::json()), c.anchorB);
    c.segments = jsonInt(j, "segments", c.segments);
    c.diameter = jsonNumber(j, "diameter", c.diameter);
    c.density = jsonNumber(j, "density", c.density);
    c.young = jsonNumber(j, "young", c.young);
    c.damping = jsonNumber(j, "damping", c.damping);
    if (j.contains("collide") && j["collide"].is_boolean()) c.collide = j["collide"];
    if (j.contains("color") && j["color"].is_string()) {
        c.color = colorFromHex(j["color"], c.color);
    }
    return clampCable(c);
}

inline nlohmann::json toJson(const LightDesc& l) {
    nlohmann::json j;
    j["name"] = l.name;
    j["kind"] = lightKindName(l.kind);
    j["position"] = toJson(l.position);
    j["rotation"] = toJson(l.rotation);
    j["color"] = colorToHex(l.color);
    j["intensity"] = l.intensity;
    j["falloff"] = l.falloff;
    j["spotInnerDeg"] = l.spotInnerDeg;
    j["spotOuterDeg"] = l.spotOuterDeg;
    j["shadows"] = l.shadows;
    return j;
}

inline LightDesc lightFromJson(const nlohmann::json& j, const LightDesc& base) {
    LightDesc l = base;
    if (!j.is_object()) return l;
    if (j.contains("name") && j["name"].is_string()) l.name = j["name"];
    if (j.contains("kind") && j["kind"].is_string()) {
        l.kind = lightKindFromName(j["kind"], l.kind);
    }
    l.position = vec3FromJson(j.value("position", nlohmann::json()), l.position);
    l.rotation = vec3FromJson(j.value("rotation", nlohmann::json()), l.rotation);
    if (j.contains("color") && j["color"].is_string()) {
        l.color = colorFromHex(j["color"], l.color);
    }
    l.intensity = j.value("intensity", l.intensity);
    l.falloff = j.value("falloff", l.falloff);
    l.spotInnerDeg = j.value("spotInnerDeg", l.spotInnerDeg);
    l.spotOuterDeg = j.value("spotOuterDeg", l.spotOuterDeg);
    l.shadows = j.value("shadows", l.shadows);
    return l;
}

inline nlohmann::json toJson(const CameraPose& c) {
    nlohmann::json j;
    j["azimuth"] = c.azimuth;
    j["elevation"] = c.elevation;
    j["radius"] = c.radius;
    j["target"] = toJson(c.target);
    j["active"] = c.active;
    return j;
}

inline CameraPose cameraPoseFromJson(const nlohmann::json& j,
                                     const CameraPose& base) {
    CameraPose c = base;
    if (!j.is_object()) return c;
    c.azimuth = j.value("azimuth", c.azimuth);
    c.elevation = j.value("elevation", c.elevation);
    c.radius = j.value("radius", c.radius);
    c.target = vec3FromJson(j.value("target", nlohmann::json()), c.target);
    c.active = j.value("active", c.active);
    return c;
}

inline nlohmann::json toJson(const NodeDesc& n) {
    nlohmann::json j;
    j["id"] = n.id;
    j["kind"] = nodeKindName(n.kind);
    j["x"] = n.x;
    j["y"] = n.y;
    j["target"] = n.target;
    j["other"] = n.other;
    j["seconds"] = n.seconds;
    j["color"] = colorToHex(n.color);
    j["vec"] = toJson(n.vec);
    j["value"] = n.value;
    return j;
}

inline NodeDesc nodeFromJson(const nlohmann::json& j, const NodeDesc& base) {
    NodeDesc n = base;
    if (!j.is_object()) return n;
    n.id = jsonInt(j, "id", n.id);
    if (j.contains("kind") && j["kind"].is_string()) {
        n.kind = nodeKindFromName(j["kind"], n.kind);
    }
    n.x = jsonNumber(j, "x", n.x);
    n.y = jsonNumber(j, "y", n.y);
    n.target = jsonInt(j, "target", n.target);
    n.other = jsonInt(j, "other", n.other);
    n.seconds = jsonNumber(j, "seconds", n.seconds);
    if (j.contains("color") && j["color"].is_string()) {
        n.color = colorFromHex(j["color"], n.color);
    }
    n.vec = vec3FromJson(j.value("vec", nlohmann::json()), n.vec);
    n.value = jsonNumber(j, "value", n.value);
    return n;
}

inline nlohmann::json toJson(const WireDesc& w) {
    return nlohmann::json{{"from", w.from}, {"to", w.to}};
}

// イベントアセット 1 個ぶん。ブラウザはこれを受けてノードエディタを描く
// （発火回数 fired は Scene が実行時に足す）。
inline nlohmann::json toJson(const EventAssetDesc& a) {
    nlohmann::json j;
    j["name"] = a.name;
    j["nodes"] = nlohmann::json::array();
    for (const auto& n : a.nodes) j["nodes"].push_back(toJson(n));
    j["wires"] = nlohmann::json::array();
    for (const auto& w : a.wires) j["wires"].push_back(toJson(w));
    return j;
}

inline WireDesc wireFromJson(const nlohmann::json& j, const WireDesc& base) {
    WireDesc w = base;
    if (!j.is_object()) return w;
    w.from = jsonInt(j, "from", w.from);
    w.to = jsonInt(j, "to", w.to);
    return w;
}

// 文書に書いてよいファイル参照（<mesh file> / <ground texture> /
// <environment hdr>）は assets/ からの相対パスだけ。".." と絶対パス
// （/ 始まり・ドライブレター）を弾いて、読み込み先を assets/ の下に
// 閉じ込める（保存名の正規化と同じ動機。文書もブラウザの入力欄も
// 手で書けるので、XML の読み込みと edit.* の両方がこれを通す）。
inline bool assetFileAllowed(const std::string& file) {
    if (file.empty()) return false;
    if (file[0] == '/' || file[0] == '\\') return false;
    if (file.size() > 1 && file[1] == ':') return false;  // C:\ ...
    if (file.find("..") != std::string::npos) return false;
    return true;
}

// 地面と環境光。キーは XML の属性名と同じ（size = 物理の半寸法、visual =
// 見える地面の半寸法）。ブラウザの World 節（Inspector）とやり取りする。
// ---- プレハブの JSON ---------------------------------------------------------
inline nlohmann::json toJson(const PartDesc& p) {
    nlohmann::json j;
    j["name"] = p.name;
    j["type"] = partKindName(p.kind);
    j["mesh"] = p.mesh;
    j["position"] = toJson(p.position);
    j["rotation"] = toJson(p.rotation);
    j["size"] = toJson(p.size);
    j["color"] = colorToHex(p.color);
    j["material"] = toJson(p.material);
    j["socket"] = p.socket;
    j["collide"] = p.collide;
    return j;
}
inline PartDesc partFromJson(const nlohmann::json& j, const PartDesc& base) {
    PartDesc p = base;
    if (!j.is_object()) return p;
    if (j.contains("name") && j["name"].is_string()) p.name = j["name"];
    if (j.contains("type") && j["type"].is_string()) {
        p.kind = partKindFromName(j["type"], p.kind);
    }
    if (j.contains("mesh") && j["mesh"].is_string()) p.mesh = j["mesh"];
    p.position = vec3FromJson(j.value("position", nlohmann::json()), p.position);
    p.rotation = vec3FromJson(j.value("rotation", nlohmann::json()), p.rotation);
    p.size = vec3FromJson(j.value("size", nlohmann::json()), p.size);
    if (j.contains("color") && j["color"].is_string()) {
        p.color = colorFromHex(j["color"], p.color);
    }
    if (j.contains("material")) {
        p.material = materialFromJson(j["material"], p.material);
    }
    if (j.contains("socket") && j["socket"].is_string()) p.socket = j["socket"];
    if (j.contains("collide") && j["collide"].is_boolean()) p.collide = j["collide"];
    return p;
}
inline PartDesc clampPart(PartDesc p) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    p.position.x = cl(p.position.x, -100.0, 100.0);
    p.position.y = cl(p.position.y, -100.0, 100.0);
    p.position.z = cl(p.position.z, -100.0, 100.0);
    p.size.x = cl(p.size.x, 0.005, 50.0);
    p.size.y = cl(p.size.y, 0.005, 50.0);
    p.size.z = cl(p.size.z, 0.005, 50.0);
    if (p.name.size() > 64) p.name.resize(64);
    if (p.socket.size() > 32) p.socket.resize(32);
    p.material = clampMaterial(p.material);
    // 当たり判定は車体に固定の箱 / 球 / 円柱だけ（車輪に付く部品は動くので
    // 複合形状にできない。メッシュ部品の当たり形状は未対応）。
    if (!p.socket.empty() || p.kind == PartKind::Mesh) p.collide = false;
    return p;
}
inline nlohmann::json toJson(const PrefabDesc& d) {
    nlohmann::json j;
    j["name"] = d.name;
    j["parts"] = nlohmann::json::array();
    for (const auto& p : d.parts) j["parts"].push_back(toJson(p));
    return j;
}

// ---- ノード式（計算式アセット）の JSON --------------------------------------
// 型は vehicle/Formula.h（Chrono も JSON も知らない側）。ここはブラウザ API
// との変換だけ。ノードの params は数値の配列で、"1 2 3" の文字列でも受ける
// （折れ線の点列を打ちやすいように）。
inline std::vector<double> numberList(const nlohmann::json& j) {
    std::vector<double> out;
    if (j.is_array()) {
        for (const auto& e : j) {
            if (e.is_number()) out.push_back(e.get<double>());
        }
    } else if (j.is_number()) {
        out.push_back(j.get<double>());
    } else if (j.is_string()) {
        const std::string s = j.get<std::string>();
        std::size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t' ||
                                    s[i] == '\n')) ++i;
            if (i >= s.size()) break;
            char* end = nullptr;
            const double v = std::strtod(s.c_str() + i, &end);
            const std::size_t used = std::size_t(end - (s.c_str() + i));
            if (used == 0) break;
            out.push_back(v);
            i += used;
        }
    }
    if (out.size() > 128) out.resize(128);
    return out;
}
// ポート名（in / out の name）: 英数字と _ だけ、32 文字まで。
inline std::string sanitizePortName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (ok) out.push_back(c);
        if (out.size() >= 32) break;
    }
    return out;
}
inline nlohmann::json toJson(const wizengine::vehicle::FormulaNodeDesc& n) {
    nlohmann::json j;
    j["id"] = n.id;
    j["type"] = n.kind;
    j["name"] = n.name;
    j["params"] = n.params;
    j["x"] = n.x;
    j["y"] = n.y;
    return j;
}
inline wizengine::vehicle::FormulaNodeDesc formulaNodeFromJson(
    const nlohmann::json& j, const wizengine::vehicle::FormulaNodeDesc& base) {
    wizengine::vehicle::FormulaNodeDesc n = base;
    if (!j.is_object()) return n;
    n.id = jsonInt(j, "id", n.id);
    if (j.contains("type") && j["type"].is_string()) n.kind = j["type"];
    if (j.contains("name") && j["name"].is_string()) {
        n.name = sanitizePortName(j["name"].get<std::string>());
    }
    if (j.contains("params")) n.params = numberList(j["params"]);
    if (j.contains("value") && j["value"].is_number()) {
        n.params = {j["value"].get<double>()};
    }
    n.x = jsonNumber(j, "x", n.x);
    n.y = jsonNumber(j, "y", n.y);
    return n;
}
inline wizengine::vehicle::FormulaNodeDesc clampFormulaNode(
    wizengine::vehicle::FormulaNodeDesc n) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    n.x = cl(n.x, 0.0, 20000.0);
    n.y = cl(n.y, 0.0, 20000.0);
    if (n.params.size() > 128) n.params.resize(128);
    for (double& v : n.params) {
        if (!(v == v)) v = 0.0;  // NaN
        v = cl(v, -1e12, 1e12);
    }
    n.name = sanitizePortName(n.name);
    return n;
}
inline nlohmann::json toJson(const wizengine::vehicle::FormulaWireDesc& w) {
    nlohmann::json j;
    j["from"] = w.from;
    j["fromPort"] = w.fromPort;
    j["to"] = w.to;
    j["port"] = w.port;
    return j;
}
inline wizengine::vehicle::FormulaWireDesc formulaWireFromJson(const nlohmann::json& j) {
    wizengine::vehicle::FormulaWireDesc w;
    w.from = jsonInt(j, "from", -1);
    w.fromPort = jsonInt(j, "fromPort", 0);
    w.to = jsonInt(j, "to", -1);
    w.port = jsonInt(j, "port", 0);
    return w;
}
inline nlohmann::json toJson(const wizengine::vehicle::FormulaGraphDesc& g) {
    nlohmann::json j;
    j["name"] = g.name;
    j["nodes"] = nlohmann::json::array();
    for (const auto& n : g.nodes) j["nodes"].push_back(toJson(n));
    j["wires"] = nlohmann::json::array();
    for (const auto& w : g.wires) j["wires"].push_back(toJson(w));
    return j;
}

inline nlohmann::json toJson(const GroundDesc& g) {
    nlohmann::json j;
    j["size"] = g.half;
    j["visual"] = g.visualHalf;
    j["texture"] = g.texture;
    j["tile"] = g.tile;
    j["color"] = colorToHex(g.tint);
    j["roughness"] = g.roughness;
    j["metallic"] = g.metallic;
    return j;
}

inline GroundDesc groundFromJson(const nlohmann::json& j,
                                 const GroundDesc& base) {
    GroundDesc g = base;
    if (!j.is_object()) return g;
    g.half = jsonNumber(j, "size", g.half);
    g.visualHalf = jsonNumber(j, "visual", g.visualHalf);
    if (j.contains("texture") && j["texture"].is_string()) {
        // 空 = 市松模様。パスとして不正なもの（".." 等）は無視して現状維持。
        const std::string t = j["texture"];
        if (t.empty() || assetFileAllowed(t)) g.texture = t;
    }
    g.tile = jsonNumber(j, "tile", g.tile);
    if (j.contains("color") && j["color"].is_string()) {
        g.tint = colorFromHex(j["color"], g.tint);
    }
    g.roughness = jsonNumber(j, "roughness", g.roughness);
    g.metallic = jsonNumber(j, "metallic", g.metallic);
    return g;
}

inline nlohmann::json toJson(const EnvironmentDesc& e) {
    nlohmann::json j;
    j["hdr"] = e.hdr;
    j["intensity"] = e.intensity;
    j["skybox"] = e.skybox;
    return j;
}

inline EnvironmentDesc environmentFromJson(const nlohmann::json& j,
                                           const EnvironmentDesc& base) {
    EnvironmentDesc e = base;
    if (!j.is_object()) return e;
    if (j.contains("hdr") && j["hdr"].is_string()) {
        // 空 = 環境マップ無し。不正なパスは無視して現状維持。
        const std::string h = j["hdr"];
        if (h.empty() || assetFileAllowed(h)) e.hdr = h;
    }
    e.intensity = jsonNumber(j, "intensity", e.intensity);
    if (j.contains("skybox") && j["skybox"].is_boolean()) {
        e.skybox = j["skybox"].get<bool>();
    }
    return e;
}

// 描画品質。キーは XML の属性名と同じで、Inspector の「描画」節が使う。
inline nlohmann::json toJson(const RenderDesc& r) {
    nlohmann::json j;
    j["shadowMap"] = r.shadowMap;
    j["cascades"] = r.cascades;
    j["shadow"] = r.shadow;
    j["contactShadows"] = r.contactShadows;
    j["msaa"] = r.msaa;
    j["taa"] = r.taa;
    j["fxaa"] = r.fxaa;
    j["postprocess"] = r.postProcess;
    j["ssao"] = r.ssao;
    j["ssaoIntensity"] = r.ssaoIntensity;
    j["bloom"] = r.bloom;
    j["ssr"] = r.ssr;
    j["dof"] = r.dof;
    j["dofBlur"] = r.dofBlur;
    j["vignette"] = r.vignette;
    j["aperture"] = r.aperture;
    j["shutter"] = r.shutter;
    j["sensitivity"] = r.sensitivity;
    j["tonemap"] = r.tonemap;
    j["contrast"] = r.contrast;
    j["saturation"] = r.saturation;
    j["temperature"] = r.temperature;
    j["tint"] = r.tint;
    return j;
}

// 送られてきたキーだけ上書き（部分更新）。"preset" があれば**先に**その
// 組み合わせへ切り替えてから、残りのキーで上書きする（ボタンを押しながら
// 1 つだけ変える、が自然に書けるように）。
inline RenderDesc renderFromJson(const nlohmann::json& j,
                                 const RenderDesc& base) {
    RenderDesc r = base;
    if (!j.is_object()) return r;
    if (j.contains("preset") && j["preset"].is_string()) {
        r = renderPreset(j["preset"].get<std::string>());
    }
    r.shadowMap = jsonInt(j, "shadowMap", r.shadowMap);
    r.cascades = jsonInt(j, "cascades", r.cascades);
    if (j.contains("shadow") && j["shadow"].is_string()) {
        r.shadow = j["shadow"].get<std::string>();
    }
    if (j.contains("contactShadows") && j["contactShadows"].is_boolean()) {
        r.contactShadows = j["contactShadows"].get<bool>();
    }
    r.msaa = jsonInt(j, "msaa", r.msaa);
    if (j.contains("taa") && j["taa"].is_boolean()) r.taa = j["taa"].get<bool>();
    if (j.contains("fxaa") && j["fxaa"].is_boolean()) {
        r.fxaa = j["fxaa"].get<bool>();
    }
    if (j.contains("postprocess") && j["postprocess"].is_boolean()) {
        r.postProcess = j["postprocess"].get<bool>();
    }
    if (j.contains("ssao") && j["ssao"].is_boolean()) {
        r.ssao = j["ssao"].get<bool>();
    }
    r.ssaoIntensity = jsonNumber(j, "ssaoIntensity", r.ssaoIntensity);
    r.bloom = jsonNumber(j, "bloom", r.bloom);
    if (j.contains("ssr") && j["ssr"].is_boolean()) r.ssr = j["ssr"].get<bool>();
    r.dof = jsonNumber(j, "dof", r.dof);
    r.dofBlur = jsonNumber(j, "dofBlur", r.dofBlur);
    r.vignette = jsonNumber(j, "vignette", r.vignette);
    r.aperture = jsonNumber(j, "aperture", r.aperture);
    r.shutter = jsonNumber(j, "shutter", r.shutter);
    r.sensitivity = jsonNumber(j, "sensitivity", r.sensitivity);
    if (j.contains("tonemap") && j["tonemap"].is_string()) {
        r.tonemap = j["tonemap"].get<std::string>();
    }
    r.contrast = jsonNumber(j, "contrast", r.contrast);
    r.saturation = jsonNumber(j, "saturation", r.saturation);
    r.temperature = jsonNumber(j, "temperature", r.temperature);
    r.tint = jsonNumber(j, "tint", r.tint);
    return clampRender(r);
}

inline nlohmann::json toJson(const SimSettings& s) {
    nlohmann::json j;
    j["gravity"] = s.gravity;
    j["gravityX"] = s.gravityX;
    j["gravityZ"] = s.gravityZ;
    j["hz"] = s.hz;
    j["substeps"] = s.substeps;
    j["iterations"] = s.iterations;
    j["envelope"] = s.envelope;
    j["recovery"] = s.recovery;
    j["friction"] = s.friction;
    j["restitution"] = s.restitution;
    j["linearDamping"] = s.linearDamping;
    j["angularDamping"] = s.angularDamping;
    j["sleeping"] = s.sleeping;
    j["integrator"] = s.integrator;
    j["solver"] = s.solver;
    j["combine"] = s.combine;
    j["contact"] = s.contact;
    j["young"] = s.young;
    j["poisson"] = s.poisson;
    j["modal"] = s.modal;
    return j;
}

inline SimSettings simFromJson(const nlohmann::json& j, const SimSettings& base) {
    SimSettings s = base;
    if (!j.is_object()) return s;
    s.gravity = jsonNumber(j, "gravity", s.gravity);
    s.gravityX = jsonNumber(j, "gravityX", s.gravityX);
    s.gravityZ = jsonNumber(j, "gravityZ", s.gravityZ);
    s.hz = j.value("hz", s.hz);
    s.substeps = j.value("substeps", s.substeps);
    s.iterations = j.value("iterations", s.iterations);
    s.envelope = j.value("envelope", s.envelope);
    s.recovery = j.value("recovery", s.recovery);
    s.friction = j.value("friction", s.friction);
    s.restitution = j.value("restitution", s.restitution);
    s.linearDamping = j.value("linearDamping", s.linearDamping);
    s.angularDamping = j.value("angularDamping", s.angularDamping);
    s.sleeping = j.value("sleeping", s.sleeping);
    if (j.contains("integrator") && j["integrator"].is_string()) s.integrator = j["integrator"];
    if (j.contains("solver") && j["solver"].is_string()) s.solver = j["solver"];
    if (j.contains("combine") && j["combine"].is_string()) s.combine = j["combine"];
    if (j.contains("contact") && j["contact"].is_string()) s.contact = j["contact"];
    s.young = jsonNumber(j, "young", s.young);
    s.poisson = jsonNumber(j, "poisson", s.poisson);
    s.modal = jsonInt(j, "modal", s.modal);
    return s;
}

inline nlohmann::json toJson(const GizmoSettings& g) {
    nlohmann::json j;
    j["mode"] = gizmoModeName(g.mode);
    j["space"] = gizmoSpaceName(g.space);
    j["snap"] = g.snap;
    j["moveStep"] = g.moveStep;
    j["rotateStep"] = g.rotateStep;
    j["scaleStep"] = g.scaleStep;
    j["grid"] = g.grid;
    j["gridStep"] = g.gridStep;
    return j;
}

inline GizmoSettings gizmoFromJson(const nlohmann::json& j,
                                   const GizmoSettings& base) {
    GizmoSettings g = base;
    if (!j.is_object()) return g;
    if (j.contains("mode") && j["mode"].is_string()) {
        g.mode = gizmoModeFromName(j["mode"], g.mode);
    }
    if (j.contains("space") && j["space"].is_string()) {
        g.space = gizmoSpaceFromName(j["space"], g.space);
    }
    g.snap = j.value("snap", g.snap);
    g.moveStep = j.value("moveStep", g.moveStep);
    g.rotateStep = j.value("rotateStep", g.rotateStep);
    g.scaleStep = j.value("scaleStep", g.scaleStep);
    g.grid = j.value("grid", g.grid);
    g.gridStep = j.value("gridStep", g.gridStep);
    // 刻みが 0 だとスナップの割り算が壊れる。
    if (g.moveStep < 1e-4) g.moveStep = 0.25;
    if (g.rotateStep < 1e-4) g.rotateStep = 15.0;
    if (g.scaleStep < 1e-4) g.scaleStep = 0.1;
    // グリッドの間隔。下限は使い勝手（100m 幅で 0.25m だと 800 本 =
    // これ以上細かくしてもモアレで見えない）。
    if (g.gridStep < 0.25) g.gridStep = 0.25;
    if (g.gridStep > 10.0) g.gridStep = 10.0;
    return g;
}

// UI から来た値の常識的な範囲。手書きリクエスト対策のクランプであって、
// チューニングの推奨値ではない。
inline SimSettings clampSim(SimSettings s) {
    auto cl = [](auto v, auto lo, auto hi) { return v < lo ? lo : (v > hi ? hi : v); };
    s.gravity = cl(s.gravity, -100.0, 100.0);
    s.gravityX = cl(s.gravityX, -100.0, 100.0);
    s.gravityZ = cl(s.gravityZ, -100.0, 100.0);
    s.hz = cl(s.hz, 10, 240);
    s.substeps = cl(s.substeps, 1, 8);
    s.iterations = cl(s.iterations, 1, 2000);
    s.envelope = cl(s.envelope, 1e-4, 0.2);
    s.recovery = cl(s.recovery, 0.0, 10.0);
    s.friction = cl(s.friction, 0.0f, 2.0f);
    s.restitution = cl(s.restitution, 0.0f, 1.0f);
    s.linearDamping = cl(s.linearDamping, 0.0, 10.0);
    s.angularDamping = cl(s.angularDamping, 0.0, 10.0);
    // 名前の打ち間違いは既定へ（読み込み側が警告を出す。ここは最後の関所）。
    if (!integratorNameValid(s.integrator)) s.integrator = "euler";
    if (!solverNameValid(s.solver)) s.solver = "bb";
    if (!combineNameValid(s.combine)) s.combine = "min";
    if (!contactNameValid(s.contact)) s.contact = "nsc";
    s.young = cl(s.young, 1000.0, 1.0e12);
    s.poisson = cl(s.poisson, 0.0, 0.49);
    s.modal = cl(s.modal, 0, 64);
    return s;
}

// 置ける大きさ・質量の範囲。0 やマイナスは Chrono を壊すのでここで止める。
inline BodyDesc clampBody(BodyDesc b) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    b.size.x = cl(b.size.x, 0.01, 50.0);
    b.size.y = cl(b.size.y, 0.01, 50.0);
    b.size.z = cl(b.size.z, 0.01, 50.0);
    b.mass = cl(b.mass, 0.001, 100000.0);
    b.position.x = cl(b.position.x, -500.0, 500.0);
    b.position.y = cl(b.position.y, -500.0, 500.0);
    b.position.z = cl(b.position.z, -500.0, 500.0);
    b.soft = clampSoft(b.soft);
    b.material = clampMaterial(b.material);
    b.surface = clampSurface(b.surface);
    b.layer = int(cl(double(b.layer), 0.0, double(kCollisionLayers - 1)));
    {
        // レイヤ一覧: 範囲内の値だけ、重複なし、昇順。
        std::vector<int> nc;
        for (int v : b.nocollide) {
            if (v < 0 || v >= kCollisionLayers) continue;
            if (std::find(nc.begin(), nc.end(), v) == nc.end()) nc.push_back(v);
        }
        std::sort(nc.begin(), nc.end());
        b.nocollide = nc;
    }
    b.velocity.x = cl(b.velocity.x, -1000.0, 1000.0);
    b.velocity.y = cl(b.velocity.y, -1000.0, 1000.0);
    b.velocity.z = cl(b.velocity.z, -1000.0, 1000.0);
    b.angularVelocity.x = cl(b.angularVelocity.x, -36000.0, 36000.0);
    b.angularVelocity.y = cl(b.angularVelocity.y, -36000.0, 36000.0);
    b.angularVelocity.z = cl(b.angularVelocity.z, -36000.0, 36000.0);
    for (double* v : {&b.force.x, &b.force.y, &b.force.z}) *v = cl(*v, -1.0e7, 1.0e7);
    for (double* v : {&b.torque.x, &b.torque.y, &b.torque.z}) *v = cl(*v, -1.0e7, 1.0e7);
    // Trimesh は当たり判定だけの値。見た目に書かれたらメッシュ（名前が
    // あれば）か箱へ。当たり判定の trimesh はメッシュ形状にしか付かない。
    if (b.shape == ShapeKind::Trimesh) {
        b.shape = b.mesh.empty() ? ShapeKind::Box : ShapeKind::Model;
        if (b.shape == ShapeKind::Box && b.collision == ShapeKind::Trimesh) {
            b.collision = ShapeKind::Box;
        }
    }
    if (b.collision == ShapeKind::Trimesh && b.shape != ShapeKind::Model) {
        b.collision = b.shape;
    }
    // ソフトボディの格子は箱か球。凸包 / 三角メッシュの当たり判定は付かない。
    if (b.hasSoft && (b.collision == ShapeKind::Trimesh || b.collision == ShapeKind::Model)) {
        b.collision = ShapeKind::Box;
    }
    // geom の無いボディ: 当たり判定も無し（部品だけ）。ソフトボディにはできない
    // （格子は箱か球）ので箱へ倒す。
    if (b.hasSoft && b.shape == ShapeKind::None) {
        b.shape = ShapeKind::Box;
        b.collision = ShapeKind::Box;
    }
    if (b.shape == ShapeKind::None) b.collision = ShapeKind::None;
    if (b.collision == ShapeKind::None && b.shape != ShapeKind::None) b.collision = b.shape;
    return b;
}

// ライトの常識的な範囲。0 やマイナスの強さ・範囲は Filament 側で意味を持たない。
inline LightDesc clampLight(LightDesc l) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    l.position.x = cl(l.position.x, -500.0, 500.0);
    l.position.y = cl(l.position.y, -500.0, 500.0);
    l.position.z = cl(l.position.z, -500.0, 500.0);
    l.intensity = cl(l.intensity, 0.0, 10000000.0);
    l.falloff = cl(l.falloff, 0.1, 500.0);
    l.spotInnerDeg = cl(l.spotInnerDeg, 1.0, 88.0);
    l.spotOuterDeg = cl(l.spotOuterDeg, l.spotInnerDeg, 89.0);
    return l;
}

// 地面・環境光の常識的な範囲。0 や負の広さは描画も物理も壊す。
inline GroundDesc clampGround(GroundDesc g) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    g.half = cl(g.half, 1.0, 1000.0);
    g.visualHalf = cl(g.visualHalf, 1.0, 1000.0);
    g.tile = cl(g.tile, 0.1, 100.0);
    g.roughness = cl(g.roughness, 0.02, 1.0);
    g.metallic = cl(g.metallic, 0.0, 1.0);
    return g;
}
inline EnvironmentDesc clampEnvironment(EnvironmentDesc e) {
    if (e.intensity < 0.0) e.intensity = 0.0;
    if (e.intensity > 10000000.0) e.intensity = 10000000.0;
    return e;
}

// ノードの常識的な範囲。タイマーの下限が一番大事: 0 に近いと毎ステップ
// 発火して、繋いだアクション（力・色）が暴走する。
inline NodeDesc clampNode(NodeDesc n) {
    auto cl = [](double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    n.x = cl(n.x, 0.0, 4000.0);
    n.y = cl(n.y, 0.0, 4000.0);
    if (n.target < -1) n.target = -1;
    if (n.other < -2) n.other = -2;
    // -2（何でも）は OnCollision の相手フィルタだけの語彙。CameraLookAt の
    // other は注視先のオブジェクト番号なので、-1（未設定）が下限。
    if (n.kind == NodeKind::CameraLookAt && n.other < -1) n.other = -1;
    n.seconds = cl(n.seconds, 0.05, 3600.0);
    // OnCollision の vec は「求める接触の向き」（零 = 問わない）。NodeDesc の
    // 既定 (0, 5, 0) は ApplyImpulse のためのものなので、そのまま残すと作った
    // ばかりの衝突トリガーが「上から乗ったとき」だけになる。既定値のままなら
    // 零へ倒す（旧 JSON の衝突ノードもこれで従来どおり何でも拾う）。
    if (n.kind == NodeKind::OnCollision && n.vec.x == 0.0 && n.vec.y == 5.0 &&
        n.vec.z == 0.0) {
        n.vec = Vec3d{0.0, 0.0, 0.0};
    }
    n.vec.x = cl(n.vec.x, -100.0, 100.0);
    n.vec.y = cl(n.vec.y, -100.0, 100.0);
    n.vec.z = cl(n.vec.z, -100.0, 100.0);
    // value の意味は種類ごと: SetFixed は 0/1、GrabPull は強さの倍率
    // （0 以下 = 既定の 1 倍）、SetLightIntensity はルーメン。
    if (n.kind == NodeKind::SetFixed || n.kind == NodeKind::SetVelocity) {
        n.value = n.value != 0.0 ? 1.0 : 0.0;
    } else if (n.kind == NodeKind::SetMotor) {
        n.value = cl(n.value, -1000000.0, 1000000.0);  // モータの目標値は負もある
    } else if (n.kind == NodeKind::GrabPull) {
        n.value = n.value <= 0.0 ? 1.0 : cl(n.value, 0.1, 10.0);
    } else {
        n.value = cl(n.value, 0.0, 10000000.0);
    }
    return n;
}

}  // namespace editor
}  // namespace wizengine
