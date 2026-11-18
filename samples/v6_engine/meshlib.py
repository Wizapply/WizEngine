# CSG（manifold3d）で部品を組み、PBR 材質付きの .glb に書き出す小さな道具。
# 座標はメートル、Y が上（glTF と WizEngine のボディ座標系に同じ）。
import math
import numpy as np
import trimesh
from manifold3d import Manifold, CrossSection

SEG = 48  # 円の既定の分割


def seg_for(r):
    # 半径に応じた分割（小さな穴は粗く）
    return int(max(16, min(96, 16 + r * 1200)))


def cyl(r, h, seg=None, center=False, r2=None):
    """Z 軸の円柱（z = 0..h、center なら -h/2..h/2）。"""
    return Manifold.cylinder(h, r, r if r2 is None else r2, seg or seg_for(r), center)


def cyl_between(a, b, r, seg=None, r2=None):
    """2 点 a → b を結ぶ円柱。"""
    a = np.array(a, float); b = np.array(b, float)
    d = b - a; L = float(np.linalg.norm(d))
    m = cyl(r, L, seg, False, r2)
    return m.transform(frame_z(d / L, a))


def frame_z(zdir, origin):
    """ローカル Z を zdir へ向け、原点を origin に置く 3x4 行列。"""
    z = np.array(zdir, float); z /= np.linalg.norm(z)
    t = np.array([1.0, 0, 0]) if abs(z[0]) < 0.9 else np.array([0, 1.0, 0])
    x = np.cross(t, z); x /= np.linalg.norm(x)
    y = np.cross(z, x)
    M = np.zeros((3, 4))
    M[:, 0] = x; M[:, 1] = y; M[:, 2] = z; M[:, 3] = origin
    return M


def box(sx, sy, sz, center=True):
    return Manifold.cube([sx, sy, sz], center)


def rot_z(deg):
    a = math.radians(deg); c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0]], float)


def poly(points):
    return CrossSection([list(map(list, points))])


def circle_pts(cx, cy, r, n=None, a0=0.0, a1=2 * math.pi):
    n = n or seg_for(r)
    return [(cx + r * math.cos(a0 + (a1 - a0) * k / n), cy + r * math.sin(a0 + (a1 - a0) * k / n))
            for k in range(n)]


def extrude_z(cs, z0, z1):
    return cs.extrude(z1 - z0).translate([0, 0, z0])


def revolve_y(profile_rz, seg=SEG):
    """(r, y) の折れ線を Y 軸まわりに回した回転体（閉じた輪郭で渡す）。"""
    cs = CrossSection([[(r, y) for r, y in profile_rz]])
    m = cs.revolve(seg)          # manifold は Y 軸まわり → 結果は Z が元の y
    # revolve は (x, y) の y を Z へ写す: X-Y 平面の回転体で軸 = Z
    return m.rotate([-90, 0, 0])  # 軸 Z → Y


def revolve_z(profile_rz, seg=SEG):
    cs = CrossSection([[(r, z) for r, z in profile_rz]])
    return cs.revolve(seg)


def union(ms):
    ms = [m for m in ms if m is not None]
    out = ms[0]
    for m in ms[1:]:
        out = out + m
    return out


def to_tri(m):
    mesh = m.to_mesh()
    v = np.array(mesh.vert_properties)[:, :3]
    f = np.array(mesh.tri_verts)
    return trimesh.Trimesh(v, f, process=False)


def tube(points, r, seg=16, closed=False):
    """折れ線に沿った管（巻きばね・配管など）。r は数か点ごとの列。三角形メッシュを直接返す。"""
    P = np.array(points, float)
    n = len(P)
    rs = list(r) if hasattr(r, "__len__") else [r] * n
    T = np.zeros_like(P)
    for i in range(n):
        a = P[max(i - 1, 0)] if not closed else P[(i - 1) % n]
        b = P[min(i + 1, n - 1)] if not closed else P[(i + 1) % n]
        T[i] = b - a
        T[i] /= np.linalg.norm(T[i])
    ref = np.array([0, 0, 1.0]) if abs(T[0][2]) < 0.9 else np.array([1.0, 0, 0])
    N = np.cross(T[0], ref); N /= np.linalg.norm(N)
    verts = []
    for i in range(n):
        N = N - T[i] * np.dot(N, T[i]); N /= np.linalg.norm(N)
        B = np.cross(T[i], N)
        for k in range(seg):
            a = 2 * math.pi * k / seg
            verts.append(P[i] + rs[i] * (math.cos(a) * N + math.sin(a) * B))
    faces = []
    rings = n if closed else n - 1
    for i in range(rings):
        j = (i + 1) % n
        for k in range(seg):
            k2 = (k + 1) % seg
            a, b, c, d = i * seg + k, i * seg + k2, j * seg + k2, j * seg + k
            faces += [[a, b, c], [a, c, d]]
    if not closed:  # 端のふた
        c0 = len(verts); verts.append(P[0]); c1 = len(verts); verts.append(P[-1])
        for k in range(seg):
            k2 = (k + 1) % seg
            faces.append([c0, k2, k])
            faces.append([c1, (n - 1) * seg + k, (n - 1) * seg + k2])
    return trimesh.Trimesh(np.array(verts), np.array(faces), process=False)


class Model:
    """材質ごとの三角形メッシュを集めて 1 つの .glb にする。"""

    def __init__(self):
        self.parts = []  # (trimesh, material)

    def add(self, m, mat):
        if isinstance(m, Manifold):
            if m.is_empty():
                return
            m = to_tri(m)
        self.parts.append((m, mat))

    def add_split(self, m, mat, section_mat, planes):
        """CSG の断面（planes = [(n, d)] で n·x = d の面）だけ別の材質にする。"""
        t = to_tri(m)
        c = t.triangles_center
        nrm = t.face_normals
        sec = np.zeros(len(t.faces), bool)
        for n, d in planes:
            n = np.array(n, float); n /= np.linalg.norm(n)
            on = np.abs(c @ n - d) < 2e-4
            par = (nrm @ n) > 0.99
            sec |= on & par
        if sec.any():
            self.parts.append((t.submesh([np.where(sec)[0]], append=True), section_mat))
        if (~sec).any():
            self.parts.append((t.submesh([np.where(~sec)[0]], append=True), mat))

    def export(self, path, crease_deg=35):
        scene = trimesh.Scene()
        by_mat = {}
        for t, mat in self.parts:
            by_mat.setdefault(mat, []).append(t)
        for k, (mat, ts) in enumerate(by_mat.items()):
            t = trimesh.util.concatenate(ts)
            t.merge_vertices()
            t = trimesh.graph.smooth_shade(t, angle=math.radians(crease_deg))
            col, metal, rough = MATS[mat]
            t.visual = trimesh.visual.TextureVisuals(material=trimesh.visual.material.PBRMaterial(
                name=mat, baseColorFactor=[col[0], col[1], col[2], 1.0],
                metallicFactor=metal, roughnessFactor=rough, doubleSided=False))
            scene.add_geometry(t, node_name=f"{mat}_{k}", geom_name=f"{mat}_{k}")
        data = trimesh.exchange.gltf.export_glb(scene, include_normals=True)
        open(path, "wb").write(data)
        nt = sum(len(t.faces) for t, _ in self.parts)
        return len(data), nt


# 材質: (リニアの色, metallic, roughness)
MATS = {
    "forged":   ((0.62, 0.64, 0.67), 1.0, 0.32),   # 鍛造鋼（クランク・コンロッド）
    "polished": ((0.80, 0.81, 0.83), 1.0, 0.12),   # 研磨面（ジャーナル・ピン）
    "steel":    ((0.55, 0.56, 0.58), 1.0, 0.30),
    "darksteel":((0.20, 0.21, 0.22), 1.0, 0.45),
    "alu_cast": ((0.62, 0.63, 0.64), 0.85, 0.55),  # アルミ鋳物（ブロック・ヘッド）
    "alu_mach": ((0.78, 0.79, 0.80), 1.0, 0.25),   # アルミ切削（ピストン）
    "section":  ((0.80, 0.08, 0.04), 0.0, 0.45),   # 断面（カットモデルの赤）
    "bearing":  ((0.72, 0.52, 0.28), 1.0, 0.30),   # 軸受メタル
    "iron":     ((0.16, 0.15, 0.14), 0.8, 0.70),   # 鋳鉄（タービンハウジング）
    "black":    ((0.025, 0.025, 0.027), 0.0, 0.55),  # 樹脂（サージタンク・コイル）
    "rubber":   ((0.02, 0.02, 0.02), 0.0, 0.8),
    "ceramic":  ((0.85, 0.85, 0.82), 0.0, 0.35),
    "carbon":   ((0.10, 0.085, 0.075), 0.0, 0.85),  # ピストン頂部のカーボン
    "valve_in": ((0.70, 0.62, 0.40), 1.0, 0.30),
    "valve_ex": ((0.45, 0.30, 0.25), 1.0, 0.45),
    "spring":   ((0.15, 0.35, 0.65), 0.6, 0.40),
    "chain":    ((0.30, 0.31, 0.33), 1.0, 0.35),
    "guide":    ((0.05, 0.05, 0.05), 0.0, 0.5),
    "paint_blue": ((0.06, 0.16, 0.40), 0.0, 0.45),
    "copper":   ((0.80, 0.42, 0.20), 1.0, 0.35),
    "brass":    ((0.85, 0.65, 0.25), 1.0, 0.30),
    "red":      ((0.60, 0.04, 0.03), 0.0, 0.4),
    "paint_gray": ((0.42, 0.44, 0.46), 0.0, 0.5),
    "paint_yellow": ((0.90, 0.62, 0.05), 0.0, 0.4),
}


def bezier(p0, p1, p2, p3, n=24):
    p0, p1, p2, p3 = (np.array(x, float) for x in (p0, p1, p2, p3))
    return [((1 - t) ** 3) * p0 + 3 * ((1 - t) ** 2) * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3
            for t in np.linspace(0, 1, n)]


def tri_box(center, xdir, ydir, size):
    """向きのある箱（三角形メッシュ）。xdir / ydir は直交する単位ベクトル。"""
    xd = np.array(xdir, float); yd = np.array(ydir, float); zd = np.cross(xd, yd)
    b = trimesh.creation.box(extents=size)
    T = np.eye(4); T[:3, 0] = xd; T[:3, 1] = yd; T[:3, 2] = zd; T[:3, 3] = center
    b.apply_transform(T)
    return b
