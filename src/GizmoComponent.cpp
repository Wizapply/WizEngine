#include "GizmoComponent.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "Log.h"
#include "Renderer.h"
#include "Scene.h"
#include "scene_math.h"

namespace ed = wizengine::editor;
using scenemath::Vec3;

namespace {

// ---- ハンドル番号 -----------------------------------------------------------
// モード（移動/回転/拡縮）と組み合わせて意味が決まる。たとえば AxisX は移動なら
// 「X に沿って動かす」、回転なら「X 軸まわりに回す」。
constexpr int kNone = -1;
constexpr int kAxisX = 0, kAxisY = 1, kAxisZ = 2;
constexpr int kPlaneYZ = 3, kPlaneZX = 4, kPlaneXY = 5;  // 移動のみ
constexpr int kUniform = 6;                              // 拡縮のみ

// 平面ハンドルの法線がどの軸か。kPlaneYZ の法線は X。
int planeNormalAxis(int handle) { return handle - kPlaneYZ; }

// バッチ番号（色）。gizmo::batchColors() の並びと一致させること。
constexpr std::size_t kBatchX = 0, kBatchY = 1, kBatchZ = 2;
constexpr std::size_t kBatchNeutral = 3, kBatchActive = 4;
constexpr std::size_t kBatchLight = 5, kBatchCam = 6;

// ---- 見た目の定数 -----------------------------------------------------------
// 画面上でだいたい一定の大きさに見せるため、ギズモの長さはカメラからの距離に
// 比例させる（Unity と同じ考え方）。0.16 で画面の高さの 2 割弱。
constexpr double kScreenSize = 0.16;
constexpr double kMinLength = 0.05;
constexpr double kMaxLength = 20.0;
// 線の太さ（ギズモの長さに対する割合）。ギズモ自体がカメラからの距離に
// 比例しているので、これで画面上の太さが一定になる。0.03 でだいたい
// 720p の 4 ピクセル、1080p の 6 ピクセル。太さを変えたいのはここ。
constexpr double kThickness = 0.030;
// 回転リングは本数が多いぶん、同じ太さだと重たく見えるので少し細く。
constexpr double kRingThickness = 0.024;
// 当たり判定の太さ（NDC）。画面の高さの約 2%。細い線を狙わせすぎない。
constexpr double kPickAxis = 0.035;
constexpr double kPickBlob = 0.05;
constexpr int kRingSegments = 48;

// 軸の始まり（中心に近すぎると回転リングや一様ハンドルと取り合いになる）。
constexpr double kShaftStart = 0.15;
constexpr double kRingRadius = 0.85;
constexpr double kPlaneNear = 0.25, kPlaneFar = 0.45;

// ---- ギズモの座標系 ---------------------------------------------------------
struct Frame {
    bool valid = false;
    Vec3 origin;
    Vec3 axis[3];
    double length = 1.0;
};

Frame makeFrame(const ed::Vec3d& position, const ed::Vec3d& rotation,
                ed::GizmoSpace space, const scenemath::Basis& basis) {
    Frame f;
    if (!basis.valid) return f;
    f.origin = Vec3(position.x, position.y, position.z);

    if (space == ed::GizmoSpace::Local) {
        const scenemath::Quat q =
            scenemath::quatFromEulerDegrees(rotation.x, rotation.y, rotation.z);
        f.axis[0] = q * Vec3::UnitX();
        f.axis[1] = q * Vec3::UnitY();
        f.axis[2] = q * Vec3::UnitZ();
    } else {
        f.axis[0] = Vec3::UnitX();
        f.axis[1] = Vec3::UnitY();
        f.axis[2] = Vec3::UnitZ();
    }

    const double distance = (f.origin - basis.eye).norm();
    f.length = std::max(kMinLength, std::min(kMaxLength, distance * kScreenSize));
    f.valid = true;
    return f;
}

// ---- 幾何の組み立て ---------------------------------------------------------
// 太線（線分＋太さ）と塗りつぶしの面を、色（＝バッチ）ごとに積んでいく。
// 太さは 1 本ごとに持たせる: バッチは全カメラで共有だが、太さはそのギズモを
// 見ているカメラからの距離で決まるため。
struct Sink {
    std::vector<std::vector<wizengine::BatchShape>>* out = nullptr;
    float width = 0.0f;  // いま組み立てているギズモの線の太さ

    static filament::math::float3 f3(const Vec3& v) {
        return {float(v.x()), float(v.y()), float(v.z())};
    }
    void push(std::size_t batch, const wizengine::BatchShape& s) const {
        if (!out || batch >= out->size()) return;
        (*out)[batch].push_back(s);
    }
    // 太線 1 本。
    void line(std::size_t batch, const Vec3& a, const Vec3& b) const {
        wizengine::BatchShape s;
        s.a = f3(a);
        s.b = f3(b);
        s.width = width;
        push(batch, s);
    }
    // 塗りつぶしの四角（隅は一周する順で）。
    void quad(std::size_t batch, const Vec3& a, const Vec3& b, const Vec3& c,
              const Vec3& d) const {
        wizengine::BatchShape s;
        s.a = f3(a);
        s.b = f3(b);
        s.c = f3(c);
        s.d = f3(d);
        push(batch, s);
    }
    // 塗りつぶしの三角（潰れた四角として渡す）。
    void tri(std::size_t batch, const Vec3& a, const Vec3& b,
             const Vec3& c) const {
        quad(batch, a, b, c, c);
    }
};

void segment(const Sink& out, std::size_t batch, const Vec3& a, const Vec3& b) {
    out.line(batch, a, b);
}

// 中心 c、半サイズ h の塗りつぶした直方体（6 面）。拡縮ハンドルのつまみ。
// マテリアルは両面描画なので、面の向き（巻き方）は気にしなくてよい。
void solidBox(const Sink& out, std::size_t batch, const Vec3& c,
              const Vec3 axis[3], double h) {
    Vec3 corner[8];
    for (int i = 0; i < 8; ++i) {
        const double sx = (i & 1) ? h : -h;
        const double sy = (i & 2) ? h : -h;
        const double sz = (i & 4) ? h : -h;
        corner[i] = c + axis[0] * sx + axis[1] * sy + axis[2] * sz;
    }
    // 各面の 4 隅を一周する順に。
    static const int faces[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5},   // ∓X
                                    {0, 1, 5, 4}, {2, 6, 7, 3},   // ∓Y
                                    {0, 2, 3, 1}, {4, 5, 7, 6}};  // ∓Z
    for (const auto& f : faces) {
        out.quad(batch, corner[f[0]], corner[f[1]], corner[f[2]], corner[f[3]]);
    }
}

// 塗りつぶした円錐（矢じり）。tip が先端、base が底面の中心、u/v が底面を
// 張る 2 方向。側面と底面のふたを三角形で埋める。
void solidCone(const Sink& out, std::size_t batch, const Vec3& tip,
               const Vec3& base, const Vec3& u, const Vec3& v, double radius) {
    constexpr int kSides = 12;
    Vec3 prev = base + u * radius;
    for (int k = 1; k <= kSides; ++k) {
        const double t = 2.0 * scenemath::kPi * double(k) / kSides;
        const Vec3 p =
            base + u * (radius * std::cos(t)) + v * (radius * std::sin(t));
        out.tri(batch, tip, prev, p);   // 側面
        out.tri(batch, base, prev, p);  // 底のふた
        prev = p;
    }
}

// 掴んでいるハンドルはハイライト色のバッチへ回す。
std::size_t batchFor(int handle, int active, std::size_t normal) {
    return handle == active && active != kNone ? kBatchActive : normal;
}

void buildTranslate(const Sink& out, const Frame& f, int active) {
    const double L = f.length;
    for (int i = 0; i < 3; ++i) {
        const Vec3& a = f.axis[i];
        const Vec3& u = f.axis[(i + 1) % 3];
        const Vec3& v = f.axis[(i + 2) % 3];
        const std::size_t b = batchFor(i, active, std::size_t(i));

        const Vec3 from = f.origin + a * (L * kShaftStart);
        const Vec3 tip = f.origin + a * L;
        segment(out, b, from, tip);
        // 矢じりは塗りつぶした円錐。骨組みだけだと、線を太くしたぶん
        // かえって中身が透けて見えてしまう。塗るなら少し太短いほうが
        // 矢印らしく見える（長さ 0.22L に対して半径 0.09L）。
        solidCone(out, b, tip, f.origin + a * (L * 0.78), u, v, L * 0.09);
    }

    // 平面ハンドル: 2 軸が張る小さな四角。色はその平面の法線軸に合わせる。
    // 面を塗りつぶしたうえで、縁を太線でなぞって輪郭を出す。
    for (int i = 0; i < 3; ++i) {
        const int handle = kPlaneYZ + i;
        const Vec3& u = f.axis[(i + 1) % 3];
        const Vec3& v = f.axis[(i + 2) % 3];
        const std::size_t b = batchFor(handle, active, std::size_t(i));
        const Vec3 p00 = f.origin + u * (L * kPlaneNear) + v * (L * kPlaneNear);
        const Vec3 p10 = f.origin + u * (L * kPlaneFar) + v * (L * kPlaneNear);
        const Vec3 p11 = f.origin + u * (L * kPlaneFar) + v * (L * kPlaneFar);
        const Vec3 p01 = f.origin + u * (L * kPlaneNear) + v * (L * kPlaneFar);
        out.quad(b, p00, p10, p11, p01);
        segment(out, b, p00, p10);
        segment(out, b, p10, p11);
        segment(out, b, p11, p01);
        segment(out, b, p01, p00);
    }
}

void buildRotate(const Sink& out, const Frame& f, int active) {
    const double r = f.length * kRingRadius;
    for (int i = 0; i < 3; ++i) {
        const Vec3& u = f.axis[(i + 1) % 3];
        const Vec3& v = f.axis[(i + 2) % 3];
        const std::size_t b = batchFor(i, active, std::size_t(i));
        Vec3 prev = f.origin + u * r;
        for (int s = 1; s <= kRingSegments; ++s) {
            const double t = 2.0 * scenemath::kPi * double(s) / kRingSegments;
            const Vec3 p = f.origin + u * (r * std::cos(t)) + v * (r * std::sin(t));
            segment(out, b, prev, p);
            prev = p;
        }
    }
}

void buildScale(const Sink& out, const Frame& f, int active) {
    const double L = f.length;
    for (int i = 0; i < 3; ++i) {
        const Vec3& a = f.axis[i];
        const std::size_t b = batchFor(i, active, std::size_t(i));
        const Vec3 from = f.origin + a * (L * kShaftStart);
        const Vec3 knob = f.origin + a * (L * 0.9);
        segment(out, b, from, knob);
        solidBox(out, b, knob, f.axis, L * 0.055);
    }
    // 中心の箱は一様拡縮。灰色にしておくと軸と取り違えない。
    solidBox(out, batchFor(kUniform, active, kBatchNeutral), f.origin, f.axis,
             L * 0.075);
}

// ---- グリッド ---------------------------------------------------------------
// Y=0 に敷く格子。エディタで物を置くときの目印（Unity のシーングリッド相当）。
// 作業の目安なのでポリゴン（太線）にはせず、1 ピクセルの LINES で描く
// （Renderer::setLineSet の細線セット）。頂点は 1 本 2 個 = 太線の 1/4 で、
// 塗りのコストも無い。原点を通る 2 本だけは軸の色（X=赤 / Z=青）。
//
// グリッドの広さ（半分）: 100×100 m。見える地面（16×16 m）や物理の床
// （20×20 m）より広い、純粋な作業目安。SceneConfig.h は scene.cpp 専用と
// いう約束なので、ここから直接は読まない。
constexpr double kGridHalf = 50.0;
constexpr double kGridLift = 0.01;  // 地面と同じ高さだと Z ファイトする

void buildGridPoints(std::vector<filament::math::float3>& gray,
                     std::vector<filament::math::float3>& xAxis,
                     std::vector<filament::math::float3>& zAxis, double step) {
    const int n = int(kGridHalf / step + 0.5);
    auto f3 = [](double x, double y, double z) {
        return filament::math::float3{float(x), float(y), float(z)};
    };
    gray.reserve(std::size_t(n) * 8);
    for (int i = -n; i <= n; ++i) {
        if (i == 0) continue;  // 軸線は色付きで別のセットに
        const double p = i * step;
        gray.push_back(f3(p, kGridLift, -kGridHalf));
        gray.push_back(f3(p, kGridLift, kGridHalf));
        gray.push_back(f3(-kGridHalf, kGridLift, p));
        gray.push_back(f3(kGridHalf, kGridLift, p));
    }
    xAxis = {f3(-kGridHalf, kGridLift, 0.0), f3(kGridHalf, kGridLift, 0.0)};
    zAxis = {f3(0.0, kGridLift, -kGridHalf), f3(0.0, kGridLift, kGridHalf)};
}

// ---- ライト・カメラのアイコン ------------------------------------------------
// Unity のシーンビューに出るアイコン相当。ギズモと同じ太線バッチに描くので
// エディタ専用レイヤに乗り、エディタカメラのビューにだけ映る。大きさは
// カメラからの距離に比例（画面上でほぼ一定の見かけ）。
constexpr double kIconScreenSize = 0.035;  // 画面の高さに対する見かけの半径
constexpr double kIconPick = 0.06;         // アイコン中心の当たり判定（NDC）

double iconSize(const Vec3& p, const scenemath::Basis& basis) {
    const double d = (p - basis.eye).norm();
    return std::max(0.02, std::min(5.0, d * kIconScreenSize));
}

// ビルボードの円（見ているカメラに正対する円）。どの向きから見ても同じ形に
// 見えるのが、目印としては一番分かりやすい。
void billboardCircle(const Sink& out, std::size_t batch, const Vec3& center,
                     const scenemath::Basis& basis, double r, int segments) {
    Vec3 prev = center + basis.right * r;
    for (int s = 1; s <= segments; ++s) {
        const double t = 2.0 * scenemath::kPi * double(s) / segments;
        const Vec3 p = center + basis.right * (r * std::cos(t)) +
                       basis.up * (r * std::sin(t));
        out.line(batch, prev, p);
        prev = p;
    }
}

// dir に直交する 2 方向。円錐の底面や矢じりを張るのに使う。
void orthoPair(const Vec3& dir, Vec3& u, Vec3& v) {
    u = dir.cross(Vec3::UnitY());
    if (u.norm() < 1e-6) u = dir.cross(Vec3::UnitX());
    u = u.normalized();
    v = dir.cross(u).normalized();
}

void buildLightIcon(Sink out, std::size_t batch, const ed::LightDesc& d,
                    const scenemath::Basis& basis) {
    const Vec3 p(d.position.x, d.position.y, d.position.z);
    const double s = iconSize(p, basis);
    out.width = float(s * 0.10);
    const Vec3 dir =
        scenemath::lightDirection(d.rotation.x, d.rotation.y, d.rotation.z);

    if (d.kind == ed::LightKind::Point) {
        // 電球: 円 + 全方向の短い光線（ビルボード面内）。向きは無い。
        billboardCircle(out, batch, p, basis, s * 0.45, 12);
        for (int k = 0; k < 8; ++k) {
            const double t = 2.0 * scenemath::kPi * double(k) / 8.0;
            const Vec3 r = basis.right * std::cos(t) + basis.up * std::sin(t);
            out.line(batch, p + r * (s * 0.6), p + r * (s * 0.95));
        }
        return;
    }
    if (d.kind == ed::LightKind::Spot) {
        // 懐中電灯: 小さな丸 + 向いた先へ開く円錐。
        billboardCircle(out, batch, p, basis, s * 0.3, 10);
        Vec3 u, v;
        orthoPair(dir, u, v);
        const Vec3 base = p + dir * (s * 2.2);
        const double r = s * 0.9;
        Vec3 prev = base + u * r;
        for (int k = 1; k <= 12; ++k) {
            const double t = 2.0 * scenemath::kPi * double(k) / 12.0;
            const Vec3 q =
                base + u * (r * std::cos(t)) + v * (r * std::sin(t));
            out.line(batch, prev, q);
            prev = q;
        }
        for (int k = 0; k < 4; ++k) {
            const double t = 2.0 * scenemath::kPi * double(k) / 4.0;
            const Vec3 q =
                base + u * (r * std::cos(t)) + v * (r * std::sin(t));
            out.line(batch, p, q);
        }
        return;
    }
    // Sun（平行光）: 円 + 周囲の光線 + 向きを示す長めの矢印。
    billboardCircle(out, batch, p, basis, s * 0.45, 12);
    for (int k = 0; k < 8; ++k) {
        const double t = 2.0 * scenemath::kPi * double(k) / 8.0;
        const Vec3 r = basis.right * std::cos(t) + basis.up * std::sin(t);
        out.line(batch, p + r * (s * 0.6), p + r * (s * 0.9));
    }
    const Vec3 tip = p + dir * (s * 2.4);
    out.line(batch, p, tip);
    Vec3 u, v;
    orthoPair(dir, u, v);
    out.line(batch, tip, tip - dir * (s * 0.5) + u * (s * 0.25));
    out.line(batch, tip, tip - dir * (s * 0.5) - u * (s * 0.25));
}

void buildCameraIcon(Sink out, std::size_t batch, const CameraObject& cam,
                     const scenemath::Basis& viewBasis) {
    const auto e = cam.eye();
    const Vec3 p(e.x, e.y, e.z);
    const double s = iconSize(p, viewBasis);
    out.width = float(s * 0.10);

    // アイコンの向きは「そのカメラ」の向き（見ているカメラではなく）。
    const scenemath::Basis own = scenemath::cameraBasis(cam);
    if (!own.valid) return;
    const Vec3& f = own.forward;
    const Vec3& r = own.right;
    const Vec3& u = own.up;

    // 胴体（ワイヤーフレームの箱）。
    const double a = s * 0.55, b = s * 0.40, c = s * 0.70;
    Vec3 corner[8];
    for (int i = 0; i < 8; ++i) {
        const double sx = (i & 1) ? 1.0 : -1.0;
        const double sy = (i & 2) ? 1.0 : -1.0;
        const double sz = (i & 4) ? 1.0 : -1.0;
        corner[i] = p + r * (a * sx) + u * (b * sy) + f * (c * sz);
    }
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                     {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                     {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& eg : edges) {
        out.line(batch, corner[eg[0]], corner[eg[1]]);
    }

    // レンズ（前方に開く角錐）= どちらを向いているかの目印。
    const Vec3 front = p + f * c;
    const Vec3 far = p + f * (s * 1.7);
    const double lw = s * 0.5, lh = s * 0.35;
    const Vec3 q0 = far + r * lw + u * lh;
    const Vec3 q1 = far - r * lw + u * lh;
    const Vec3 q2 = far - r * lw - u * lh;
    const Vec3 q3 = far + r * lw - u * lh;
    out.line(batch, front, q0);
    out.line(batch, front, q1);
    out.line(batch, front, q2);
    out.line(batch, front, q3);
    out.line(batch, q0, q1);
    out.line(batch, q1, q2);
    out.line(batch, q2, q3);
    out.line(batch, q3, q0);

    // 上向きの三角（Unity のカメラアイコンと同じ「上」の目印）。
    const Vec3 t0 = p + u * b + r * (s * 0.3);
    const Vec3 t1 = p + u * b - r * (s * 0.3);
    const Vec3 t2 = p + u * (s * 0.8);
    out.line(batch, t0, t1);
    out.line(batch, t1, t2);
    out.line(batch, t2, t0);
}

// ---- ギズモの対象 -----------------------------------------------------------
// 選択がオブジェクトかライトかカメラかを 1 つの形に吸収する。姿勢の読み書きの
// 相手が違うだけで、ハンドルの見た目・当たり判定・ドラッグの計算は共通。
struct Target {
    bool valid = false;
    int type = 0;  // 0=オブジェクト 1=ライト 2=カメラ
    std::size_t index = 0;
    ed::Vec3d pos, rot, size;
    bool isObject() const { return type == 0; }
};

// ライト・カメラはスケールを持たないので、拡縮モードは移動として扱う
// （Unity も非対応の対象ではツールが効かないが、何も出ないより移動できる
// ほうが手が止まらない）。
ed::GizmoMode effectiveMode(const Target& t, ed::GizmoMode mode) {
    if (!t.isObject() && mode == ed::GizmoMode::Scale) {
        return ed::GizmoMode::Translate;
    }
    return mode;
}

// 現在の対象の設計値を読む。lockLists は INPUT スレッドからだけ true
// （PHYSICS は唯一の書き手、RENDER は applyToRenderer がロック済み）。
Target currentTarget(Scene& scene, std::size_t camIndex, bool lockLists) {
    Target t;
    std::unique_lock<std::mutex> lk;
    if (lockLists) lk = scene.lockObjects();

    const auto selKind = scene.editor().selKind();
    const int selIndex = scene.editor().selIndex();
    if (selKind == EditorState::SelKind::Light) {
        if (selIndex >= 0 && std::size_t(selIndex) < scene.lightCount() &&
            scene.lightAlive(std::size_t(selIndex))) {
            const ed::LightDesc& d =
                scene.lightItem(std::size_t(selIndex)).desc;
            t.valid = true;
            t.type = 1;
            t.index = std::size_t(selIndex);
            t.pos = d.position;
            t.rot = d.rotation;
        }
        return t;
    }
    if (selKind == EditorState::SelKind::Camera) {
        if (selIndex >= 0 && std::size_t(selIndex) < scene.cameraCount() &&
            scene.cameraActive(std::size_t(selIndex))) {
            t.valid = true;
            t.type = 2;
            t.index = std::size_t(selIndex);
            scene.cameraEditPose(t.index, t.pos, t.rot);
        }
        return t;
    }

    const std::size_t sel = scene.boxController(camIndex).selected();
    if (sel >= scene.objectCount() || !scene.objectAlive(sel)) return t;
    const ed::BodyDesc& d = scene.object(sel).desc;
    t.valid = true;
    t.type = 0;
    t.index = sel;
    t.pos = d.position;
    t.rot = d.rotation;
    t.size = d.size;
    return t;
}

// out は値渡し（ポインタ 1 本と float だけ）。太さをここで決めて配るため。
void buildGizmo(Sink out, const Frame& f, ed::GizmoMode mode, int active) {
    out.width = float(f.length * kThickness);
    switch (mode) {
        case ed::GizmoMode::Rotate:
            out.width = float(f.length * kRingThickness);
            buildRotate(out, f, active);
            break;
        case ed::GizmoMode::Scale: buildScale(out, f, active); break;
        case ed::GizmoMode::Translate: buildTranslate(out, f, active); break;
    }
}

// ---- 当たり判定 -------------------------------------------------------------
// NDC（画面）上で行う。3D で線に近いかを測ると、奥の軸ほど当たりにくくなって
// 「見えているとおりに掴めない」ので、見えている絵の上で判定する。
struct Projector {
    scenemath::Basis basis;
    double fov = 45.0;
    double aspect = 16.0 / 9.0;
    bool ndc(const Vec3& p, double& x, double& y) const {
        return scenemath::projectToNdc(basis, p, fov, aspect, x, y);
    }
};

// 線分の NDC 距離。どちらかの端点がカメラの後ろなら「当たらない」。
double segmentDistance(const Projector& pr, const Vec3& a, const Vec3& b,
                       double px, double py) {
    double ax, ay, bx, by;
    if (!pr.ndc(a, ax, ay) || !pr.ndc(b, bx, by)) return 1e9;
    return scenemath::distanceToSegment2D(px, py, ax, ay, bx, by);
}

double pointDistance(const Projector& pr, const Vec3& p, double px, double py) {
    double x, y;
    if (!pr.ndc(p, x, y)) return 1e9;
    return std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
}

int hitTest(const Frame& f, const Projector& pr, ed::GizmoMode mode, double px,
            double py) {
    if (!f.valid) return kNone;
    const double L = f.length;
    int best = kNone;
    double bestDistance = 1e9;
    auto consider = [&](int handle, double d, double limit) {
        if (d < limit && d < bestDistance) {
            best = handle;
            bestDistance = d;
        }
    };

    if (mode == ed::GizmoMode::Scale) {
        // 中心の一様ハンドルを先に見る。軸の根元と重なる位置にあるので、
        // 後回しにすると軸に取られてしまう。
        consider(kUniform, pointDistance(pr, f.origin, px, py), kPickBlob);
    }
    if (mode == ed::GizmoMode::Translate) {
        for (int i = 0; i < 3; ++i) {
            const Vec3& u = f.axis[(i + 1) % 3];
            const Vec3& v = f.axis[(i + 2) % 3];
            const double m = L * (kPlaneNear + kPlaneFar) * 0.5;
            const Vec3 center = f.origin + u * m + v * m;
            consider(kPlaneYZ + i, pointDistance(pr, center, px, py), kPickBlob);
        }
    }

    if (mode == ed::GizmoMode::Rotate) {
        const double r = L * kRingRadius;
        for (int i = 0; i < 3; ++i) {
            const Vec3& u = f.axis[(i + 1) % 3];
            const Vec3& v = f.axis[(i + 2) % 3];
            Vec3 prev = f.origin + u * r;
            double closest = 1e9;
            for (int s = 1; s <= kRingSegments; ++s) {
                const double t = 2.0 * scenemath::kPi * double(s) / kRingSegments;
                const Vec3 p =
                    f.origin + u * (r * std::cos(t)) + v * (r * std::sin(t));
                closest = std::min(closest, segmentDistance(pr, prev, p, px, py));
                prev = p;
            }
            consider(i, closest, kPickAxis);
        }
        return best;
    }

    // 移動と拡縮は同じ棒。長さだけ拡縮のほうが少し短い。
    const double tipScale = (mode == ed::GizmoMode::Scale) ? 0.9 : 1.0;
    for (int i = 0; i < 3; ++i) {
        const Vec3 a = f.origin + f.axis[i] * (L * kShaftStart);
        const Vec3 b = f.origin + f.axis[i] * (L * tipScale);
        consider(i, segmentDistance(pr, a, b, px, py), kPickAxis);
    }
    return best;
}

double snapTo(double value, double step) {
    if (step <= 1e-9) return value;
    return std::round(value / step) * step;
}

// アイコンの中心を NDC に投影して一番近いものを拾う。INPUT スレッド専用
// （中でオブジェクト一覧のロックを取る）。
struct IconHit {
    int type = 0;  // 1=ライト 2=カメラ
    int index = -1;
};
IconHit pickIcon(Scene& scene, const Projector& pr, double px, double py) {
    IconHit best;
    double bestDistance = kIconPick;
    auto lk = scene.lockObjects();
    for (std::size_t i = 0; i < scene.lightCount(); ++i) {
        if (!scene.lightAlive(i)) continue;
        const ed::Vec3d& p = scene.lightItem(i).desc.position;
        const double d = pointDistance(pr, Vec3(p.x, p.y, p.z), px, py);
        if (d < bestDistance) {
            bestDistance = d;
            best = {1, int(i)};
        }
    }
    for (std::size_t c = 0; c < scene.cameraCount(); ++c) {
        // エディタカメラ自身は選べない（自分の目の位置。アイコンも出ない）。
        if (!scene.cameraActive(c) || c == scene.editorCamera()) continue;
        const auto e = scene.camera(c).eye();
        const double d = pointDistance(pr, Vec3(e.x, e.y, e.z), px, py);
        if (d < bestDistance) {
            bestDistance = d;
            best = {2, int(c)};
        }
    }
    return best;
}

}  // namespace

namespace gizmo {

// 線の色（リニア RGB）。unlit のマテリアルでも Filament のトーンマッピングは
// 効く（ポストプロセスはシーン全体にかかる）。ACES は彩度の高い色を白側へ
// 寄せるので、素直に「赤 = (0.9, 0.2, 0.2)」と書くと画面では淡いピンク寄りの
// 赤になる。そのぶんを見越して**副次チャンネルをほぼ 0 まで落として**ある。
// 色を変えたいのはここ。薄く感じたら R/G/B のうち主役でない 2 つを下げる
// （主役を上げても白っぽくなるだけで、濃くはならない）。
const std::vector<filament::math::float3>& batchColors() {
    static const std::vector<filament::math::float3> colors = {
        {0.95f, 0.03f, 0.04f},  // X: 赤
        {0.09f, 0.78f, 0.07f},  // Y: 緑
        {0.05f, 0.20f, 1.00f},  // Z: 青
        {0.80f, 0.83f, 0.90f},  // 中立: 灰（暗い床に埋もれないよう明るめ）
        {1.00f, 0.66f, 0.00f},  // 掴んでいるハンドル: 山吹
        {0.92f, 0.86f, 0.20f},  // ライトのアイコン: 黄（Unity の電球風）
        {0.55f, 0.68f, 0.90f},  // カメラのアイコン: 水色がかった灰
    };
    return colors;
}

const std::vector<filament::math::float3>& gridColors() {
    static const std::vector<filament::math::float3> colors = {
        {0.42f, 0.45f, 0.52f},  // kGridSetGray: 控えめな灰（主役は物のほう）
        {0.95f, 0.03f, 0.04f},  // kGridSetX: 軸バッチと同じ赤
        {0.05f, 0.20f, 1.00f},  // kGridSetZ: 軸バッチと同じ青
    };
    return colors;
}

}  // namespace gizmo

// ---- カメラごとの状態 -------------------------------------------------------
struct GizmoComponent::Cam {
    // INPUT スレッドが書く。
    std::atomic<int> handle{kNone};
    std::atomic<bool> pointer{false};
    std::atomic<double> ndcX{0.0}, ndcY{0.0};
    std::atomic<bool> hasHover{false};
    std::atomic<double> hoverX{0.0}, hoverY{0.0};
    // RENDER スレッドだけが使う（今どのハンドルの上にカーソルがあるか）。
    int hovered = kNone;

    // PHYSICS スレッドだけが使うドラッグ状態。
    int active = kNone;             // 今処理しているハンドル
    int targetType = 0;             // 0=オブジェクト 1=ライト 2=カメラ
    std::size_t object = 0;         // 対象の番号（type に応じた一覧の中で）
    ed::Vec3d startPos, startRot, startSize;
    Frame frame;                    // 開始時の座標系（途中で動かさない）
    double startParam = 0.0;        // 軸上の位置 / 半径
    Vec3 startPoint{0, 0, 0};       // 平面ドラッグの基準点
    double prevAngle = 0.0;         // 回転: 直前の角度（巻き戻り検出用）
    double totalAngle = 0.0;        // 回転: 積算角
};

GizmoComponent::GizmoComponent() = default;
GizmoComponent::~GizmoComponent() = default;

void GizmoComponent::ensure(Scene& scene) {
    if (cams_.size() == scene.cameraCount()) return;
    cams_.clear();
    for (std::size_t i = 0; i < scene.cameraCount(); ++i) {
        cams_.push_back(std::make_unique<Cam>());
    }
}

bool GizmoComponent::onCommand(Scene& scene, std::size_t camIndex,
                               const nlohmann::json& msg) {
    ensure(scene);
    if (camIndex >= cams_.size()) return false;
    Cam& cam = *cams_[camIndex];
    const std::string cmd = msg.value("cmd", "");

    // カーソルの位置だけを送ってくる通知。掴んでいないときのハイライト用で、
    // これが無いと「どの軸を掴めるか」が押してみるまで分からない。
    if (cmd == "hover") {
        // ギズモ以外に用途が無いので、エディタカメラ以外のぶんは捨てる。
        if (camIndex != scene.editorCamera()) return true;
        cam.hoverX.store(msg.value("x", 0.0));
        cam.hoverY.store(msg.value("y", 0.0));
        cam.hasHover.store(true);
        return true;
    }

    // ギズモはエディタモード・エディタカメラ専用。それ以外の pick / drag /
    // release は素通しして、通常の選択・グラブ・カメラ操作に任せる。
    if (!scene.editor().isEditor() || camIndex != scene.editorCamera()) {
        return false;
    }

    if (cmd == "pick") {
        Projector pr;
        pr.basis = scenemath::cameraBasis(scene.camera(camIndex));
        pr.fov = scene.renderer().verticalFovDegrees();
        pr.aspect = scene.renderer().aspect();
        const double px = msg.value("x", 0.0);
        const double py = msg.value("y", 0.0);

        // 1) 選択中の対象（オブジェクト / ライト / カメラ）のギズモハンドル。
        //    当たっていればこのコマンドはここで止める（選択し直しをさせない）。
        const Target t = currentTarget(scene, camIndex, /*lockLists=*/true);
        if (t.valid) {
            const Frame f = makeFrame(t.pos, t.rot,
                                      scene.editor().gizmo().space, pr.basis);
            const int handle = hitTest(
                f, pr, effectiveMode(t, scene.editor().gizmo().mode), px, py);
            if (handle != kNone) {
                cam.handle.store(handle);
                cam.ndcX.store(px);
                cam.ndcY.store(py);
                cam.pointer.store(true);
                // 自由移動（グラブ）が同じフレームで動かないよう、カーソルを
                // 外しておく。以降 drag はこちらが受け取るので二重には動かない
                // が、直前の掴みが残っていると 1 フレームだけ引っぱられる。
                scene.boxController(camIndex).clearPointer();
                return true;
            }
        }

        // 2) ライト / カメラのアイコン。当たれば選択を切り替えて消費する
        //    （オブジェクトの選択は外す - ギズモの対象は常に 1 つ）。
        const IconHit hit = pickIcon(scene, pr, px, py);
        if (hit.index >= 0) {
            scene.boxController(camIndex).setSelected(BoxController::kNone);
            scene.editor().setSel(hit.type == 1 ? EditorState::SelKind::Light
                                                : EditorState::SelKind::Camera,
                                  hit.index);
            scene.boxController(camIndex).clearPointer();
            return true;
        }
        return false;  // 3) 通常のオブジェクト選択（pickBoxAt）へ
    }

    if (cmd == "drag" && cam.handle.load() != kNone) {
        cam.ndcX.store(msg.value("x", 0.0));
        cam.ndcY.store(msg.value("y", 0.0));
        cam.pointer.store(true);
        return true;  // グラブ（自由移動）には渡さない
    }

    if (cmd == "release" && cam.handle.load() != kNone) {
        cam.handle.store(kNone);
        cam.pointer.store(false);
        return true;
    }

    return false;
}

void GizmoComponent::onEditorStep(Scene& scene, double dt) {
    (void)dt;
    ensure(scene);
    const ed::GizmoSettings g = scene.editor().gizmo();

    for (std::size_t c = 0; c < cams_.size(); ++c) {
        Cam& cam = *cams_[c];
        // ハンドルはエディタカメラでしか立たない（onCommand が弾く）が、
        // 将来の変更に強いよう、ここでも明示しておく。
        if (c != scene.editorCamera()) {
            cam.active = kNone;
            continue;
        }
        const int handle = cam.handle.load();
        if (handle == kNone) {
            cam.active = kNone;
            continue;
        }
        if (!cam.pointer.load()) continue;

        // 対象（オブジェクト / ライト / カメラ）を統一の形で読む。物理
        // スレッドは一覧の唯一の書き手なのでロックは取らない。
        const Target t = currentTarget(scene, c, /*lockLists=*/false);
        if (!t.valid) continue;
        const ed::GizmoMode mode = effectiveMode(t, g.mode);

        const auto basis = scenemath::cameraBasis(scene.camera(c));
        if (!basis.valid) continue;
        const Vec3 dir = scenemath::rayThrough(
            basis, cam.ndcX.load(), cam.ndcY.load(),
            scene.renderer().verticalFovDegrees(), scene.renderer().aspect());

        // ドラッグ開始: 基準値を撮る。以後この値からの差分だけを見るので、
        // 適用結果が入力に混ざって暴走することがない。
        if (cam.active != handle || cam.targetType != t.type ||
            cam.object != t.index) {
            cam.active = handle;
            cam.targetType = t.type;
            cam.object = t.index;
            cam.startPos = t.pos;
            cam.startRot = t.rot;
            cam.startSize = t.size;
            cam.frame = makeFrame(t.pos, t.rot, g.space, basis);
            cam.prevAngle = 0.0;
            cam.totalAngle = 0.0;
            if (!cam.frame.valid) continue;

            if (handle >= kAxisX && handle <= kAxisZ) {
                if (mode == ed::GizmoMode::Rotate) {
                    const Vec3& a = cam.frame.axis[handle];
                    const double t =
                        scenemath::rayHitsPlane(basis.eye, dir, cam.frame.origin, a);
                    if (t < 0.0) { cam.active = kNone; continue; }
                    const Vec3 p = basis.eye + dir * t - cam.frame.origin;
                    const Vec3& u = cam.frame.axis[(handle + 1) % 3];
                    const Vec3 v = a.cross(u);
                    cam.prevAngle = std::atan2(p.dot(v), p.dot(u));
                } else {
                    double s = 0.0;
                    if (!scenemath::closestOnAxis(cam.frame.origin,
                                                  cam.frame.axis[handle],
                                                  basis.eye, dir, s)) {
                        cam.active = kNone;
                        continue;
                    }
                    cam.startParam = s;
                }
            } else if (handle >= kPlaneYZ && handle <= kPlaneXY) {
                const Vec3& n = cam.frame.axis[planeNormalAxis(handle)];
                const double t =
                    scenemath::rayHitsPlane(basis.eye, dir, cam.frame.origin, n);
                if (t < 0.0) { cam.active = kNone; continue; }
                cam.startPoint = basis.eye + dir * t;
            } else if (handle == kUniform) {
                // 一様拡縮は画面上の「中心からの距離」で決める。どの向きから
                // 見ても同じ操作感になる。
                Projector pr;
                pr.basis = basis;
                pr.fov = scene.renderer().verticalFovDegrees();
                pr.aspect = scene.renderer().aspect();
                double cx, cy;
                if (!pr.ndc(cam.frame.origin, cx, cy)) { cam.active = kNone; continue; }
                const double dx = cam.ndcX.load() - cx;
                const double dy = cam.ndcY.load() - cy;
                cam.startParam = std::max(1e-3, std::sqrt(dx * dx + dy * dy));
            }
            continue;  // 開始フレームでは動かさない
        }

        if (!cam.frame.valid) continue;
        const Frame& f = cam.frame;

        // ---- 移動 ---------------------------------------------------------
        if (mode == ed::GizmoMode::Translate) {
            Vec3 delta(0, 0, 0);
            if (handle >= kAxisX && handle <= kAxisZ) {
                double s = 0.0;
                if (!scenemath::closestOnAxis(f.origin, f.axis[handle],
                                              basis.eye, dir, s)) {
                    continue;
                }
                double along = s - cam.startParam;
                if (g.snap) along = snapTo(along, g.moveStep);
                delta = f.axis[handle] * along;
            } else if (handle >= kPlaneYZ && handle <= kPlaneXY) {
                const Vec3& n = f.axis[planeNormalAxis(handle)];
                const double hit = scenemath::rayHitsPlane(basis.eye, dir,
                                                           f.origin, n);
                if (hit < 0.0) continue;
                delta = basis.eye + dir * hit - cam.startPoint;
                if (g.snap) {
                    delta = Vec3(snapTo(delta.x(), g.moveStep),
                                 snapTo(delta.y(), g.moveStep),
                                 snapTo(delta.z(), g.moveStep));
                }
            } else {
                continue;
            }
            const double nx = cam.startPos.x + delta.x();
            const double ny = cam.startPos.y + delta.y();
            const double nz = cam.startPos.z + delta.z();
            if (t.type == 1) {
                scene.moveLight(t.index, nx, ny, nz);
            } else if (t.type == 2) {
                scene.moveCamera(t.index, nx, ny, nz);
            } else {
                scene.moveObject(t.index, nx, ny, nz);
            }
            continue;
        }

        // ---- 回転 ---------------------------------------------------------
        if (mode == ed::GizmoMode::Rotate) {
            if (handle < kAxisX || handle > kAxisZ) continue;
            const Vec3& a = f.axis[handle];
            const double hit = scenemath::rayHitsPlane(basis.eye, dir,
                                                       f.origin, a);
            if (hit < 0.0) continue;
            const Vec3 p = basis.eye + dir * hit - f.origin;
            const Vec3& u = f.axis[(handle + 1) % 3];
            const Vec3 v = a.cross(u);
            const double angle = std::atan2(p.dot(v), p.dot(u));

            // atan2 は ±π で折り返すので、差分を積み上げて連続な角度にする。
            double step = angle - cam.prevAngle;
            if (step > scenemath::kPi) step -= 2.0 * scenemath::kPi;
            if (step < -scenemath::kPi) step += 2.0 * scenemath::kPi;
            cam.prevAngle = angle;
            cam.totalAngle += step;

            double applied = cam.totalAngle;
            if (g.snap) {
                applied = scenemath::radians(
                    snapTo(applied * 180.0 / scenemath::kPi, g.rotateStep));
            }
            const scenemath::Quat start = scenemath::quatFromEulerDegrees(
                cam.startRot.x, cam.startRot.y, cam.startRot.z);
            const scenemath::Quat turn(Eigen::AngleAxisd(applied, a));
            const Vec3 euler = scenemath::eulerDegreesFromQuat(turn * start);
            if (t.type == 1) {
                scene.rotateLight(t.index, euler.x(), euler.y(), euler.z());
            } else if (t.type == 2) {
                // カメラはロールを持たない（オービット）。pitch/yaw だけ渡す。
                scene.rotateCamera(t.index, euler.x(), euler.y());
            } else {
                scene.rotateObject(t.index, euler.x(), euler.y(), euler.z());
            }
            continue;
        }

        // ---- 拡縮（オブジェクトのみ。ライト / カメラでは移動に丸めてある）--
        if (!t.isObject()) continue;
        double factor = 1.0;
        int axis = -1;
        if (handle >= kAxisX && handle <= kAxisZ) {
            double s = 0.0;
            if (!scenemath::closestOnAxis(f.origin, f.axis[handle], basis.eye,
                                          dir, s)) {
                continue;
            }
            if (std::abs(cam.startParam) < 1e-4) continue;
            factor = s / cam.startParam;
            axis = handle;
        } else if (handle == kUniform) {
            Projector pr;
            pr.basis = basis;
            pr.fov = scene.renderer().verticalFovDegrees();
            pr.aspect = scene.renderer().aspect();
            double cx, cy;
            if (!pr.ndc(f.origin, cx, cy)) continue;
            const double dx = cam.ndcX.load() - cx;
            const double dy = cam.ndcY.load() - cy;
            factor = std::sqrt(dx * dx + dy * dy) / cam.startParam;
        } else {
            continue;
        }
        factor = std::max(0.02, std::min(50.0, factor));

        // 球とモデルは 1 つの寸法しか持たないので、軸を掴んでも一様に効かせる。
        ed::Vec3d size = cam.startSize;
        const bool uniform =
            (axis < 0) ||
            scene.object(t.index).desc.shape != ed::ShapeKind::Box;
        if (uniform) {
            size.x = cam.startSize.x * factor;
            size.y = cam.startSize.y * factor;
            size.z = cam.startSize.z * factor;
        } else if (axis == 0) {
            size.x = cam.startSize.x * factor;
        } else if (axis == 1) {
            size.y = cam.startSize.y * factor;
        } else {
            size.z = cam.startSize.z * factor;
        }
        if (g.snap) {
            size.x = std::max(g.scaleStep, snapTo(size.x, g.scaleStep));
            size.y = std::max(g.scaleStep, snapTo(size.y, g.scaleStep));
            size.z = std::max(g.scaleStep, snapTo(size.z, g.scaleStep));
        }
        scene.resizeObject(t.index, size.x, size.y, size.z);
    }
}

void GizmoComponent::onRender(Scene& scene) {
    ensure(scene);
    const ed::GizmoSettings g = scene.editor().gizmo();
    const bool editing = scene.editor().isEditor();

    // ---- グリッド（細線セット）------------------------------------------
    // 頂点を作り直すのは表示状態か間隔が変わったときだけ。静止している
    // フレームでは何も送らない（作業目安にフレーム毎のコストを払わない）。
    const bool showGrid = editing && g.grid;
    if (showGrid != gridShown_ || (showGrid && gridStep_ != g.gridStep)) {
        gridShown_ = showGrid;
        gridStep_ = g.gridStep;
        std::vector<filament::math::float3> gray, xAxis, zAxis;
        if (showGrid) buildGridPoints(gray, xAxis, zAxis, g.gridStep);
        scene.renderer().setLineSet(gizmo::kGridSetGray, gray);
        scene.renderer().setLineSet(gizmo::kGridSetX, xAxis);
        scene.renderer().setLineSet(gizmo::kGridSetZ, zAxis);
    }

    const std::size_t batchCount = gizmo::batchColors().size();
    if (batches_.size() != batchCount) batches_.assign(batchCount, {});
    for (auto& b : batches_) b.clear();
    Sink sink;
    sink.out = &batches_;

    // シミュレート中は出さない（物が動いているのでハンドルを掴めない。
    // ライト / カメラのアイコンも編集できないモードで見せない）。
    if (editing) {
        Projector pr;
        pr.fov = scene.renderer().verticalFovDegrees();
        pr.aspect = scene.renderer().aspect();

        // ギズモとアイコンはエディタカメラのぶんだけ組み立てる。バッチは
        // エディタ専用レイヤに居るので、映るのもエディタカメラのビューだけ
        // （Renderer::setViewEditorLayerVisible）。他のカメラには一切出ない。
        const std::size_t c = scene.editorCamera();
        if (c < cams_.size()) {
            Cam& cam = *cams_[c];
            pr.basis = scenemath::cameraBasis(scene.camera(c));

            // ---- ライト・カメラのアイコン ---------------------------------
            // エディタ中は常に出す（クリックで選べることが見えるように）。
            // 選択中のものはハイライト色。エディタカメラ自身のアイコンは
            // 出さない（自分の目の位置なので、映るとしても画面の縁）。
            // RENDER スレッド: applyToRenderer が objectsMutex_ を握ったまま
            // 呼ぶので、一覧はロック無しで読める。
            if (pr.basis.valid) {
                const auto selKind = scene.editor().selKind();
                const int selIndex = scene.editor().selIndex();
                Sink icons;
                icons.out = &batches_;
                for (std::size_t i = 0; i < scene.lightCount(); ++i) {
                    if (!scene.lightAlive(i)) continue;
                    const bool on = selKind == EditorState::SelKind::Light &&
                                    selIndex == int(i);
                    buildLightIcon(icons, on ? kBatchActive : kBatchLight,
                                   scene.lightItem(i).desc, pr.basis);
                }
                for (std::size_t k = 0; k < scene.cameraCount(); ++k) {
                    if (!scene.cameraActive(k) || k == c) continue;
                    const bool on = selKind == EditorState::SelKind::Camera &&
                                    selIndex == int(k);
                    buildCameraIcon(icons, on ? kBatchActive : kBatchCam,
                                    scene.camera(k), pr.basis);
                }
            }

            // ---- 選択中の対象のギズモ -------------------------------------
            const Target t = currentTarget(scene, c, /*lockLists=*/false);
            if (t.valid && pr.basis.valid) {
                const ed::GizmoMode mode = effectiveMode(t, g.mode);
                const Frame f = makeFrame(t.pos, t.rot, g.space, pr.basis);
                if (f.valid) {
                    // 掴んでいるあいだはそのハンドル、掴んでいなければ
                    // カーソルの下のハンドルを光らせる。押す前にどこを
                    // 掴めるか分かるようにするため。
                    int active = cam.handle.load();
                    if (active == kNone && cam.hasHover.load()) {
                        cam.hovered = hitTest(f, pr, mode, cam.hoverX.load(),
                                              cam.hoverY.load());
                        active = cam.hovered;
                    }
                    buildGizmo(sink, f, mode, active);
                }
            } else {
                cam.hovered = kNone;
            }
        }
    }

    for (std::size_t i = 0; i < batches_.size(); ++i) {
        scene.renderer().setLineBatch(i, batches_[i]);
    }
}
