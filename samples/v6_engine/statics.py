# 動かない部品（エンジンブロックのプレハブ = ブロック座標、原点 = クランク中心）。
# 右バンク（1・3・5 番）の外側半分をカットモデルにして中を見せる（断面は赤）。
import math, os, sys
import numpy as np
import trimesh
from manifold3d import Manifold, CrossSection
from meshes import *

Z0, Z1 = BLOCK_Z0, BLOCK_Z1
CUT_Y = -0.06                       # 右側の切り取りの下端（その下のクランクケースは残す）
NR = outward(BANKS["R"])
TH = math.radians(THETA_V)
CAM_S = SEAT_S + CAM_DIST * math.cos(TH)            # バンク座標のカム軸の高さ
CAM_L = SEAT_L + CAM_DIST * math.sin(TH)            # カム軸の横（吸気 = -、排気 = +）
HEAD_W = 0.088
HEAD_TOP = CAM_S                                     # ヘッド本体の上面 = カム軸の高さ
RAIL_TOP = CAM_S + 0.034


def cut_region():
    r = box(1.0, 2.0, 2.0).translate([0.5, 0, 0]).transform(rot_z(BANKS["R"]))
    return r.trim_by_plane([0, 1, 0], CUT_Y)


CUT_PLANES = [((NR[0], NR[1], 0.0), 0.0), ((0, 1, 0), CUT_Y)]


def bank_box(l0, l1, s0, s1, z0=Z0, z1=Z1):
    return box(l1 - l0, s1 - s0, z1 - z0).translate([(l0 + l1) / 2, (s0 + s1) / 2, (z0 + z1) / 2])


def along_s(r, s0, s1, l=0.0, z=0.0, seg=None):
    return cyl(r, s1 - s0, seg).rotate([-90, 0, 0]).translate([l, s0, z])


def valve_local(kind, h, zz):
    """バンク座標でのバルブ軸上の点（シートから h）。"""
    sg = valve_sign(kind)
    return np.array([sg * SEAT_L + sg * h * math.sin(TH), SEAT_S + h * math.cos(TH), zz])


def valve_dir_local(kind):
    sg = valve_sign(kind)
    return np.array([sg * math.sin(TH), math.cos(TH), 0.0])


# ------------------------------------------------------------------ ブロック
def engine_block():
    md = Model()
    pts = circle_pts(0, 0, 0.112, 64) + [(-0.128, -0.095), (0.128, -0.095), (0.128, 0.0), (-0.128, 0.0)]
    for b in ("R", "L"):
        for l in (-0.064, 0.064):
            p = bank_pt(b, l, 0.085)
            pts.append((p[0], p[1]))
    case = extrude_z(CrossSection([pts]).hull(), Z0, Z1)
    solid = case
    for b in ("R", "L"):
        bank = bank_box(-0.064, 0.064, 0.06, DECK)
        # デッキのふち（少し張り出す）
        bank = bank + bank_box(-0.066, 0.066, DECK - 0.010, DECK)
        solid = solid + bank_xf(bank, b)
    # 中をくり抜く: ボア・ウォータージャケット・コンロッドの逃げ
    for b in ("R", "L"):
        cut = []
        for c in bank_cyls(b):
            z = c["z"]
            cut.append(along_s(BORE_R + 0.0002, 0.04, DECK + 0.01, 0, z, 96))
            jac = along_s(0.0545, 0.115, DECK + 0.001, 0, z, 64) - along_s(0.0485, 0.10, DECK + 0.01, 0, z, 64)
            cut.append(jac)
            cut.append(bank_box(-0.058, 0.058, -0.02, 0.10, z - 0.040, z + 0.040))
        solid = solid - bank_xf(union(cut), b)
    bays = []
    for z0, z1 in CS_BAYS:
        bays.append(cyl(0.095, z1 - z0).translate([0, 0, z0]))
        bays.append(box(0.19, 0.12, z1 - z0).translate([0, -0.06 - 0.04, (z0 + z1) / 2]))
    solid = solid - union(bays)
    # 主軸受の穴・前後のオイルシール
    solid = solid - cyl(JOURNAL_R + 0.0018, Z1 - Z0 + 0.02).translate([0, 0, Z0 - 0.01])
    solid = solid - cyl(0.045, 0.012).translate([0, 0, Z1 - 0.008])
    # 右側をカット
    solid = solid - cut_region()
    md.add_split(solid, "alu_cast", "section", CUT_PLANES)
    # 主軸受メタル
    sh = [cyl(JOURNAL_R + 0.0018, 0.020, center=True).translate([0, 0, z]) - cyl(JOURNAL_R + 0.0003, 0.03, center=True).translate([0, 0, z])
          for z in JOURNAL_Z]
    md.add(union(sh), "bearing")
    # オイルパン（上 = アルミ、下 = 鋼板）
    up = box(0.256, 0.052, Z1 - Z0).translate([0, -0.095 - 0.026, 0])
    up = up - box(0.236, 0.06, Z1 - Z0 - 0.02).translate([0, -0.095 - 0.020, 0])
    md.add(up, "alu_cast")
    fins = [box(0.004, 0.010, Z1 - Z0 - 0.03).translate([x, -0.150, 0]) for x in np.linspace(-0.11, 0.11, 9)]
    md.add(union(fins), "alu_cast")
    lp = CrossSection([[(-0.118, 0.0), (0.118, 0.0), (0.112, -0.062), (-0.112, -0.062)]]).offset(0.004)
    lower = extrude_z(lp, Z0 + 0.015, Z1 - 0.075).translate([0, -0.147, 0])
    md.add(lower, "darksteel")
    md.add(cyl(0.008, 0.008, 6).rotate([-90, 0, 0]).translate([0.05, -0.218, 0.0]), "polished")
    # カバーの合わせ面（上のパンとブロックのあいだの薄い帯）
    md.add(box(0.260, 0.004, Z1 - Z0 + 0.004).translate([0, -0.097, 0]), "darksteel")
    return md


# ------------------------------------------------------------------ シリンダヘッド
def cylinder_head(b):
    md = Model()
    zb = BANK_Z[b]
    h = bank_box(-HEAD_W, HEAD_W, DECK, HEAD_TOP)
    h = h + bank_box(-HEAD_W, -HEAD_W + 0.010, HEAD_TOP - 0.01, RAIL_TOP) + bank_box(HEAD_W - 0.010, HEAD_W, HEAD_TOP - 0.01, RAIL_TOP)
    h = h + bank_box(-HEAD_W, HEAD_W, HEAD_TOP - 0.01, RAIL_TOP, Z1 - 0.010, Z1)
    cut = []
    brg = cam_bearings(b)
    for kind in ("i", "e"):
        l = valve_sign(kind) * CAM_L
        tr = cyl(0.0305, Z1 - Z0 + 0.04).translate([l, CAM_S, Z0 - 0.02])
        sad = union([box(0.08, 0.08, 0.020).translate([l, CAM_S, z]) for z in brg])
        cut.append(tr - sad)
        cut.append(cyl(0.0142, Z1 - Z0 + 0.04).translate([l, CAM_S, Z0 - 0.02]))
        # カムの上は開ける（カバー無し）
        cut.append(bank_box(l - 0.0305, l + 0.0305, CAM_S, RAIL_TOP + 0.01, Z0 - 0.02, Z1 - 0.010))
    for c in bank_cyls(b):
        z = c["z"]
        # 燃焼室（ペントルーフ）
        ch = along_s(BORE_R, DECK - 0.001, DECK + 0.02, 0, z, 96)
        ch = ch.trim_by_plane([-ROOF_TAN, -1, 0], -(DECK + BORE_R * ROOF_TAN)).trim_by_plane([ROOF_TAN, -1, 0], -(DECK + BORE_R * ROOF_TAN))
        cut.append(ch)
        for kind in ("in", "ex"):
            hd = (HEAD_IN if kind == "in" else HEAD_EX) / 2
            d = valve_dir_local(kind)
            for zz in (-VZ, VZ):
                p0 = valve_local(kind, 0.0, z + zz)
                cut.append(cyl_between(p0 - 0.004 * d, p0 + 0.010 * d, hd - 0.0015, 48))      # のど
                cut.append(cyl_between(p0, p0 + 0.060 * d, 0.0034, 16))                       # ガイド
                cut.append(cyl_between(p0 + 0.047 * d, p0 + 0.14 * d, 0.0158, 48))           # バケット穴
                if kind == "in":
                    ex = np.array([-HEAD_W - 0.01, 0.292, z + zz * 0.45])
                    cut.append(cyl_between(p0 + 0.006 * d, ex, 0.0115, 32))
                else:
                    ex = np.array([HEAD_W + 0.01, 0.270, z + zz * 0.45])
                    cut.append(cyl_between(p0 + 0.006 * d, ex, 0.0100, 32))
        cut.append(along_s(0.0072, DECK, DECK + 0.05, 0, z, 24))       # プラグ穴
        cut.append(along_s(0.0118, DECK + 0.045, RAIL_TOP + 0.01, 0, z, 32))
    h = h - union(cut)
    hw = bank_xf(h, b)
    if b == "R":
        hw = hw - cut_region()
        md.add_split(hw, "alu_cast", "section", CUT_PLANES)
    else:
        md.add(hw, "alu_cast")
    # カムキャップ（ラダー）とボルト
    caps, bolts = [], []
    for kind in ("i", "e"):
        l = valve_sign(kind) * CAM_L
        for z in brg:
            cp = box(0.042, 0.016, 0.019).translate([l, CAM_S + 0.008, z]) - cyl(0.0142, 0.03, center=True).translate([l, CAM_S, z])
            caps.append(cp)
            for dl in (-0.0155, 0.0155):
                bolts.append(along_s(0.0045, CAM_S + 0.016, CAM_S + 0.021, l + dl, z, 6))
    capm = bank_xf(union(caps), b)
    boltm = bank_xf(union(bolts), b)
    if b == "R":
        capm = capm - cut_region(); boltm = boltm - cut_region()
        md.add_split(capm, "alu_mach", "section", CUT_PLANES)
    else:
        md.add(capm, "alu_mach")
    md.add(boltm, "darksteel")
    # 点火プラグとイグニッションコイル
    plugs, cer, coils = [], [], []
    for c in bank_cyls(b):
        z = c["z"]
        plugs.append(along_s(0.0065, DECK + 0.002, DECK + 0.02, 0, z, 24))
        plugs.append(along_s(0.0082, DECK + 0.02, DECK + 0.030, 0, z, 6))
        cer.append(along_s(0.0052, DECK + 0.030, DECK + 0.062, 0, z, 24))
        coils.append(along_s(0.0105, DECK + 0.062, RAIL_TOP + 0.012, 0, z, 32))
        coils.append(bank_box(-0.016, 0.016, RAIL_TOP + 0.012, RAIL_TOP + 0.034, z - 0.012, z + 0.012))
        coils.append(bank_box(0.016, 0.030, RAIL_TOP + 0.018, RAIL_TOP + 0.030, z - 0.007, z + 0.007))
    md.add(bank_xf(union(plugs), b), "polished")
    md.add(bank_xf(union(cer), b), "ceramic")
    md.add(bank_xf(union(coils), b), "black")
    # ヘッドガスケット（ブロックとのあいだの黒い帯）
    g = bank_box(-0.066, 0.066, DECK - 0.0006, DECK + 0.0006, Z0 - 0.0005, Z1 + 0.0005)
    for c in bank_cyls(b):
        g = g - along_s(BORE_R + 0.001, DECK - 0.01, DECK + 0.01, 0, c["z"], 64)
    gw = bank_xf(g, b)
    if b == "R":
        gw = gw - cut_region()
    md.add(gw, "darksteel")
    return md


# ------------------------------------------------------------------ 吸気（サージタンク・ランナー・スロットル）
def rounded_rect(cx, cy, w, h, r):
    return CrossSection([[(cx - w / 2 + r, cy - h / 2), (cx + w / 2 - r, cy - h / 2), (cx + w / 2 - r, cy + h / 2), (cx - w / 2 + r, cy + h / 2)]]).offset(r) + \
        CrossSection([[(cx - w / 2, cy - h / 2 + r), (cx + w / 2, cy - h / 2 + r), (cx + w / 2, cy + h / 2 - r), (cx - w / 2, cy + h / 2 - r)]])


TANK_Y = 0.405
TANK_Z0, TANK_Z1 = -0.150, 0.165


def intake():
    md = Model()
    tank = extrude_z(rounded_rect(0, TANK_Y, 0.13, 0.056, 0.022), TANK_Z0, TANK_Z1)
    for z in np.linspace(TANK_Z0 + 0.05, TANK_Z1 - 0.05, 4):
        tank = tank + extrude_z(rounded_rect(0, TANK_Y, 0.134, 0.060, 0.024), z - 0.002, z + 0.002)
    tank = tank + extrude_z(rounded_rect(0, TANK_Y + 0.028, 0.04, 0.008, 0.003), TANK_Z0 + 0.02, TANK_Z1 - 0.02)
    md.add(tank, "black")
    tubes = []
    flanges = []
    for c in CYL:
        b = bank_of(c)
        z = c["z"]
        P0 = np.array(bank_pt(b, -HEAD_W, 0.292, z))
        nin = -np.array([outward(BANKS[b])[0], outward(BANKS[b])[1], 0.0])
        sx = 1 if b == "R" else -1
        P3 = np.array([sx * 0.035, TANK_Y - 0.022, z])
        pts = bezier(P0 - 0.004 * nin, P0 + 0.05 * nin, P3 - np.array([0, 0.04, 0]), P3, 20)
        tubes.append(tube(pts, 0.0165, 28))
        fl = bank_box(-HEAD_W - 0.008, -HEAD_W, 0.272, 0.312, z - 0.024, z + 0.024)
        flanges.append(bank_xf(fl, b))
    for t in tubes:
        md.add(t, "black")
    md.add(union(flanges), "alu_cast")
    # スロットルボディ（前）
    tb = cyl(0.036, 0.050).translate([0, TANK_Y, TANK_Z0 - 0.050]) + box(0.04, 0.046, 0.03).translate([0.040, TANK_Y, TANK_Z0 - 0.025])
    tb = tb - cyl(0.029, 0.06).translate([0, TANK_Y, TANK_Z0 - 0.055])
    md.add(tb, "alu_cast")
    md.add(cyl(0.028, 0.002, center=True).rotate([60, 0, 0]).translate([0, TANK_Y, TANK_Z0 - 0.030]), "brass")   # バタフライ
    md.add(cyl(0.018, 0.045).rotate([0, 90, 0]).translate([0.058, TANK_Y, TANK_Z0 - 0.025]), "black")      # 電子スロットルのモータ
    return md


# ------------------------------------------------------------------ 排気（左バンク）とターボ
def volute(axis_z, center, r0, tr0, tr1, mat_body, outlet_len, outlet_dir_sign, body_w):
    """渦巻きのハウジング（軸 Z）。r0 = 渦の中心半径、tr0→tr1 = 管の太さ。"""
    cx, cy = center
    pts, rs = [], []
    n = 40
    for k in range(n + 1):
        a = 2 * math.pi * 0.85 * k / n
        rr = r0 + (tr1 - tr0) * k / n * 0.8
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a), axis_z))
        rs.append(tr0 + (tr1 - tr0) * k / n)
    # 接線方向の出口
    a = 2 * math.pi * 0.85
    t = np.array([-math.sin(a), math.cos(a), 0.0])
    last = np.array(pts[-1])
    for k in range(1, 6):
        pts.append(tuple(last + t * outlet_len * k / 5)); rs.append(tr1)
    spiral = tube(pts, rs, 28)
    disc = cyl(r0 + 0.004, body_w, center=True).translate([cx, cy, axis_z])
    return spiral, disc, last + t * outlet_len, t


TURBO = np.array([-0.300, 0.040, 0.0])


def exhaust_turbo():
    md = Model()
    b = "L"
    pipes = []
    col_x, col_y = -0.272, 0.138
    for c in bank_cyls(b):
        z = c["z"]
        P0 = np.array(bank_pt(b, HEAD_W, 0.270, z))
        n = np.array([outward(BANKS[b])[0], outward(BANKS[b])[1], 0.0])
        P3 = np.array([col_x, col_y, z])
        pipes.append(tube(bezier(P0 - 0.004 * n, P0 + 0.04 * n, P3 + np.array([0, 0.05, 0]), P3, 18), 0.0155, 24))
        fl = bank_box(HEAD_W, HEAD_W + 0.008, 0.252, 0.288, z - 0.022, z + 0.022)
        md.add(bank_xf(fl, b), "iron")
    zs = [c["z"] for c in bank_cyls(b)]
    pipes.append(tube([(col_x, col_y, zs[0] - 0.02), (col_x, col_y, zs[-1] + 0.02)], 0.020, 28))
    # 集合管 → タービン入口
    tin = TURBO + np.array([0.040, 0.075, 0.040])
    pipes.append(tube(bezier((col_x, col_y - 0.01, 0.040), (col_x, col_y - 0.05, 0.040), tin + np.array([0, 0.04, 0]), tin, 12), 0.019, 28))
    for p in pipes:
        md.add(p, "iron")
    # タービン（後ろ）
    sp, disc, outp, t = volute(TURBO[2] + 0.040, (TURBO[0], TURBO[1]), 0.046, 0.012, 0.024, "iron", 0.02, 1, 0.040)
    md.add(sp, "iron"); md.add(disc, "iron")
    md.add(cyl(0.030, 0.050).translate([TURBO[0], TURBO[1], TURBO[2] + 0.060]), "iron")              # 出口
    md.add(tube(bezier(TURBO + [0, 0, 0.11], TURBO + [0, 0, 0.20], TURBO + [0, -0.12, 0.26], TURBO + [0, -0.30, 0.30], 16), 0.030, 28), "darksteel")
    # センターハウジング
    md.add(cyl(0.032, 0.040, center=True).translate([TURBO[0], TURBO[1], TURBO[2]]), "steel")
    md.add(tube(bezier(TURBO + [0, 0.032, 0], TURBO + [0, 0.08, 0], TURBO + [0.06, 0.12, -0.02], TURBO + [0.09, 0.15, -0.06], 14), 0.004, 12), "copper")  # オイルライン
    # コンプレッサー（前）
    sp, disc, outp, t = volute(TURBO[2] - 0.048, (TURBO[0], TURBO[1]), 0.052, 0.013, 0.026, "alu_cast", 0.03, -1, 0.034)
    md.add(sp, "alu_cast"); md.add(disc, "alu_cast")
    inlet = cyl(0.034, 0.05).translate([TURBO[0], TURBO[1], TURBO[2] - 0.115]) - cyl(0.028, 0.06).translate([TURBO[0], TURBO[1], TURBO[2] - 0.12])
    md.add(inlet, "alu_cast")
    # 吸気側: エアクリーナー（コーン）
    md.add(tube(bezier(TURBO + [0, 0, -0.115], TURBO + [0, 0, -0.17], TURBO + [0.0, -0.05, -0.22], TURBO + [0.02, -0.08, -0.27], 12), 0.034, 32), "rubber")
    fil = cyl(0.050, 0.10, r2=0.036).translate([0, 0, 0])
    md.add(fil.transform(frame_z((0.2, -0.3, -1.0), TURBO + np.array([0.02, -0.08, -0.27]))), "red")
    # 圧縮空気 → スロットル（インタークーラーは省略）
    P0 = outp
    P3 = np.array([0, TANK_Y, TANK_Z0 - 0.050])
    md.add(tube(bezier(P0, P0 + 0.12 * np.array(t), P3 + np.array([0, 0.0, -0.20]), P3, 30), 0.027, 32), "alu_mach")
    md.add(tube([P3 + np.array([0, 0, -0.012]), P3 + np.array([0, 0, 0.004])], 0.038, 32), "rubber")
    return md


# ------------------------------------------------------------------ タイミングチェーン
def chain_path(b):
    """クランク・排気カム・吸気カムのスプロケット（ピッチ円）に外から掛けたチェーンの道（2D）。"""
    rc = CHAIN_PITCH / (2 * math.sin(math.pi / N_CRANK))
    rm = CHAIN_PITCH / (2 * math.sin(math.pi / N_CAM))
    al = BANKS[b]
    circles = [((0.0, 0.0), rc), (cam_center(al, "e"), rm), (cam_center(al, "i"), rm)]
    cx = sum(c[0][0] for c in circles) / 3; cy = sum(c[0][1] for c in circles) / 3
    circles.sort(key=lambda c: math.atan2(c[0][1] - cy, c[0][0] - cx))   # 反時計回り
    # 隣どうしの外側の共通接線（反時計回りのループ = 右手側が外）
    tang = []
    for i in range(3):
        (p, r1), (q, r2) = circles[i], circles[(i + 1) % 3]
        d = np.array(q) - np.array(p); L = np.linalg.norm(d); e = d / L
        # 外接線: 法線 n（外向き = 進行方向の右）を求める: n·e = (r1 - r2)/L
        cth = (r1 - r2) / L; sth = math.sqrt(1 - cth * cth)
        right = np.array([e[1], -e[0]])
        nrm = cth * e + sth * right
        tang.append((np.array(p) + r1 * nrm, np.array(q) + r2 * nrm, nrm))
    pts = []
    for i in range(3):
        a0, a1, _ = tang[i]
        pts += [a0 + (a1 - a0) * t for t in np.linspace(0, 1, 30, endpoint=False)]
        # 次の円の弧（接点から次の接線の始点まで、時計回り = 外側を回る）
        (cc, r) = circles[(i + 1) % 3]
        s_ang = math.atan2(*(tang[i][2][::-1]))
        e_ang = math.atan2(*(tang[(i + 1) % 3][2][::-1]))
        while e_ang < s_ang: e_ang += 2 * math.pi
        for t in np.linspace(0, 1, 40, endpoint=False):
            a = s_ang + (e_ang - s_ang) * t
            pts.append(np.array(cc) + r * np.array([math.cos(a), math.sin(a)]))
    return pts, tang


def timing():
    md = Model()
    for b in ("R", "L"):
        z = CHAIN_Z_BANK[b]
        pts, tang = chain_path(b)
        P = np.array(pts)
        seg = np.linalg.norm(np.roll(P, -1, 0) - P, axis=1)
        cum = np.concatenate([[0], np.cumsum(seg)])
        total = cum[-1]
        nlinks = int(total / CHAIN_PITCH)
        pitch = total / nlinks
        def at(sv):
            sv %= total
            i = np.searchsorted(cum, sv) - 1; i = max(0, min(i, len(P) - 1))
            t = (sv - cum[i]) / max(seg[i], 1e-9)
            return P[i] + (P[(i + 1) % len(P)] - P[i]) * t
        plates, pins = [], []
        for k in range(nlinks):
            A = at(k * pitch); B = at((k + 1) * pitch)
            d = B - A; L = np.linalg.norm(d); e = d / L
            mid = (A + B) / 2
            off = 0.0047 if k % 2 == 0 else 0.0033
            for sz in (-1, 1):
                plates.append(tri_box([mid[0], mid[1], z + sz * off], [e[0], e[1], 0], [-e[1], e[0], 0], [L + 0.0045, 0.0058, 0.0011]))
            pins.append(trimesh.creation.cylinder(0.0017, 0.0115, sections=8).apply_translation([A[0], A[1], z]))
        md.add(trimesh.util.concatenate(plates), "chain")
        md.add(trimesh.util.concatenate(pins), "polished")
        # ガイド（直線部の外側）とテンショナー
        guides = []
        for (a0, a1, nrm) in tang:
            L = np.linalg.norm(a1 - a0)
            if L < 0.12: continue
            e = (a1 - a0) / L
            mid = (a0 + a1) / 2 + nrm * 0.0065
            guides.append(tri_box([mid[0], mid[1], z], [e[0], e[1], 0], [nrm[0], nrm[1], 0], [L * 0.75, 0.005, 0.013]))
            base = (a0 + a1) / 2 + nrm * 0.016
            guides.append(tri_box([base[0], base[1], z], [e[0], e[1], 0], [nrm[0], nrm[1], 0], [L * 0.55, 0.012, 0.006]))
        md.add(trimesh.util.concatenate(guides), "guide")
    return md


# ------------------------------------------------------------------ 架台とダイナモ
FLOOR = -CY


def stand_dyno():
    md = Model()
    blue = []
    for x in (-0.20, 0.20):
        blue.append(box(0.05, 0.04, 1.30).translate([x, FLOOR + 0.02, 0.25]))
    for z in (-0.16, 0.10, 0.62, 0.86):
        blue.append(box(0.45, 0.04, 0.05).translate([0, FLOOR + 0.02, z]))
    # エンジンの脚（ブロックの横へ）
    for sx in (-1, 1):
        for z in (-0.12, 0.10):
            blue.append(box(0.04, -0.075 - FLOOR - 0.04, 0.04).translate([sx * 0.20, (FLOOR + 0.04 - 0.075) / 2, z]))
            blue.append(box(0.08, 0.012, 0.07).translate([sx * 0.165, -0.075, z]))
            blue.append(box(0.012, 0.05, 0.07).translate([sx * 0.131, -0.075, z]))
    md.add(union(blue), "paint_blue")
    # ダイナモ（渦電流式の箱）
    dz0, dz1 = 0.405, 0.78
    body = cyl(0.165, dz1 - dz0).translate([0, 0, dz0])
    for z in np.linspace(dz0 + 0.04, dz1 - 0.04, 9):
        body = body + cyl(0.175, 0.008, center=True).translate([0, 0, z])
    body = body - cyl(0.03, 0.02).translate([0, 0, dz0 - 0.01])
    md.add(body, "paint_gray")
    y0, y1 = FLOOR + 0.04, -0.14
    ped = box(0.30, y1 - y0, 0.30).translate([0, (y0 + y1) / 2, (dz0 + dz1) / 2])
    md.add(ped, "paint_gray")
    md.add(box(0.12, 0.08, 0.12).translate([0, 0.20, (dz0 + dz1) / 2]), "black")    # 端子箱
    md.add(cyl(0.012, 0.25).rotate([0, 90, 0]).translate([0.0, 0.215, (dz0 + dz1) / 2]).translate([0, 0, 0]), "black")
    # 継手のカバー（上半分の網の代わりに細い棒）
    bars = []
    for k in range(7):
        a = math.pi * (0.15 + 0.7 * k / 6)
        bars.append(cyl(0.003, 0.18, 8).translate([0.085 * math.cos(a), 0.085 * math.sin(a), 0.225]))
    for z in (0.23, 0.40):
        ring = cyl(0.088, 0.006, center=True).translate([0, 0, z]) - cyl(0.082, 0.01, center=True).translate([0, 0, z])
        bars.append(ring.trim_by_plane([0, 1, 0], -0.02))
    md.add(union(bars), "paint_yellow")
    return md


if __name__ == "__main__":
    which = sys.argv[2:] or ["all"]
    def want(n): return "all" in which or n in which
    jobs = []
    if want("block"): jobs.append(("block", engine_block))
    if want("head"):
        jobs.append(("head_R", lambda: cylinder_head("R"))); jobs.append(("head_L", lambda: cylinder_head("L")))
    if want("intake"): jobs.append(("intake", intake))
    if want("exhaust"): jobs.append(("exhaust", exhaust_turbo))
    if want("timing"): jobs.append(("timing", timing))
    if want("stand"): jobs.append(("stand", stand_dyno))
    for name, fn in jobs:
        size, nt = fn().export(os.path.join(OUT, name + ".glb"))
        print(f"{name}: {size/1024:.0f} KB, {nt} tris")
