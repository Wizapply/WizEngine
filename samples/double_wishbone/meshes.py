#!/usr/bin/env python3
"""ダブルウィッシュボーンの部品メッシュ（assets/models/double_wishbone/*.glb）を作る。

  pip install manifold3d trimesh numpy scipy networkx
  python3 meshes.py

寸法は gen.py のハードポイントが唯一の定義。部品は設計姿勢のワールド座標で組み、
最後にそのボディの座標系（gen.body_frames() の中心・回転）へ直して書き出す。
シーンではプレハブの部品（type="mesh"、pos 0・倍率 1）として各ボディに付く。

色は部品の役割ごとに分けてある（凡例は gen.py が文書の先頭に書く）。
コイルばねは上下 2 つのボディ（ロッド側・ダンパ本体側）に半分ずつ載せてあるので、
サスペンションが縮むと真ん中で重なり、伸びると間が開く（形は伸び縮みしないが、
ばねの長さの変化は見える）。
"""
import math
import os
import sys

import numpy as np
import trimesh

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "v6_engine"))

import gen  # noqa: E402
from gen import CL, CU, LBJ, LEG_L, LEG_U, LP, R_TIRE, TI, TO, UBJ, UP, W_TIRE, WC  # noqa: E402
import meshlib as ml  # noqa: E402

OUT = os.path.join(HERE, "..", "..", "assets", "models", "double_wishbone")

# 役割ごとの材質（リニアの色, metallic, roughness）
ml.MATS.update({
    "body":    ((0.05, 0.09, 0.22), 0.0, 0.45),   # 車体（紺）
    "lower":   ((0.90, 0.32, 0.04), 0.0, 0.40),   # ロアアーム（橙）
    "upper":   ((0.08, 0.45, 0.14), 0.0, 0.40),   # アッパーアーム（緑）
    "knuckle": ((0.70, 0.71, 0.72), 0.9, 0.40),   # ナックル（銀）
    "tierod":  ((0.05, 0.22, 0.75), 0.0, 0.35),   # タイロッド（青）
    "coil":    ((0.75, 0.04, 0.03), 0.2, 0.35),   # コイル（赤）
    "damper":  ((0.03, 0.03, 0.035), 0.3, 0.50),  # ダンパ本体（黒）
    "chrome":  ((0.85, 0.86, 0.88), 1.0, 0.08),   # ピストンロッド
    "ball":    ((0.85, 0.62, 0.20), 1.0, 0.30),   # ボールジョイント（真鍮）
    "bush":    ((0.015, 0.015, 0.015), 0.0, 0.75),  # ブッシュ（ゴム）
    "tire":    ((0.02, 0.02, 0.022), 0.0, 0.85),
    "rim":     ((0.75, 0.76, 0.78), 1.0, 0.22),
    "disc":    ((0.40, 0.40, 0.41), 1.0, 0.35),
    "caliper": ((0.90, 0.62, 0.02), 0.0, 0.35),   # キャリパ（黄）
})

X = np.array([1.0, 0, 0])
Y = np.array([0, 1.0, 0])
Z = np.array([0, 0, 1.0])
SD = gen.unit(CU - CL)  # コイルオーバーの軸（下 → 上）


def cyl_along(center, axis, length, r, seg=None):
    """中心と軸の向きで置く円柱。"""
    axis = gen.unit(np.asarray(axis, float))
    c = np.asarray(center, float)
    return ml.cyl_between(c - axis * length / 2, c + axis * length / 2, r, seg)


def sphere(center, r):
    return ml.Manifold.sphere(r, 32).translate(list(center))


def oriented_box(center, xdir, ydir, size):
    return ml.tri_box(center, gen.unit(np.asarray(xdir, float)), gen.unit(np.asarray(ydir, float)), size)


# ---- 部品ごと（ワールド座標の設計姿勢）--------------------------------------------
def arm(m, pivot, leg, bj, mat, r_tube, r_bush):
    """A アーム: 車体側の 2 つのブッシュ（Z 軸）からボールジョイントへの管 2 本。"""
    for s in (-1, 1):
        p = pivot + Z * (s * leg)
        m.add(ml.cyl_between(p, bj, r_tube), mat)
        m.add(cyl_along(p, Z, 0.055, r_bush), "bush")                 # ゴムブッシュ
        m.add(cyl_along(p, Z, 0.07, r_bush * 0.45), "steel")          # 芯のカラー
    # 2 本をつなぐ補強（三角形の中ほど）
    a = pivot + Z * (-leg) + 0.45 * (bj - (pivot + Z * (-leg)))
    b = pivot + Z * (leg) + 0.45 * (bj - (pivot + Z * (leg)))
    m.add(ml.cyl_between(a, b, r_tube * 0.8), mat)
    # ボールジョイントのハウジング（腕の色）と玉（真鍮）
    m.add(cyl_along(bj, Y, 0.045, r_tube * 2.0), mat)
    m.add(sphere(bj, r_tube * 1.7), "ball")


def build_lower_arm():
    m = ml.Model()
    arm(m, LP, LEG_L, LBJ, "lower", 0.017, 0.030)
    # コイルオーバーの下端の取り付け（腕の上の台座）
    m.add(oriented_box(CL - Y * 0.025, X, Y, (0.07, 0.025, 0.09)), "lower")
    return m


def build_upper_arm():
    m = ml.Model()
    arm(m, UP, LEG_U, UBJ, "upper", 0.014, 0.026)
    return m


def build_upright():
    m = ml.Model()
    kp = gen.unit(UBJ - LBJ)
    # キングピンの柱（両ボールジョイントの間）
    m.add(ml.cyl_between(LBJ + kp * 0.02, UBJ - kp * 0.02, 0.024), "knuckle")
    # スピンドルの付け根（ハブのボス）とスピンドル
    t = (WC[1] - LBJ[1]) / (UBJ[1] - LBJ[1])
    base = LBJ + t * (UBJ - LBJ)
    m.add(ml.cyl_between(base, np.array([WC[0] - 0.085, WC[1], 0.0]), 0.055), "knuckle")
    m.add(ml.cyl_between(np.array([WC[0] - 0.09, WC[1], 0.0]), WC + X * 0.03, 0.022), "steel")
    # ナックルアーム（後ろへ出てタイロッドの外側の玉へ）
    t2 = (TO[1] - LBJ[1]) / (UBJ[1] - LBJ[1])
    kb = LBJ + t2 * (UBJ - LBJ)
    m.add(ml.cyl_between(kb, TO, 0.015), "knuckle")
    # ブレーキキャリパ（ディスクの後ろ上、黄）
    disc_x = WC[0] - 0.055
    cal = np.array([disc_x, WC[1] + 0.07, 0.10])
    m.add(oriented_box(cal, X, gen.unit(np.array([0, 0.6, 0.8])), (0.07, 0.11, 0.05)), "caliper")
    m.add(ml.cyl_between(base + Z * 0.02, cal - X * 0.03, 0.012), "knuckle")
    return m


def build_wheel():
    m = ml.Model()
    # タイヤ: 断面（r, z）を X 軸まわりに回す（revolve_z の軸 Z → X）
    rw = W_TIRE / 2
    rim_r = 0.215
    sh = 0.035     # 肩の丸みの半径
    poly = [(rim_r, -rw + 0.012), (R_TIRE - 0.07, -rw - 0.004)]
    poly += [(R_TIRE - sh + sh * math.cos(a), -rw + sh + sh * math.sin(a))
             for a in np.linspace(-math.pi / 2, 0, 8)]
    poly += [(R_TIRE - sh + sh * math.cos(a), rw - sh + sh * math.sin(a))
             for a in np.linspace(0, math.pi / 2, 8)]
    poly += [(R_TIRE - 0.07, rw + 0.004), (rim_r, rw - 0.012)]
    tire = ml.revolve_z(poly, 96).rotate([0, 90, 0]).translate(list(WC))
    m.add(tire, "tire")
    # リム（樽）と 5 本スポーク・センターキャップ
    barrel = ml.revolve_z([(rim_r - 0.012, -rw + 0.01), (rim_r + 0.006, -rw + 0.01),
                           (rim_r + 0.006, rw - 0.01), (rim_r - 0.012, rw - 0.01)], 96)
    m.add(barrel.rotate([0, 90, 0]).translate(list(WC)), "rim")
    face_x = WC[0] + 0.045
    for k in range(5):
        a = 2 * math.pi * k / 5
        d = np.array([0, math.cos(a), math.sin(a)])
        m.add(oriented_box(np.array([face_x, WC[1], WC[2]]) + d * 0.125, X, d, (0.025, 0.19, 0.045)), "rim")
    m.add(cyl_along(np.array([face_x, WC[1], WC[2]]), X, 0.035, 0.06), "rim")
    m.add(cyl_along(np.array([face_x + 0.02, WC[1], WC[2]]), X, 0.012, 0.035), "chrome")
    # ブレーキディスク（ホイールと一緒に回る）
    disc = ml.revolve_z([(0.075, -0.013), (0.16, -0.013), (0.16, 0.013), (0.075, 0.013)], 72)
    m.add(disc.rotate([0, 90, 0]).translate([WC[0] - 0.055, WC[1], WC[2]]), "disc")
    m.add(cyl_along(np.array([WC[0] - 0.02, WC[1], WC[2]]), X, 0.06, 0.075), "disc")
    return m


def build_tie_rod():
    m = ml.Model()
    d = gen.unit(TO - TI)
    m.add(ml.cyl_between(TI + d * 0.02, TO - d * 0.04, 0.010), "tierod")
    m.add(cyl_along(TO - d * 0.05, d, 0.05, 0.016), "tierod")   # タイロッドエンド
    m.add(sphere(TI, 0.020), "ball")
    m.add(sphere(TO, 0.018), "ball")
    return m


# コイルばね: 下のばね座（ダンパ本体）から上のばね座（ロッド側）までの螺旋を半分ずつ
SEAT_LO = 0.12    # CL から下のばね座まで
SEAT_HI = 0.06    # CU から上のばね座まで
COIL_R = 0.050
WIRE_R = 0.0075
TURNS = 6.5


def coil_points(t0, t1):
    e1 = gen.unit(np.cross(SD, Z))
    e2 = np.cross(SD, e1)
    p0 = CL + SD * SEAT_LO
    L = np.linalg.norm(CU - CL) - SEAT_LO - SEAT_HI
    pts = []
    n = int(TURNS * 48 * (t1 - t0)) + 2
    for t in np.linspace(t0, t1, n):
        th = 2 * math.pi * TURNS * t
        pts.append(p0 + SD * (L * t) + COIL_R * (math.cos(th) * e1 + math.sin(th) * e2))
    return pts


def build_shock_body():
    """下半分: ダンパ本体（黒）・下の目玉ブッシュ・下のばね座・コイルの下半分。"""
    m = ml.Model()
    m.add(ml.cyl_between(CL + SD * 0.03, CL + SD * 0.33, 0.026), "damper")
    m.add(cyl_along(CL, Z, 0.05, 0.022), "bush")
    m.add(cyl_along(CL, Z, 0.06, 0.009), "steel")
    m.add(cyl_along(CL + SD * (SEAT_LO - 0.006), SD, 0.01, 0.062), "steel")
    m.add(ml.tube(coil_points(0.0, 0.5), WIRE_R, seg=12), "coil")
    return m


def build_shock_rod():
    """上半分: ピストンロッド（クロム）・アッパーマウント・上のばね座・コイルの上半分。"""
    m = ml.Model()
    m.add(ml.cyl_between(CU - SD * 0.42, CU - SD * 0.03, 0.009), "chrome")
    m.add(cyl_along(CU, SD, 0.03, 0.045), "bush")
    m.add(cyl_along(CU - SD * (SEAT_HI - 0.006), SD, 0.01, 0.062), "steel")
    m.add(ml.tube(coil_points(0.5, 1.0), WIRE_R, seg=12), "coil")
    return m


def build_chassis():
    """車体（ばね上）: 車体の箱・サブフレーム・アームのブラケット・ストラットタワー・ラック。"""
    m = ml.Model()
    c = gen.CHASSIS
    x_in = c[0] - 0.14     # 車体の箱の内側の面
    x_out = c[0] + 0.06    # 車体の箱の外側の面（サブフレームの面）
    m.add(oriented_box(np.array([0.5 * (x_in + x_out), c[1] + 0.05, 0.0]), X, Y,
                       (x_out - x_in, 0.62, 0.24)), "body")
    # ロアアームのブラケット（ブッシュを前後から挟む）
    for s in (-1, 1):
        for e in (-1, 1):
            zc = s * LEG_L + e * 0.036
            m.add(oriented_box(np.array([0.5 * (x_out + LP[0] + 0.03), LP[1], zc]), X, Y,
                               (LP[0] + 0.03 - x_out, 0.07, 0.008)), "body")
    m.add(oriented_box(np.array([0.5 * (x_out + LP[0]), LP[1] - 0.04, 0.0]), X, Y,
                       (LP[0] - x_out, 0.02, 2 * LEG_L + 0.08)), "body")
    # アッパーアームのブラケット
    for s in (-1, 1):
        for e in (-1, 1):
            zc = s * LEG_U + e * 0.032
            m.add(oriented_box(np.array([0.5 * (x_out + UP[0] + 0.03), UP[1], zc]), X, Y,
                               (UP[0] + 0.03 - x_out, 0.06, 0.008)), "body")
    m.add(oriented_box(np.array([0.5 * (x_out + UP[0]), UP[1] + 0.035, 0.0]), X, Y,
                       (UP[0] - x_out, 0.015, 2 * LEG_U + 0.07)), "body")
    # ストラットタワー（コイルオーバーの上端を受ける板）
    tw = np.array([0.5 * (x_out + CU[0] + 0.07), CU[1] + 0.03, 0.0])
    m.add(oriented_box(tw, X, Y, (CU[0] + 0.07 - x_out, 0.025, 0.11)), "body")
    # ステアリングラック（ハウジングとブーツ）
    m.add(ml.cyl_between(np.array([x_in + 0.02, TI[1], TI[2]]), TI - X * 0.07, 0.025), "steel")
    m.add(ml.cyl_between(TI - X * 0.07, TI - X * 0.015, 0.018), "bush")
    return m


BUILDERS = dict(
    chassis=build_chassis,
    lower_arm=build_lower_arm,
    upper_arm=build_upper_arm,
    upright=build_upright,
    wheel=build_wheel,
    tie_rod=build_tie_rod,
    shock_rod=build_shock_rod,
    shock_body=build_shock_body,
)


def to_local(model, center, R):
    """ワールドの設計姿勢 → ボディの座標系（v_local = R^T (v - c)）。"""
    T = np.eye(4)
    T[:3, :3] = R.T
    T[:3, 3] = -R.T @ center
    parts = []
    for t, mat in model.parts:
        t = t.copy()
        t.apply_transform(T)
        parts.append((t, mat))
    model.parts = parts
    return model


def main():
    os.makedirs(OUT, exist_ok=True)
    frames = gen.body_frames()
    total = 0
    for name, fn in BUILDERS.items():
        c, R = frames[name]
        m = to_local(fn(), c, R)
        # Manifold は add の時点で三角形メッシュになっている
        size, nt = m.export(os.path.join(OUT, f"{name}.glb"))
        total += size
        print(f"{name:12s} {nt:7d} tris {size / 1024:7.1f} KB")
    print(f"total {total / 1024:.0f} KB")


if __name__ == "__main__":
    main()
