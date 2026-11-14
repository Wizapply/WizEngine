# V6（VR30DDTT 相当）の部品を実寸の glTF にする。各部品はボディの座標系で作る
# （プレハブの部品は pos 0・回転 0・倍率 1 で置く）。
import math, os, sys
import numpy as np
import trimesh
from manifold3d import Manifold, CrossSection, JoinType, set_min_circular_angle, set_min_circular_edge_length
set_min_circular_angle(6.0); set_min_circular_edge_length(0.0008)
from meshlib import *
from geom import *

# 出力先（既定 = リポジトリの assets/models/v6）。python3 meshes.py [出力先] [部品...]
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../assets/models/v6"))
os.makedirs(OUT, exist_ok=True)

R = R_CRANK
JOURNAL_Z = [-0.162, -0.054, 0.054, 0.162]
JOURNAL_R = 0.031
PIN_R = 0.026
PIN_HALF = 0.01175           # ピン 1 本の幅の半分（2 本並び = 分割ピン）
BLOCK_Z0, BLOCK_Z1 = -0.182, 0.182
CS_BAYS = [(-0.152, -0.064), (-0.044, 0.044), (0.064, 0.152)]


def bank_cyls(b):
    return [c for c in CYL if bank_of(c) == b]


def bank_xf(m, b):
    """バンク座標（x = 外向き l、y = シリンダ軸 s、z）で作った物をワールド（ブロック座標）へ。"""
    alpha = BANKS[b]
    if b == "L":
        m = m.mirror([1, 0, 0])
    return m.transform(rot_z(alpha))


def bank_pt(b, l, s, z=0.0):
    a = u(BANKS[b]); n = outward(BANKS[b])
    return (s * a[0] + l * n[0], s * a[1] + l * n[1], z)


# ------------------------------------------------------------------ 歯車の輪郭
def gear_outline(n, rp, add=0.003, ded=0.004, tip_frac=0.35, root_frac=0.35):
    pts = []
    rt, rr = rp + add, rp - ded
    for k in range(n):
        a0 = 2 * math.pi * k / n
        da = 2 * math.pi / n
        for f, r in ((0.0, rr), (root_frac, rr), (0.5 - tip_frac / 2, rt), (0.5 + tip_frac / 2, rt),
                     (1 - root_frac + 0.0001, rr)):
            a = a0 + da * f
            pts.append((r * math.cos(a), r * math.sin(a)))
    # 重複点を除く
    out = []
    for p in pts:
        if not out or abs(out[-1][0] - p[0]) + abs(out[-1][1] - p[1]) > 1e-7:
            out.append(p)
    return out


def sprocket(n, pitch, z0, z1, bore_r=0.0):
    rp = pitch / (2 * math.sin(math.pi / n))
    cs = poly(gear_outline(n, rp, add=pitch * 0.32, ded=pitch * 0.45, tip_frac=0.25, root_frac=0.38))
    m = extrude_z(cs, z0, z1)
    if bore_r > 0:
        m = m - cyl(bore_r, 1, center=True).translate([0, 0, (z0 + z1) / 2])
    return m, rp


CHAIN_PITCH = 0.008
N_CRANK, N_CAM = 18, 36   # 2:1


# ------------------------------------------------------------------ クランクシャフト
def web_profile(beta):
    """ピン側の腕 + 反対側のカウンターウェイト（2D、ピンが u(beta) の向き）。"""
    arm = CrossSection.hull(CrossSection([circle_pts(0, 0, 0.040)]) + CrossSection([circle_pts(0, R, 0.033)]))
    cw_pts = [(0, 0)]
    for k in range(25):
        a = math.radians(-90 - 62 + 124 * k / 24)
        cw_pts.append((0.082 * math.cos(a), 0.082 * math.sin(a)))
    cw = CrossSection.hull(CrossSection([cw_pts]) + CrossSection([circle_pts(0, 0, 0.042)]))
    cs = (arm + cw).offset(-0.003, JoinType.Round).offset(0.003, JoinType.Round)
    return cs.rotate(beta)


def crankshaft():
    md = Model()
    polished, forged = [], []
    for z in JOURNAL_Z:
        polished.append(cyl(JOURNAL_R, 0.026, center=True).translate([0, 0, z]))
    for c in CYL:
        ux, uy = u(c["beta"])
        p = cyl(PIN_R, 2 * PIN_HALF, center=True).translate([R * ux, R * uy, c["z"]])
        # 油穴
        p = p - cyl_between((R * ux - 0.03 * uy, R * uy + 0.03 * ux, c["z"]),
                            (R * ux + 0.03 * uy, R * uy - 0.03 * ux, c["z"]), 0.0028, 12)
        polished.append(p)
    for k, zk in enumerate([-0.108, 0.0, 0.108]):
        ca, cb = CYL[2 * k], CYL[2 * k + 1]
        zi = 2 * PIN_HALF + 0.0008
        # 外側の 2 枚（ジャーナル ⇄ ピン）
        forged.append(extrude_z(web_profile(ca["beta"]), zk - 0.0545 + 0.0125, zk - zi))
        forged.append(extrude_z(web_profile(cb["beta"]), zk + zi, zk + 0.0545 - 0.0125))
        # 分割ピンのあいだの薄いウェブ
        mid = CrossSection.hull(CrossSection([circle_pts(*[R * x for x in u(ca["beta"])], 0.031)]) +
                                CrossSection([circle_pts(*[R * x for x in u(cb["beta"])], 0.031)]))
        forged.append(extrude_z(mid, zk - 0.0012, zk + 0.0012))
    # 前: スナウト・クランクスプロケット（2 列 = バンクごと）・ダンパープーリー
    forged.append(cyl(0.017, 0.105).translate([0, 0, -0.27]))
    for b in ("R", "L"):
        zc = CHAIN_Z_BANK[b]
        sp, _ = sprocket(N_CRANK, CHAIN_PITCH, zc - 0.0045, zc + 0.0045, 0.0)
        forged.append(sp)
    pulley_z0, pulley_z1 = -0.252, -0.222
    pul = cyl(0.080, pulley_z1 - pulley_z0).translate([0, 0, pulley_z0])
    for k in range(6):  # ポリ V 溝
        z = pulley_z0 + 0.004 + k * 0.0037
        pul = pul - (cyl(0.090, 0.0019).translate([0, 0, z]) - cyl(0.0765, 0.0019).translate([0, 0, z]))
    pul = pul - (cyl(0.064, 0.012).translate([0, 0, pulley_z0 - 0.001]) - cyl(0.030, 0.014).translate([0, 0, pulley_z0 - 0.002]))
    md.add(pul, "darksteel")
    rubber = cyl(0.0655, 0.006).translate([0, 0, pulley_z1 - 0.007]) - cyl(0.058, 0.01).translate([0, 0, pulley_z1 - 0.008])
    md.add(rubber, "rubber")
    md.add(cyl(0.012, 0.01, 6).translate([0, 0, pulley_z0 - 0.009]), "steel")  # センターボルト（六角）
    # 後: フランジ・フライホイール（リングギヤ）・ダイナモへの軸と継手
    forged.append(cyl(0.040, 0.012).translate([0, 0, 0.175]))
    fw = cyl(0.145, 0.022).translate([0, 0, 0.192])
    fw = fw - (cyl(0.120, 0.008).translate([0, 0, 0.208]) - cyl(0.055, 0.01).translate([0, 0, 0.207]))
    for k in range(8):
        a = 2 * math.pi * k / 8
        fw = fw - cyl(0.009, 0.05, 16).translate([0.09 * math.cos(a), 0.09 * math.sin(a), 0.18])
    for k in range(6):
        a = 2 * math.pi * (k + 0.5) / 6
        fw = fw - cyl(0.0045, 0.05, 12).translate([0.047 * math.cos(a), 0.047 * math.sin(a), 0.18])
    md.add(fw, "darksteel")
    ring = extrude_z(poly(gear_outline(112, 0.152, 0.0028, 0.0028)), 0.199, 0.211) - cyl(0.143, 0.1, center=True).translate([0, 0, 0.2])
    md.add(ring, "steel")
    # タイミングの印（黄色）
    md.add(box(0.012, 0.024, 0.002).translate([0, 0.128, 0.2145]), "paint_yellow")
    md.add(box(0.006, 0.014, 0.002).translate([0, 0.071, pulley_z0 - 0.001]), "paint_yellow")
    # ダイナモへの継手（プロペラシャフト + フランジ）
    shaft = cyl(0.026, 0.19).translate([0, 0, 0.214])
    for z in (0.226, 0.39):
        fl = cyl(0.058, 0.012, center=True).translate([0, 0, z])
        shaft = shaft + fl
    md.add(shaft, "steel")
    bolts = []
    for z in (0.226, 0.39):
        for k in range(6):
            a = 2 * math.pi * k / 6
            bolts.append(cyl(0.0055, 0.018, 6, center=True).translate([0.044 * math.cos(a), 0.044 * math.sin(a), z]))
    md.add(union(bolts), "darksteel")
    md.add(union(polished), "polished")
    md.add(union(forged), "forged")
    return md


# ------------------------------------------------------------------ コンロッド（I 断面）
def conrod():
    md = Model()
    h = L_ROD / 2
    T = 0.0215
    big_o, small_o = 0.0385, 0.0165
    outline = CrossSection.hull(CrossSection([circle_pts(0, -h, big_o)]) + CrossSection([circle_pts(0, h, small_o)]))
    # 胴の両脇を削いだ形（くびれ）
    side = CrossSection([[(0.0125, -h + 0.03), (0.06, -h + 0.03), (0.06, h - 0.012), (0.0095, h - 0.012)]])
    body = outline - side - side.mirror([1, 0])
    body = body + CrossSection([circle_pts(0, -h, big_o)]) + CrossSection([circle_pts(0, h, small_o)])
    rod = extrude_z(body, -T / 2, T / 2)
    # I 断面の溝（両面）
    pocket = CrossSection([[(-0.0055, -h + 0.045), (0.0055, -h + 0.045), (0.0042, h - 0.026), (-0.0042, h - 0.026)]])
    pocket = pocket.offset(0.0012, JoinType.Round)
    rod = rod - extrude_z(pocket, T / 2 - 0.0055, T / 2 + 0.01) - extrude_z(pocket, -T / 2 - 0.01, -T / 2 + 0.0055)
    # 穴（大端 = ピン + メタル、小端 = ピストンピン + ブッシュ）
    rod = rod - cyl(PIN_R + 0.0017, 0.1, center=True).translate([0, -h, 0])
    rod = rod - cyl(0.0122, 0.1, center=True).translate([0, h, 0])
    # 大端キャップのボルト座
    for sx in (-1, 1):
        rod = rod + box(0.013, 0.012, T).translate([sx * 0.0335, -h - 0.002, 0])
    md.add(rod, "forged")
    # キャップの合わせ目（細い溝を黒で）
    md.add(box(0.083, 0.0008, T + 0.0004).translate([0, -h - 0.0005, 0]) - cyl(PIN_R + 0.0017, 0.1, center=True).translate([0, -h, 0]), "darksteel")
    shell = cyl(PIN_R + 0.0017, T - 0.002, center=True) - cyl(PIN_R + 0.0002, T, center=True)
    md.add(shell.translate([0, -h, 0]), "bearing")
    bush = cyl(0.0122, T - 0.001, center=True) - cyl(0.0112, T, center=True)
    md.add(bush.translate([0, h, 0]), "bearing")
    bolts = []
    for sx in (-1, 1):
        bolts.append(cyl_between((sx * 0.0335, -h - 0.032, 0), (sx * 0.0335, -h + 0.016, 0), 0.0042, 12))
        bolts.append(cyl_between((sx * 0.0335, -h - 0.032, 0), (sx * 0.0335, -h - 0.026, 0), 0.0068, 6))
    md.add(union(bolts), "darksteel")
    return md


# ------------------------------------------------------------------ ピストン
def piston(mirror):
    md = Model()
    CH = 0.044                       # ピン中心 → 頂面（従来の値。デッキとのすき間 4 mm）
    rp = 0.0429
    body = cyl(rp, 0.030).rotate([-90, 0, 0]).translate([0, CH - 0.030, 0])   # ランド部
    # スカート: スラスト面（±X）だけ残すスリッパー形
    sk = cyl(rp - 0.0002, 0.050).rotate([-90, 0, 0]).translate([0, CH - 0.030 - 0.048, 0])
    sk = sk ^ box(0.07, 0.2, 0.062)
    inner = cyl(rp - 0.005, 0.09).rotate([-90, 0, 0]).translate([0, CH - 0.030 - 0.09 + 0.0065, 0])
    body = (body + sk) - inner
    # ピンボス
    boss = cyl(0.0175, 0.062, center=True).translate([0, 0, 0]) ^ cyl(rp - 0.002, 0.1).rotate([-90, 0, 0]).translate([0, -0.05, 0])
    boss = boss + box(0.026, 0.03, 0.062).translate([0, 0.013, 0]) ^ cyl(rp - 0.002, 0.1).rotate([-90, 0, 0]).translate([0, -0.05, 0])
    boss = boss - box(0.0235, 0.1, 0.0235).translate([0, -0.03, 0])   # 小端の入る隙間
    body = body + boss
    body = body - cyl(0.0112, 0.1, center=True)
    # リング溝 3 本
    for k, (y, w) in enumerate(((CH - 0.0070, 0.0013), (CH - 0.0115, 0.0013), (CH - 0.0175, 0.0024))):
        g = cyl(rp + 0.001, w).rotate([-90, 0, 0]).translate([0, y - w / 2, 0]) - cyl(rp - 0.0035, w + 0.001).rotate([-90, 0, 0]).translate([0, y - w / 2 - 0.0005, 0])
        body = body - g
        ringm = cyl(rp - 0.0001, w * 0.9).rotate([-90, 0, 0]).translate([0, y - w * 0.45, 0]) - cyl(rp - 0.0033, w).rotate([-90, 0, 0]).translate([0, y - w / 2, 0])
        md.add(ringm, "darksteel")
    # 頂面: バルブリセス（傾いたバルブの傘に合わせた浅い皿）
    cr = []
    for kind in ("in", "ex"):
        sgn = valve_sign(kind)
        hr = (HEAD_IN if kind == "in" else HEAD_EX) / 2 + 0.0012
        th = math.radians(THETA_V)
        d = (sgn * math.sin(th), math.cos(th), 0.0)
        for zz in (-VZ, VZ):
            p0 = np.array([sgn * SEAT_L, CH + 0.0108 - (BORE_R - SEAT_L) * ROOF_TAN + 0.0005 - 0.009, zz])
            # 頂面から 1.2 mm 掘る位置まで下げる
            base = np.array([sgn * SEAT_L, CH - 0.0008, zz])
            cr.append(cyl_between(base, base + 0.03 * np.array(d), hr, 32))
    body = body - union(cr)
    if mirror:
        body = body.mirror([1, 0, 0])
    md.add_split(body, "alu_mach", "carbon", [((0, 1, 0), CH)])
    md.add(cyl(0.011, 0.058, center=True), "polished")   # ピストンピン
    return md


# ------------------------------------------------------------------ バルブ（2 本 1 組）
def helix(r, wire, y0, y1, turns, seg=10, per_turn=28, dead=0.6):
    pts = []
    n = int(turns * per_turn)
    for k in range(n + 1):
        t = k / n
        a = 2 * math.pi * turns * t
        # 両端は密巻き（座巻き）
        e = dead / turns
        if t < e:
            y = y0 + wire * 0.55 + (t / e) * wire
        elif t > 1 - e:
            y = y1 - wire * 0.55 - ((1 - t) / e) * wire
        else:
            y = y0 + wire * 1.55 + (y1 - y0 - 3.1 * wire) * (t - e) / (1 - 2 * e)
        pts.append((r * math.cos(a), y, r * math.sin(a)))
    return tube(pts, wire / 2, seg)


def valve_pair(kind):
    md = Model()
    hd = (HEAD_IN if kind == "in" else HEAD_EX) / 2
    prof = [(0, 0), (hd, 0), (hd, 0.0012), (hd - 0.0018, 0.0030)]
    # 傘からステムへの R
    for k in range(1, 13):
        a = math.pi / 2 * k / 12
        rr = 0.00275 + (hd - 0.0018 - 0.00275) * (1 - math.sin(a))
        y = 0.0030 + 0.010 * (1 - math.cos(a)) / 1.0
        prof.append((rr, y))
    prof += [(0.00275, 0.094), (0.0022, 0.0955), (0, 0.0955)]
    vm, sp, metal = [], [], []
    for zz in (-VZ, VZ):
        v = revolve_y(prof, 40).translate([0, 0, zz])
        vm.append(v)
        # バケット（逆さのカップ。上面 = VALVE_LEN）
        b = cyl(0.0155, 0.020).rotate([-90, 0, 0]).translate([0, VALVE_LEN - 0.020, zz]) - cyl(0.0140, 0.0185).rotate([-90, 0, 0]).translate([0, VALVE_LEN - 0.0215, zz])
        metal.append(b)
        # リテーナとコッタ
        ret = revolve_y([(0.0028, 0.0865), (0.0115, 0.0865), (0.0115, 0.0885), (0.006, 0.0905), (0.0028, 0.0905)], 32)
        metal.append(ret.translate([0, 0, zz]))
        # ばね（座面 0.050 → リテーナ 0.0865。一緒に動く）
        sp.append(helix(0.0098, 0.0034, 0.050, 0.0865, 5.5).apply_translation([0, 0, zz]))
        # ばね座
        metal.append(revolve_y([(0.0035, 0.0485), (0.0125, 0.0485), (0.0125, 0.050), (0.0035, 0.050)], 32).translate([0, 0, zz]))
    md.add(union(vm), "valve_in" if kind == "in" else "valve_ex")
    md.add(union(metal), "polished")
    for s in sp:
        md.add(s, "spring")
    return md


# ------------------------------------------------------------------ カムシャフト
def lobe_profile(kind, n=360):
    """平らなバケットが valvetrain のリフト式どおり押される形（半平面の交わり）。ノーズ = +Y。"""
    P = [(-0.1, -0.1), (0.1, -0.1), (0.1, 0.1), (-0.1, 0.1)]
    for k in range(n):
        ph = 2 * math.pi * k / n - math.pi
        d = (-math.sin(ph), math.cos(ph))
        h = RB + cam_lift(ph, kind)
        Q = []
        for i in range(len(P)):
            A, B = P[i], P[(i + 1) % len(P)]
            fa = A[0] * d[0] + A[1] * d[1] - h
            fb = B[0] * d[0] + B[1] * d[1] - h
            if fa <= 0: Q.append(A)
            if fa * fb < 0:
                t = fa / (fa - fb)
                Q.append((A[0] + t * (B[0] - A[0]), A[1] + t * (B[1] - A[1])))
        P = Q
    return P


def cam_bearings(b):
    zs = sorted(c["z"] for c in bank_cyls(b))
    mids = [(zs[i] + zs[i + 1]) / 2 for i in range(len(zs) - 1)]
    return [zs[0] - 0.050] + mids + [zs[-1] + 0.050]


def camshaft(b, kind):
    md = Model()
    zb = BANK_Z[b]
    alpha = BANKS[b]
    vk = "in" if kind == "i" else "ex"
    center = IN_CENTER if kind == "i" else EX_CENTER
    zf = CHAIN_Z_BANK[b] - zb
    brg = [z - zb for z in cam_bearings(b)]
    shaft = cyl(0.0115, brg[-1] + 0.012 - (zf - 0.006)).translate([0, 0, zf - 0.006])
    parts = [shaft]
    for z in brg:
        parts.append(cyl(0.0140, 0.020, center=True).translate([0, 0, z]))
    # 六角（工具を掛ける所）
    parts.append(cyl(0.0145, 0.012, 6, center=True).translate([0, 0, brg[0] + 0.022]))
    prof = poly(lobe_profile(vk))
    dvd = dir_deg(valve_dir(alpha, vk))
    for c in bank_cyls(b):
        cc = wrap360((c["fire"] + center) / 2.0)
        lam = wrap360(dvd + 180.0 - cc)
        for zz in (-VZ, VZ):
            z = c["z"] - zb + zz
            parts.append(extrude_z(prof.rotate(lam), z - 0.0075, z + 0.0075))
    md.add(union(parts), "forged")
    sp, rp = sprocket(N_CAM, CHAIN_PITCH, zf - 0.0045, zf + 0.0045, 0.0)
    sp = sp - union([cyl(0.0085, 0.02, 24, center=True).translate([0.028 * math.cos(a), 0.028 * math.sin(a), zf])
                     for a in [2 * math.pi * k / 5 for k in range(5)]])
    md.add(sp, "steel")
    if kind == "i":   # 電動 VTC のアクチュエータ（スプロケットの前）
        vtc = cyl(0.040, 0.026).translate([0, 0, zf - 0.0045 - 0.026])
        vtc = vtc - cyl(0.030, 0.004).translate([0, 0, zf - 0.0045 - 0.0275])
        md.add(vtc, "alu_mach")
        bl = [cyl(0.003, 0.006, 6).translate([0.034 * math.cos(a), 0.034 * math.sin(a), zf - 0.0045 - 0.029])
              for a in [2 * math.pi * k / 6 for k in range(6)]]
        md.add(union(bl), "darksteel")
        md.add(cyl(0.008, 0.004, 6).translate([0, 0, zf - 0.0045 - 0.030]), "darksteel")
    else:
        md.add(cyl(0.010, 0.006, 6).translate([0, 0, zf - 0.0045 - 0.006]), "darksteel")
    return md


if __name__ == "__main__":
    which = sys.argv[2:] or ["all"]
    def want(n): return "all" in which or n in which
    jobs = []
    if want("crank"): jobs.append(("crankshaft", crankshaft))
    if want("rod"): jobs.append(("conrod", conrod))
    if want("piston"):
        jobs.append(("piston_R", lambda: piston(False))); jobs.append(("piston_L", lambda: piston(True)))
    if want("valve"):
        jobs.append(("valve_in", lambda: valve_pair("in"))); jobs.append(("valve_ex", lambda: valve_pair("ex")))
    if want("cam"):
        for b in ("R", "L"):
            for k in ("i", "e"):
                jobs.append((f"cam{b}{k}", (lambda b=b, k=k: camshaft(b, k))))
    for name, fn in jobs:
        size, nt = fn().export(os.path.join(OUT, name + ".glb"))
        print(f"{name}: {size/1024:.0f} KB, {nt} tris")
