# 手元で .glb を作るための小さなモデリング API（CSG = manifold3d、書き出し = trimesh）。
#
# LLM が書くスクリプトも、人が書くスクリプトも、この API だけを使う:
#
#     def build(m):
#         wood = material("wood")
#         m.add(box(0.45, 0.04, 0.45, at=(0, 0.45, 0)), wood)
#
# 約束（シーン文書と同じ）: 単位はメートル、Y が上、角度は度。プリミティブは
# すべて「at = 中心」で置く（box の at は箱の中心、cylinder の at は軸の中点）。
# 色は sRGB（見たままの色、"#rrggbb" か 0..1 の 3 つ組）で書き、glTF に書く
# ときにリニアへ直す（glTF の baseColorFactor はリニア）。
#
# samples/v6_engine/meshlib.py と同じ流儀（CSG → 材質ごとにまとめて PBR の .glb）
# だが、こちらは依存を増やさないよう独立させてある。
import math

import numpy as np
import trimesh
from manifold3d import CrossSection, Manifold

# ---- 分割 ---------------------------------------------------------------------


def _seg(r):
    """半径に応じた円の分割数（小さい物は粗く、大きい物は細かく）。"""
    return int(max(16, min(72, 16 + r * 300)))


def _vec3(v, name="at"):
    if v is None:
        return (0.0, 0.0, 0.0)
    if isinstance(v, (int, float)):
        return (float(v), float(v), float(v))
    t = tuple(float(x) for x in v)
    if len(t) != 3:
        raise ValueError(f"{name} needs 3 numbers (x, y, z), got {len(t)}")
    return t


def _positive(name, *vals):
    for v in vals:
        if not (isinstance(v, (int, float)) and v > 0 and math.isfinite(v)):
            raise ValueError(f"{name}: sizes must be positive numbers, got {v!r}")


# 軸 Y の物（cylinder / cone など）を指定の軸へ向ける回転（度）。
_AXIS_FROM_Y = {"y": (0, 0, 0), "x": (0, 0, -90), "z": (90, 0, 0)}


def _axis(axis):
    a = str(axis).lower()
    if a not in _AXIS_FROM_Y:
        raise ValueError(f"axis must be 'x', 'y' or 'z', got {axis!r}")
    return a


# ---- 形 -----------------------------------------------------------------------


class Shape:
    """CSG の形。演算子 + （和）・ - （差）・ & （積）で組み合わせる。変換は新しい形を返す。"""

    def __init__(self, man):
        self.m = man

    # 変換
    def move(self, x=0.0, y=0.0, z=0.0):
        return Shape(self.m.translate([float(x), float(y), float(z)]))

    def rotate(self, x=0.0, y=0.0, z=0.0):
        """原点まわりの回転（度）。X → Y → Z の順に回す（R = Rz Ry Rx、シーン文書の euler と同じ）。"""
        return Shape(self.m.rotate([float(x), float(y), float(z)]))

    def rotate_about(self, point, x=0.0, y=0.0, z=0.0):
        px, py, pz = _vec3(point, "point")
        return self.move(-px, -py, -pz).rotate(x, y, z).move(px, py, pz)

    def scale(self, x, y=None, z=None):
        y = x if y is None else y
        z = x if z is None else z
        _positive("scale", x, y, z)
        return Shape(self.m.scale([float(x), float(y), float(z)]))

    def mirror(self, axis="x"):
        """その軸の向きを反転した写し（原点を通る面で鏡映）。"""
        n = {"x": [1, 0, 0], "y": [0, 1, 0], "z": [0, 0, 1]}[_axis(axis)]
        return Shape(self.m.mirror(n))

    def symmetric(self, axis="x"):
        """自分と鏡映の和（左右対称の脚・肘掛けなどを 1 本書けば済む）。"""
        return self + self.mirror(axis)

    # 演算
    def __add__(self, o):
        return Shape(self.m + _man(o))

    def __sub__(self, o):
        return Shape(self.m - _man(o))

    def __and__(self, o):
        return Shape(self.m ^ _man(o))

    def bounds(self):
        b = self.m.bounding_box()
        return (tuple(b[:3]), tuple(b[3:]))

    def is_empty(self):
        return self.m.is_empty()


def _man(s):
    if isinstance(s, Shape):
        return s.m
    raise TypeError(f"expected a shape, got {type(s).__name__}")


def _place(man, at):
    x, y, z = _vec3(at)
    return Shape(man.translate([x, y, z]))


def _orient(man, axis):
    rx, ry, rz = _AXIS_FROM_Y[_axis(axis)]
    return man.rotate([rx, ry, rz])


# ---- プリミティブ（すべて at = 中心）---------------------------------------------


def box(w, h, d, at=None):
    """幅 w (X)・高さ h (Y)・奥行き d (Z) の箱。"""
    _positive("box", w, h, d)
    return _place(Manifold.cube([w, h, d], True), at)


def rounded_box(w, h, d, r, at=None):
    """角を半径 r で丸めた箱（r は短い辺の半分まで）。"""
    _positive("rounded_box", w, h, d, r)
    r = min(r, 0.5 * min(w, h, d) * 0.999)
    hx, hy, hz = w / 2 - r, h / 2 - r, d / 2 - r
    seg = _seg(r)
    balls = [Manifold.sphere(r, seg).translate([sx * hx, sy * hy, sz * hz])
             for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    return _place(Manifold.batch_hull(balls), at)


def cylinder(r, h, at=None, axis="y"):
    """半径 r・長さ h の円柱。軸は axis（既定 Y = 縦）。"""
    _positive("cylinder", r, h)
    return _place(_orient(Manifold.cylinder(h, r, r, _seg(r), True).rotate([-90, 0, 0]), axis), at)


def cone(r1, r2, h, at=None, axis="y"):
    """下（軸の負の側）の半径 r1・上の半径 r2 の円錐台（r2 = 0 で尖った円錐）。"""
    _positive("cone", r1, h)
    if r2 < 0:
        raise ValueError("cone: r2 must be >= 0")
    m = Manifold.cylinder(h, r1, r2, _seg(max(r1, r2)), True).rotate([-90, 0, 0])
    return _place(_orient(m, axis), at)


def sphere(r, at=None):
    _positive("sphere", r)
    return _place(Manifold.sphere(r, _seg(r)), at)


def ellipsoid(rx, ry, rz, at=None):
    """3 軸の半径が違う球（頭・胴・果物など）。"""
    _positive("ellipsoid", rx, ry, rz)
    r = max(rx, ry, rz)
    return _place(Manifold.sphere(1.0, _seg(r)).scale([rx, ry, rz]), at)


def capsule(r, length, at=None, axis="y"):
    """両端が半球の棒。length は両端の球の中心の間隔（全長 = length + 2r）。"""
    _positive("capsule", r, length)
    seg = _seg(r)
    a = Manifold.sphere(r, seg).translate([0, -length / 2, 0])
    b = Manifold.sphere(r, seg).translate([0, length / 2, 0])
    return _place(_orient(Manifold.batch_hull([a, b]), axis), at)


def torus(R, r, at=None, axis="y"):
    """輪の半径 R・管の半径 r のドーナツ（axis = 輪の軸）。"""
    _positive("torus", R, r)
    if r >= R:
        raise ValueError("torus: tube radius r must be smaller than ring radius R")
    n = _seg(r)
    pts = [(R + r * math.cos(2 * math.pi * k / n), r * math.sin(2 * math.pi * k / n)) for k in range(n)]
    m = CrossSection([pts]).revolve(_seg(R)).rotate([-90, 0, 0])  # 軸 Z → Y
    return _place(_orient(m, axis), at)


def extrude(points, depth, at=None, axis="z"):
    """2D の多角形 points を厚さ depth で押し出した板（厚さの中心が at）。

    axis='z': points は (x, y)、axis='x': points は (z, y)、axis='y': points は (x, z)。
    多角形は反時計回りでも時計回りでもよい（自己交差はしないこと）。"""
    _positive("extrude", depth)
    pts = [(float(u), float(v)) for u, v in points]
    if len(pts) < 3:
        raise ValueError("extrude needs at least 3 points")
    cs = CrossSection([pts])
    if cs.area() < 1e-12:
        cs = CrossSection([pts[::-1]])
    if cs.is_empty():
        raise ValueError("extrude: the polygon is empty or self-intersecting")
    m = cs.extrude(depth).translate([0, 0, -depth / 2])
    a = _axis(axis)
    if a == "x":
        m = m.rotate([0, -90, 0])
    elif a == "y":
        m = m.rotate([90, 0, 0])
    return _place(m, at)


def revolve(profile, at=None, segments=None):
    """(半径, 高さ) の折れ線 profile を Y 軸まわりに回した回転体（壺・瓶・ランプの笠）。

    半径は 0 以上。軸上の点（半径 0）から始めて軸上で終えると中の詰まった形になる。"""
    pts = [(max(0.0, float(r)), float(y)) for r, y in profile]
    if len(pts) < 3:
        raise ValueError("revolve needs at least 3 profile points")
    rmax = max(p[0] for p in pts)
    cs = CrossSection([pts])
    if cs.area() < 1e-12:
        cs = CrossSection([pts[::-1]])
    m = cs.revolve(int(segments or _seg(rmax))).rotate([-90, 0, 0])  # 軸 Z → Y
    return _place(m, at)


def tube(points, r, closed=False):
    """3D の折れ線 points に沿った太さ r の管（ハンドル・配管・ワイヤー・ツル）。"""
    _positive("tube", r)
    P = [_vec3(p, "points") for p in points]
    if len(P) < 2:
        raise ValueError("tube needs at least 2 points")
    if closed:
        P = P + [P[0]]
    seg = max(12, _seg(r) // 2)
    balls = [Manifold.sphere(r, seg).translate(list(p)) for p in P]
    pieces = [Manifold.batch_hull([balls[i], balls[i + 1]]) for i in range(len(balls) - 1)]
    return Shape(Manifold.batch_boolean(pieces, _OP_ADD) if len(pieces) > 1 else pieces[0])


def hull(*shapes):
    """形の凸包（球をいくつか並べて包むと、なめらかな塊が作れる）。"""
    ms = [_man(s) for s in _flat(shapes)]
    if not ms:
        raise ValueError("hull needs at least one shape")
    return Shape(Manifold.batch_hull(ms))


def union(*shapes):
    ms = [_man(s) for s in _flat(shapes)]
    if not ms:
        raise ValueError("union needs at least one shape")
    return Shape(Manifold.batch_boolean(ms, _OP_ADD) if len(ms) > 1 else ms[0])


def _flat(shapes):
    out = []
    for s in shapes:
        if isinstance(s, (list, tuple)):
            out.extend(_flat(s))
        else:
            out.append(s)
    return out


try:  # manifold3d の版で列挙の置き場が違う
    from manifold3d import OpType as _OpType
    _OP_ADD = _OpType.Add
except Exception:  # pragma: no cover
    _OP_ADD = 0


# ---- 材質 ---------------------------------------------------------------------

# 名前 → (sRGB の色, metallic, roughness, 密度 kg/m³)。material("wood") のように名前で呼べる。
# 密度は部品ごとの質量（体積 × 密度）を出すのに使う（エンジンの <geom mass>）。
PRESETS = {
    "wood":          ("#9a6a3a", 0.0, 0.65, 600),
    "dark_wood":     ("#5a3a22", 0.0, 0.6, 700),
    "light_wood":    ("#c9a272", 0.0, 0.65, 500),
    "steel":         ("#8c8f93", 1.0, 0.35, 7850),
    "chrome":        ("#d8dadc", 1.0, 0.08, 7850),
    "aluminum":      ("#c4c7ca", 1.0, 0.3, 2700),
    "iron":          ("#3a3836", 0.8, 0.65, 7200),
    "gold":          ("#e6b84a", 1.0, 0.25, 19300),
    "brass":         ("#c9a44a", 1.0, 0.3, 8500),
    "copper":        ("#c27a4a", 1.0, 0.3, 8960),
    "plastic_white": ("#eeeeec", 0.0, 0.4, 1100),
    "plastic_black": ("#1c1c1e", 0.0, 0.45, 1100),
    "plastic_red":   ("#c8281e", 0.0, 0.4, 1100),
    "plastic_blue":  ("#2a5ab8", 0.0, 0.4, 1100),
    "plastic_green": ("#2e9a46", 0.0, 0.4, 1100),
    "plastic_yellow": ("#f0c020", 0.0, 0.4, 1100),
    "rubber":        ("#202020", 0.0, 0.85, 1200),
    "fabric":        ("#7a7f8a", 0.0, 0.95, 300),
    "leather":       ("#6b3b22", 0.0, 0.55, 900),
    "ceramic":       ("#f2f0ea", 0.0, 0.2, 2400),
    "glass":         ("#bfd9e6", 0.0, 0.05, 2500),   # 透過は無い（青みの艶）
    "stone":         ("#8a8780", 0.0, 0.9, 2600),
    "concrete":      ("#a3a19b", 0.0, 0.95, 2400),
    "brick":         ("#9c4a32", 0.0, 0.9, 1900),
    "paper":         ("#f4f1e8", 0.0, 0.9, 700),
    "skin":          ("#e0b090", 0.0, 0.6, 1000),
    "leaf":          ("#3f7d2c", 0.0, 0.7, 600),
    "bark":          ("#5b4433", 0.0, 0.95, 700),
    "soil":          ("#5a4330", 0.0, 1.0, 1500),
    "water":         ("#3a6f9a", 0.0, 0.05, 1000),
}


class Material:
    def __init__(self, name, color, metal, rough, emissive, density):
        self.name = name
        self.color = color      # sRGB 0..1
        self.metal = metal
        self.rough = rough
        self.emissive = emissive
        self.density = density  # kg/m³

    def key(self):
        return (self.name, tuple(round(c, 4) for c in self.color), self.metal, self.rough, self.emissive)


def _parse_color(c):
    if isinstance(c, str):
        s = c.strip().lstrip("#")
        if len(s) != 6:
            raise ValueError(f"color {c!r}: use '#rrggbb' or (r, g, b) in 0..1")
        return tuple(int(s[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    t = tuple(float(x) for x in c)
    if len(t) < 3:
        raise ValueError(f"color {c!r}: needs 3 numbers")
    if max(t[:3]) > 1.0:  # 0..255 で書かれていたら直す
        t = tuple(x / 255.0 for x in t)
    return tuple(min(1.0, max(0.0, x)) for x in t[:3])


def material(color="#cccccc", metal=None, rough=None, emissive=0.0, density=None):
    """材質。color はプリセット名（"wood" "steel" …）か "#rrggbb" か (r, g, b)。
    density は kg/m³（省略時はプリセットの値、色だけの材質は 1000）。"""
    name = "custom"
    m0, r0, d0 = 0.0, 0.6, 1000.0
    if isinstance(color, str) and color in PRESETS:
        name = color
        color, m0, r0, d0 = PRESETS[color]
    rgb = _parse_color(color)
    metal = m0 if metal is None else min(1.0, max(0.0, float(metal)))
    rough = r0 if rough is None else min(1.0, max(0.02, float(rough)))
    density = d0 if density is None else float(density)
    if not (density > 0):
        raise ValueError("material: density must be positive")
    return Material(name, rgb, metal, rough, max(0.0, float(emissive)), density)


def _srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _as_material(mat):
    if mat is None:
        return material()
    if isinstance(mat, Material):
        return mat
    return material(mat)  # 名前や色をそのまま渡してもよい


def _pbr(mat, i):
    col = [_srgb_to_linear(x) for x in mat.color]
    return trimesh.visual.material.PBRMaterial(
        name=f"{mat.name}_{i}", baseColorFactor=[col[0], col[1], col[2], 1.0],
        metallicFactor=mat.metal, roughnessFactor=mat.rough, doubleSided=False,
        emissiveFactor=[c * mat.emissive for c in col] if mat.emissive > 0 else None)


# ---- 部品（パーツ）と関節 --------------------------------------------------------
#
# 動く所のある物（扉・引き出し・蓋・車輪・羽根・腕）は、V6 エンジンの見本と同じく
# **動く単位ごとに別の部品**にする。部品はどれも「組み上がった姿勢のまま、同じ
# 座標で」作り、関節（joint）で親につなぐ。書き出しは部品ごとに 1 つの .glb と、
# それを剛体 + ジョイントとして並べたシーン文書（XML）。

JOINT_TYPES = ("hinge", "slide", "ball", "weld")
_NAME_RE = __import__("re").compile(r"^[A-Za-z][A-Za-z0-9_]{0,31}$")


class Part:
    """1 つの剛体になる部品。add(形, 材質) で形を足す（Model.add と同じ）。"""

    def __init__(self, model, name, fixed, mass, collision):
        self.model = model
        self.name = name
        self.fixed = bool(fixed)
        self.mass = mass
        self.collision = collision
        self.shapes = []  # (Shape, Material)

    def add(self, shape, mat=None):
        if not isinstance(shape, Shape):
            raise TypeError(f"{self.name}.add() needs a shape first, got {type(shape).__name__}")
        if shape.is_empty():
            raise ValueError(f"{self.name}.add(): the shape is empty (a difference removed everything?)")
        if self.model.count() >= Model.MAX_SHAPES:
            raise ValueError(f"too many shapes (limit {Model.MAX_SHAPES}); merge shapes with + first")
        self.shapes.append((shape, _as_material(mat)))
        return shape

    def by_material(self):
        out = {}
        for s, mat in self.shapes:
            out.setdefault(mat.key(), (mat, []))[1].append(s.m)
        return [(mat, Manifold.batch_boolean(ms, _OP_ADD) if len(ms) > 1 else ms[0])
                for mat, ms in out.values()]

    def solid(self):
        ms = [s.m for s, _ in self.shapes]
        return Manifold.batch_boolean(ms, _OP_ADD) if len(ms) > 1 else ms[0]

    def mass_kg(self):
        if self.mass is not None:
            return float(self.mass)
        # 材質ごとの和の体積 × 密度（材質の違う形どうしの重なりは二重に数える - 数 mm なので無視）
        return sum(man.volume() * mat.density for mat, man in self.by_material())


class Joint:
    def __init__(self, child, parent, type, anchor, axis, range, motor, target, stiffness, damping):
        self.child, self.parent, self.type = child, parent, type
        self.anchor, self.axis = anchor, axis
        self.range, self.motor, self.target = range, motor, target
        self.stiffness, self.damping = stiffness, damping


class Model:
    """build(m) の m。

    動かない物: m.add(形, 材質) だけ（1 つの .glb）。
    動く所のある物: p = m.part("名前", fixed=...) で部品を作って p.add(...)、
    m.joint("子", "親", "hinge", anchor=..., axis=...) でつなぐ（部品ごとの .glb + シーン XML）。"""

    MAX_SHAPES = 400
    MAX_PARTS = 24

    def __init__(self):
        self.parts = {}   # 名前 → Part（作った順）
        self.joints = []

    def count(self):
        return sum(len(p.shapes) for p in self.parts.values())

    # ---- 組み立て ----
    def part(self, name, fixed=False, mass=None, collision="hull"):
        """部品（1 つの剛体）。fixed = 動かない（床に固定）、mass = kg（省略 = 体積 × 密度）、
        collision = "hull"（凸包。既定）か "trimesh"（凹形のまま。fixed の部品だけ）。"""
        if not isinstance(name, str) or not _NAME_RE.match(name):
            raise ValueError(f"part name {name!r}: use letters, digits and _ (start with a letter)")
        if name in self.parts:
            raise ValueError(f"part {name!r} already exists")
        if name == "world":
            raise ValueError("'world' is reserved for the ground")
        if len(self.parts) >= self.MAX_PARTS:
            raise ValueError(f"too many parts (limit {self.MAX_PARTS})")
        if collision not in ("hull", "trimesh"):
            raise ValueError("collision must be 'hull' or 'trimesh'")
        if collision == "trimesh" and not fixed:
            raise ValueError(f"part {name!r}: collision='trimesh' needs fixed=True")
        if mass is not None:
            _positive("mass", mass)
        p = Part(self, name, fixed, mass, collision)
        self.parts[name] = p
        return p

    def add(self, shape, mat=None):
        """部品を作らずに足した形は "main" という部品に入る（動かない物はこれだけでよい）。"""
        p = self.parts.get("main") or self.part("main")
        return p.add(shape, mat)

    def joint(self, child, parent="world", type="hinge", anchor=None, axis=(0, 1, 0), range=None,
              motor=None, target=0.0, stiffness=0.0, damping=0.0):
        """child を parent（部品名か "world" = 地面）に関節でつなぐ。

        type: hinge（回転、axis まわり）/ slide（直動、axis 方向）/ ball（球関節）/ weld（固定）。
        anchor = 関節の位置（組み上がった姿勢のワールド座標）、axis = 向き。
        range = (lo, hi)（hinge は度、slide は m、今の姿勢が 0）。
        motor = "speed" | "position"（target は hinge: 度/s・度、slide: m/s・m）。range と同時には使えない。
        stiffness / damping = ばね（hinge は N·m/rad、slide は N/m。今の姿勢が自然長）。"""
        def name_of(x):
            return x.name if isinstance(x, Part) else str(x)
        child, parent = name_of(child), name_of(parent)
        if type not in JOINT_TYPES:
            raise ValueError(f"joint type {type!r}: use one of {', '.join(JOINT_TYPES)}")
        if anchor is None:
            raise ValueError(f"joint {child!r}: anchor=(x, y, z) is required")
        a = _vec3(anchor, "anchor")
        ax = _vec3(axis, "axis")
        n = math.sqrt(sum(c * c for c in ax))
        if n < 1e-9:
            raise ValueError(f"joint {child!r}: axis must not be zero")
        ax = tuple(c / n for c in ax)
        if range is not None:
            if type not in ("hinge", "slide"):
                raise ValueError(f"joint {child!r}: range only applies to hinge / slide")
            lo, hi = (float(v) for v in range)
            if lo > hi:
                lo, hi = hi, lo
            range = (lo, hi)
        if motor is not None:
            if motor not in ("speed", "position"):
                raise ValueError(f"joint {child!r}: motor must be 'speed' or 'position'")
            if type not in ("hinge", "slide"):
                raise ValueError(f"joint {child!r}: motor only applies to hinge / slide")
            if range is not None:
                raise ValueError(f"joint {child!r}: use either range or motor, not both")
        self.joints.append(Joint(child, parent, type, a, ax, range, motor, float(target),
                                 float(stiffness), float(damping)))

    def is_assembly(self):
        return len(self.parts) > 1 or bool(self.joints)

    # ---- 検査（LLM に返す材料。英語）----
    def check(self):
        """problems = 直させたい誤り（文）、pieces = 部品ごとの「分かれた塊」の数。"""
        problems = []
        if not self.parts or not any(p.shapes for p in self.parts.values()):
            return {"problems": ["build(m) did not add any shape"], "pieces": 0, "pieces_bounds": []}
        lo, hi = self.bounds()
        tol = max(0.01, 0.03 * float(max(hi - lo)))
        pieces_total, pieces_bounds = 0, []
        for p in self.parts.values():
            if not p.shapes:
                problems.append(f"part {p.name!r} has no shapes (call {p.name}.add(...))")
                continue
            comps = p.solid().decompose()
            pieces_total += len(comps)
            if len(comps) > 1:
                bb = [[round(x, 3) for x in c.bounding_box()]
                      for c in sorted(comps, key=lambda c: -c.volume())]
                pieces_bounds += [[p.name] + b for b in bb]
        # 関節
        seen_child = {}
        for j in self.joints:
            for nm in (j.child, j.parent):
                if nm != "world" and nm not in self.parts:
                    problems.append(f"joint {j.child!r}->{j.parent!r}: there is no part {nm!r}")
            if j.child == j.parent:
                problems.append(f"joint {j.child!r}: a part cannot be joined to itself")
            if j.child in seen_child:
                problems.append(f"part {j.child!r} has more than one joint as the child "
                                "(each moving part has exactly one joint to its parent)")
            seen_child[j.child] = j
            for nm in (j.child, j.parent):
                if nm in self.parts and self.parts[nm].shapes:
                    d = _box_distance(j.anchor, self.parts[nm].solid().bounding_box())
                    if d > tol:
                        problems.append(f"joint {j.child!r}->{j.parent!r}: anchor {_fmt(j.anchor)} is "
                                        f"{d:.3f} m away from part {nm!r}; put it where the parts meet")
            if j.child in self.parts and self.parts[j.child].fixed:
                problems.append(f"part {j.child!r} is fixed=True but has a joint; only the base is fixed")
        # 木構造（輪が無いこと）
        for start in seen_child:
            cur, hops = start, 0
            while cur in seen_child and hops <= len(self.parts):
                cur = seen_child[cur].parent
                hops += 1
            if hops > len(self.parts):
                problems.append(f"the joints form a loop around part {start!r}")
                break
        # 関節の無い部品が 2 つ以上 = ばらばらの物（大抵は付け忘れ）
        if self.is_assembly():
            loose = [n for n in self.parts if n not in seen_child]
            if len(loose) > 1:
                problems.append("parts " + ", ".join(repr(n) for n in loose) + " have no joint; only "
                                "the base part may be without a joint (join the others to it, or merge "
                                "parts that never move relative to each other into one part)")
        return {"problems": problems, "pieces": pieces_total - len([p for p in self.parts.values() if p.shapes]) + 1
                if pieces_total else 0, "pieces_bounds": pieces_bounds}

    def bounds(self):
        lo = np.array([np.inf] * 3)
        hi = -lo
        for p in self.parts.values():
            for s, _ in p.shapes:
                b = np.array(s.m.bounding_box())
                lo = np.minimum(lo, b[:3])
                hi = np.maximum(hi, b[3:])
        return lo, hi

    # ---- 書き出し ----
    def _placement(self, height, origin):
        """全体の縮尺 k と、座標から引く点 c（書き出しの座標 = (v - c) * k）。"""
        lo, hi = self.bounds()
        size = hi - lo
        k = 1.0
        if height:
            if size[1] <= 1e-9:
                raise ValueError("the model has no height")
            k = float(height) / float(size[1])
        c = (lo + hi) / 2
        if origin == "bottom":
            c[1] = lo[1]
        return k, c, size

    def to_scene(self, height=None, origin="center", crease_deg=35):
        """全部品を 1 つの trimesh.Scene に（1 つの .glb・確認用の絵）。"""
        if not self.parts:
            raise ValueError("the model has no parts (call m.add)")
        k, c, size = self._placement(height, origin)
        scene = trimesh.Scene()
        tris, i = 0, 0
        for p in self.parts.values():
            for mat, man in p.by_material():
                t = _to_trimesh(man, c, k, crease_deg)
                t.visual = trimesh.visual.TextureVisuals(material=_pbr(mat, i))
                scene.add_geometry(t, node_name=f"{p.name}_{mat.name}_{i}", geom_name=f"{p.name}_{mat.name}_{i}")
                tris += len(t.faces)
                i += 1
        return scene, {"scale": k, "size": [round(float(x * k), 4) for x in size], "triangles": tris,
                       "materials": len({mat.key() for p in self.parts.values() for _, mat in p.shapes}),
                       "parts": len(self.parts), "shapes": self.count()}

    def export(self, path, height=None, origin="center"):
        scene, info = self.to_scene(height, origin)
        data = trimesh.exchange.gltf.export_glb(scene, include_normals=True)
        with open(path, "wb") as f:
            f.write(data)
        info["bytes"] = len(data)
        return scene, info

    def export_assembly(self, folder, name, file_prefix, height=None, layer=7, crease_deg=35):
        """部品ごとの .glb と、剛体 + ジョイントのシーン文書（XML テキスト）。

        組み立て全体は「底を y = 0.005、X / Z の中心を 0」に置く。各 .glb の原点は
        **その部品の凸包の体積重心** - エンジンは凸包で当たる剛体の原点を体積重心へ
        寄せる（Scene の hullCenter）ので、ここで合わせておけば文書の pos がそのまま
        部品の置き場所になる。file_prefix は assets/ からの相対フォルダ（<mesh file>）。"""
        import os
        k, c, size = self._placement(height, "bottom")
        lift = 0.005  # 床へのめり込みを避ける
        os.makedirs(folder, exist_ok=True)
        bodies, meshes, info_parts, scene = [], [], [], trimesh.Scene()
        total_bytes, tris, i = 0, 0, 0
        for p in self.parts.values():
            ts = []
            for mat, man in p.by_material():
                t = _to_trimesh(man, c, k, crease_deg)
                t.apply_translation([0, lift, 0])
                t.visual = trimesh.visual.TextureVisuals(material=_pbr(mat, i))
                ts.append(t)
                i += 1
            allv = np.concatenate([t.vertices for t in ts])
            hull = trimesh.Trimesh(allv).convex_hull
            centroid = np.array(hull.center_mass if hull.is_volume else allv.mean(0))
            part_scene = trimesh.Scene()
            for t in ts:
                shown = t.copy()                           # 確認用の絵は組み上がった姿勢
                shown.metadata["part"] = p.name
                scene.add_geometry(shown)
                t.apply_translation(-centroid)
                part_scene.add_geometry(t)
                tris += len(t.faces)
            data = trimesh.exchange.gltf.export_glb(part_scene, include_normals=True)
            with open(os.path.join(folder, p.name + ".glb"), "wb") as f:
                f.write(data)
            total_bytes += len(data)
            ext = allv.max(0) - allv.min(0)
            mass = p.mass_kg() * (k ** 3 if p.mass is None else 1.0)
            body = f"{name}_{p.name}"
            meshes.append(f'    <mesh name="{body}" file="{file_prefix}/{p.name}.glb" scale="1"/>')
            geom = (f'<geom type="mesh" mesh="{body}" size="{_n(max(ext) / 2)}" mass="{_n(max(mass, 1e-3))}"'
                    + (' collision="trimesh"' if p.collision == "trimesh" else "")
                    + (f' layer="{layer}" nocollide="{layer}"' if layer is not None and len(self.parts) > 1 else "")
                    + "/>")
            bodies.append(f'    <body name="{body}" pos="{_fmt(centroid)}"'
                          + (' fixed="true"' if p.fixed else "") + ">\n"
                          f"      {geom}\n    </body>")
            info_parts.append({"name": p.name, "mass": round(mass, 3), "size": [round(float(x), 3) for x in ext],
                               "fixed": p.fixed, "bytes": len(data)})
        joints = []
        for j in self.joints:
            a = (np.array(j.anchor) - c) * k + np.array([0, lift, 0])
            attrs = (f'name="{name}_{j.child}" type="{j.type}" body1="{name}_{j.child}" '
                     f'body2="{"world" if j.parent == "world" else name + "_" + j.parent}" '
                     f'anchor="{_fmt(a)}" axis="{_fmt(j.axis)}"')
            unit = 1.0 if j.type == "hinge" else k  # 角度は縮尺に依らない、長さは縮む
            if j.range is not None:
                attrs += f' range="{_n(j.range[0] * unit)} {_n(j.range[1] * unit)}"'
            if j.motor is not None:
                attrs += f' motor="{j.motor}" target="{_n(j.target * unit)}"'
            if j.stiffness > 0 or j.damping > 0:
                attrs += f' stiffness="{_n(j.stiffness)}" damping="{_n(j.damping)}"'
            joints.append(f"    <joint {attrs}/>")
        # カメラ（全体が入る距離）。ライト・地面は書かない = エンジンの既定で開く
        ext = (self.bounds()[1] - self.bounds()[0]) * k
        target = (0.0, ext[1] / 2 + lift, 0.0)
        radius = max(1.0, 2.2 * float(max(ext)))
        xml = (f'<wizengine model="{name}" version="4">\n'
               f"  <!-- generated by samples/meshgen/meshgen.py: {len(self.parts)} parts, "
               f"{len(self.joints)} joints -->\n"
               "  <asset>\n" + "\n".join(meshes) + "\n  </asset>\n"
               "  <worldbody>\n"
               f'    <camera name="cam0" target="{_fmt(target)}" azimuth="35" elevation="22" radius="{_n(radius)}"/>\n'
               + "\n".join(bodies) + "\n  </worldbody>\n"
               + ("  <equality>\n" + "\n".join(joints) + "\n  </equality>\n" if joints else "")
               + "</wizengine>\n")
        info = {"scale": k, "size": [round(float(x), 4) for x in ext], "triangles": tris,
                "parts": len(self.parts), "joints": len(self.joints), "shapes": self.count(),
                "bytes": total_bytes, "part_info": info_parts,
                "materials": len({mat.key() for p in self.parts.values() for _, mat in p.shapes})}
        # 確認用の絵に関節の軸を描くための値（書き出し後の座標）
        info["joint_axes"] = [[list((np.array(j.anchor) - c) * k + np.array([0, lift, 0])), list(j.axis), j.type]
                              for j in self.joints]
        info["moved_scene"] = self._moved_scene(scene, c, k, lift)
        return scene, xml, info

    def _moved_scene(self, scene, c, k, lift, frac=0.7):
        """関節を動かした姿勢（range の 70%、範囲の無い hinge は 45°、slide は動かさない）。
        扉が外へ開くか・引き出しが前へ出るか（= range の符号）を絵で確かめるため。
        向きの約束は Chrono と同じ（hinge は axis まわりの右ねじが正）。"""
        joint_of = {j.child: j for j in self.joints}
        cache = {}

        def world_of(name, depth=0):
            if name not in joint_of or depth > len(self.parts):
                return np.eye(4)
            if name in cache:
                return cache[name]
            j = joint_of[name]
            a = (np.array(j.anchor) - c) * k + np.array([0, lift, 0])
            if j.type == "hinge":
                ang = (j.range[0] + (j.range[1] - j.range[0]) * frac) if j.range else 45.0
                if j.motor == "position":
                    ang = j.target
                M = trimesh.transformations.rotation_matrix(math.radians(ang), j.axis, a)
            elif j.type == "slide":
                d = (j.range[0] + (j.range[1] - j.range[0]) * frac) * k if j.range else 0.0
                if j.motor == "position":
                    d = j.target * k
                M = trimesh.transformations.translation_matrix(np.array(j.axis) * d)
            else:
                M = np.eye(4)
            parent = world_of(j.parent, depth + 1) if j.parent != "world" else np.eye(4)
            cache[name] = parent @ M
            return cache[name]

        out = trimesh.Scene()
        for node in scene.graph.nodes_geometry:
            _, gname = scene.graph[node]
            g = scene.geometry[gname].copy()
            part = g.metadata.get("part")
            if part is not None:
                g.apply_transform(world_of(part))
            out.add_geometry(g)
        return out


def _to_trimesh(man, c, k, crease_deg):
    mesh = man.to_mesh()
    v = (np.array(mesh.vert_properties)[:, :3] - c) * k
    t = trimesh.Trimesh(v, np.array(mesh.tri_verts), process=False)
    t.merge_vertices()
    return trimesh.graph.smooth_shade(t, angle=math.radians(crease_deg))


def _box_distance(p, bb):
    d = 0.0
    for i in range(3):
        if p[i] < bb[i]:
            d += (bb[i] - p[i]) ** 2
        elif p[i] > bb[i + 3]:
            d += (p[i] - bb[i + 3]) ** 2
    return math.sqrt(d)


def _n(v):
    s = f"{float(v):.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def _fmt(v):
    return " ".join(_n(x) for x in v)


# ---- 確認用の絵（matplotlib があれば）------------------------------------------


def preview_png(scene, path, max_faces=60000, joint_axes=None, moved=None):
    """斜め前と横からの 2 枚を 1 枚の PNG に。エンジンを起動せずに形を確かめる用。
    joint_axes = [(anchor, axis, type), ...] を渡すと関節の軸を赤い線で重ねる
    （向きと位置が合っているかを絵で確かめる）。"""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    if moved is not None:
        # 左 = 組み上がった姿勢（関節の軸つき）、右 = 関節を動かした姿勢
        panels = [(scene, 20, -55, "assembled (red = joint axes)", joint_axes),
                  (moved, 20, -55, "joints moved to 70% of range", None)]
    else:
        panels = [(scene, 20, -55, "front-left", joint_axes), (scene, 10, 0, "side (+X)", joint_axes)]
    data = [_preview_tris(sc, max_faces) for sc, *_ in panels]
    allp = np.concatenate([T.reshape(-1, 3) for T, _ in data])
    lo, hi = allp.min(0), allp.max(0)
    mid, half = (lo + hi) / 2, (hi - lo).max() / 2 * 1.05

    fig = plt.figure(figsize=(10, 5), dpi=110)
    for i, ((_, elev, azim, title, axes_), (T, C)) in enumerate(zip(panels, data)):
        ax = fig.add_subplot(1, 2, i + 1, projection="3d")
        # matplotlib は Z が上なので (x, y, z) → (x, -z, y) で描く（Y-up をそのまま上に）
        P = T[:, :, [0, 2, 1]] * np.array([1, -1, 1])
        ax.computed_zorder = False  # 関節の線を必ず手前に描く
        ax.add_collection3d(Poly3DCollection(P, facecolors=C, edgecolors="none", linewidths=0,
                                             antialiased=False, zorder=1))
        for anchor, axis, jtype in axes_ or []:
            a = np.array(anchor, float)
            d = np.array(axis, float) * half * 0.35
            seg = np.array([a - d, a + d])[:, [0, 2, 1]] * np.array([1, -1, 1])
            style = "-" if jtype in ("hinge", "slide") else ":"
            ax.plot(seg[:, 0], seg[:, 1], seg[:, 2], style, color="#e0201a", lw=2.2, zorder=10)
            ax.scatter([seg[:, 0].mean()], [seg[:, 1].mean()], [seg[:, 2].mean()], color="#e0201a",
                       s=14, zorder=11)
        ax.set_xlim(mid[0] - half, mid[0] + half)
        ax.set_ylim(-mid[2] - half, -mid[2] + half)
        ax.set_zlim(mid[1] - half, mid[1] + half)
        ax.set_box_aspect((1, 1, 1))
        ax.view_init(elev=elev, azim=azim)
        ax.set_title(title, fontsize=9)
        ax.set_xlabel("x")
        ax.set_ylabel("-z")
        ax.set_zlabel("y")
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def _preview_tris(scene, max_faces):
    """絵のための三角形と色（matplotlib 用）。"""
    tris, cols = [], []
    light = np.array([0.4, 0.8, 0.45])
    light /= np.linalg.norm(light)
    ext = max(float(np.max(scene.extents)), 1e-6)
    for g in scene.geometry.values():
        base = np.array(g.visual.material.baseColorFactor[:3], float)
        if base.max() > 1.0:
            base = base / 255.0
        base = np.array([(x ** (1 / 2.2)) for x in base])  # リニア → 見た目
        # matplotlib は三角形ごとの奥行きで塗り順を決めるので、大きな三角形が
        # 手前の物を塗りつぶす。絵のためだけに細かく割る（.glb は割らない）。
        v, f = g.vertices, g.faces
        try:
            v, f = trimesh.remesh.subdivide_to_size(v, f, max_edge=ext / 12, max_iter=6)
        except Exception:
            pass
        T0 = v[f]
        n = np.cross(T0[:, 1] - T0[:, 0], T0[:, 2] - T0[:, 0])
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
        shade = 0.35 + 0.65 * np.clip(n @ light, 0, 1)
        tris.append(T0)
        cols.append(np.clip(base[None, :] * shade[:, None], 0, 1))
    T = np.concatenate(tris)
    C = np.concatenate(cols)
    if len(T) > max_faces:
        idx = np.random.default_rng(0).choice(len(T), max_faces, replace=False)
        T, C = T[idx], C[idx]
    return T, C


# スクリプトから見える名前（meshgen.py の実行環境が使う一覧）。
API = {
    "box": box, "rounded_box": rounded_box, "cylinder": cylinder, "cone": cone, "sphere": sphere,
    "ellipsoid": ellipsoid, "capsule": capsule, "torus": torus, "extrude": extrude,
    "revolve": revolve, "tube": tube, "hull": hull, "union": union, "material": material,
    "pi": math.pi, "sin": math.sin, "cos": math.cos, "tan": math.tan, "atan2": math.atan2,
    "sqrt": math.sqrt, "radians": math.radians, "degrees": math.degrees, "hypot": math.hypot,
}
