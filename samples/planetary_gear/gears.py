# 遊星歯車（assets/scenes/planetary_gear.xml）の部品メッシュを作る。
# 歯数は CAD 図どおり: 太陽歯車 A 16・遊星歯車 B 22 × 4・内歯車 C 60（遊星キャリヤ D）。
# 歯形はインボリュート（圧力角 20°、標準歯車 = 転位なし）。寸法の唯一の定義はこのファイルの
# 上の定数で、シーンのボディの位置・歯車拘束の比も同じ値から出す（README.md）。
#
#   pip install manifold3d trimesh numpy scipy networkx
#   cd samples/planetary_gear && python3 gears.py
#
# 出力: assets/models/planetary/{sun,planet,ring,carrier}.glb
# 各部品はそのボディの座標系（歯車の中心 = 原点、回転軸 = +Z、歯幅の中央が z = 0）。
import math
import os
import sys

import numpy as np
from manifold3d import CrossSection

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "v6_engine"))
import meshlib as ml  # noqa: E402  （V6 と同じ CSG → .glb の道具）

# ---- 寸法（m）---------------------------------------------------------------
M = 0.005             # モジュール
ZA, ZB, ZC = 16, 22, 60  # 太陽 A・遊星 B・内歯車 C の歯数
N_PLANETS = 4
ALPHA = math.radians(20.0)  # 圧力角
W = 0.025             # 歯幅
BACKLASH = 0.04 * M   # ピッチ円上の歯厚を少し薄くする（見た目のすき間）
RA, RB, RC = M * ZA / 2, M * ZB / 2, M * ZC / 2   # ピッチ円の半径
CENTER_DIST = RA + RB                              # 太陽 ⇄ 遊星の中心距離
PLANET_ANGLES = [90.0 * i for i in range(N_PLANETS)]  # 遊星の位置（度、X 軸から反時計回り）
RING_OUTER = RC + 1.25 * M + 0.03                  # 内歯車の外周
PLANET_BORE = 0.020   # 遊星の軸穴（図の大きな丸）
PIN_R = 0.0185        # キャリヤのピン
SUN_HUB = 0.014       # 太陽の軸穴（入力軸）
PLATE_Z = (-W / 2 - 0.0105, -W / 2 - 0.0025)       # キャリヤの板（歯の後ろ）
assert ZC == ZA + 2 * ZB, "同軸の条件 Zc = Za + 2 Zb"
assert (ZA + ZC) % N_PLANETS == 0, "等間隔に組める条件 (Za + Zc) / N が整数"


def inv(a):
    return math.tan(a) - a


def half_angle(z, rho, space=False):
    """ピッチ円の歯厚が π m / 2 − バックラッシのインボリュート歯の、半径 rho での半角（rad）。
    内歯車の歯溝も同じ形（歯溝 = 相手の外歯車の歯の形）なので同じ式を使い、space = True
    なら歯溝をバックラッシぶん広げる（歯を薄くするのと同じ向きのすき間）。"""
    r = M * z / 2
    rb = r * math.cos(ALPHA)
    rho = max(rho, rb)
    a = math.acos(rb / rho)
    s = math.pi * M / 2 + (BACKLASH if space else -BACKLASH)
    return s / (2 * r) + inv(ALPHA) - inv(a)


def pt(rho, ang):
    return (rho * math.cos(ang), rho * math.sin(ang))


def external_profile(z, phase_deg, nflank=10, narc=4):
    """外歯車の輪郭（反時計回り）。歯の中心の角度 = phase + k 360/z。"""
    r = M * z / 2
    rb = r * math.cos(ALPHA)
    ra, rf = r + M, r - 1.25 * M
    r0 = max(rb, rf)
    pts = []
    for k in range(z):
        c = math.radians(phase_deg) + 2 * math.pi * k / z
        nxt = c + 2 * math.pi / z
        # 歯元（基礎円より下は半径方向の直線）
        if rf < rb:
            pts.append(pt(rf, c - half_angle(z, rb)))
        rhos = [r0 + (ra - r0) * i / nflank for i in range(nflank + 1)]
        for rho in rhos:                       # 右の歯面（下 → 上）
            pts.append(pt(rho, c - half_angle(z, rho)))
        a0, a1 = c - half_angle(z, ra), c + half_angle(z, ra)
        for i in range(1, narc):               # 歯先の円弧
            pts.append(pt(ra, a0 + (a1 - a0) * i / narc))
        for rho in reversed(rhos):             # 左の歯面（上 → 下）
            pts.append(pt(rho, c + half_angle(z, rho)))
        if rf < rb:
            pts.append(pt(rf, c + half_angle(z, rb)))
        b0, b1 = c + half_angle(z, max(rb, rf)), nxt - half_angle(z, max(rb, rf))
        for i in range(1, narc):               # 歯底の円弧
            pts.append(pt(rf, b0 + (b1 - b0) * i / narc))
    return pts


def internal_hole(z, phase_deg, nflank=10, narc=4):
    """内歯車の内側の輪郭（反時計回り）。歯溝の中心の角度 = phase + k 360/z。
    歯先が内側（r − m）、歯底が外側（r + 1.25 m）。"""
    r = M * z / 2
    rtip, rroot = r - M, r + 1.25 * M
    pts = []
    for k in range(z):
        c = math.radians(phase_deg) + 2 * math.pi * k / z
        nxt = c + 2 * math.pi / z
        rhos = [rtip + (rroot - rtip) * i / nflank for i in range(nflank + 1)]
        for rho in rhos:                       # 歯溝の右側（内 → 外）
            pts.append(pt(rho, c - half_angle(z, rho, True)))
        a0, a1 = c - half_angle(z, rroot, True), c + half_angle(z, rroot, True)
        for i in range(1, narc):               # 歯溝の底（外側の円弧）
            pts.append(pt(rroot, a0 + (a1 - a0) * i / narc))
        for rho in reversed(rhos):             # 歯溝の左側（外 → 内）
            pts.append(pt(rho, c + half_angle(z, rho, True)))
        b0, b1 = c + half_angle(z, rtip, True), nxt - half_angle(z, rtip, True)
        for i in range(1, narc):               # 歯先（内側の円弧）
            pts.append(pt(rtip, b0 + (b1 - b0) * i / narc))
    return pts


def circle(r, n=None):
    return CrossSection([ml.circle_pts(0, 0, r, n or ml.seg_for(r))])


def plate(cs, z0, z1):
    return ml.extrude_z(cs, z0, z1)


# 歯の位相: 遊星の位置 θ で、太陽と内歯車は歯溝、遊星は歯（θ と θ + 180° の両方）を向ける。
#   太陽 A:   歯の中心 = 180/Za + k 360/Za（90° ごとの θ が歯溝）
#   遊星 B:   ボディを θ 回して置くので、ローカルで歯の中心 = k 360/Zb（0° = 内歯車側）
#   内歯車 C: 歯溝の中心 = θ + k 360/Zc（θ = 0 から始める）
SUN_PHASE = 180.0 / ZA
PLANET_PHASE = 0.0
RING_SPACE_PHASE = 0.0
for th in PLANET_ANGLES:
    assert abs(((th - SUN_PHASE) / (360.0 / ZA)) % 1 - 0.5) < 1e-9, "太陽: θ が歯溝"
    assert abs(((th - RING_SPACE_PHASE) / (360.0 / ZC)) % 1) < 1e-9, "内歯車: θ が歯溝"
assert (180.0 / (360.0 / ZB)) % 1 == 0, "遊星: 太陽側と内歯車側の両方が歯"

ml.MATS.update({
    "gear_sun":     ((0.80, 0.60, 0.26), 1.0, 0.28),   # 太陽: 真鍮色
    "gear_planet":  ((0.74, 0.76, 0.79), 1.0, 0.22),   # 遊星: 研磨鋼
    "gear_ring":    ((0.50, 0.52, 0.55), 1.0, 0.34),   # 内歯車: 鋼
    "carrier":      ((0.10, 0.22, 0.45), 0.0, 0.45),   # キャリヤ: 青の塗装
    "pin":          ((0.82, 0.83, 0.85), 1.0, 0.12),
})


def build_sun():
    m = ml.Model()
    g = plate(CrossSection([external_profile(ZA, SUN_PHASE)]) - circle(SUN_HUB), -W / 2, W / 2)
    m.add(g, "gear_sun")
    shaft = ml.cyl(SUN_HUB, 0.26).translate([0, 0, -W / 2])   # 入力軸（手前 +Z へ）
    m.add(shaft, "polished")
    return m


def build_planet():
    m = ml.Model()
    g = plate(CrossSection([external_profile(ZB, PLANET_PHASE)]) - circle(PLANET_BORE), -W / 2, W / 2)
    m.add(g, "gear_planet")
    return m


def build_ring():
    m = ml.Model()
    hole = CrossSection([internal_hole(ZC, RING_SPACE_PHASE)])
    g = plate(circle(RING_OUTER, 180) - hole, -W / 2, W / 2)
    m.add(g, "gear_ring")
    return m


def build_carrier():
    """キャリヤ D: 4 本のピンを結ぶ板（図の破線）+ ピン + 出力軸（奥 −Z へ）。"""
    m = ml.Model()
    lobes = [circle(0.034).translate(ml_xy(CENTER_DIST, th)) for th in PLANET_ANGLES]
    cs = circle(0.072)
    for c in lobes:
        cs = cs + c
    cs = cs.hull()                                     # ピンの外側を包む丸い四角
    # 遊星の間をくぼませる（図の破線の内側へ入る線）
    for th in PLANET_ANGLES:
        cs = cs - circle(0.030).translate(ml_xy(CENTER_DIST * 1.32, th + 45.0))
    m.add(plate(cs, *PLATE_Z), "carrier")
    for th in PLANET_ANGLES:
        x, y = ml_xy(CENTER_DIST, th)
        pin = ml.cyl(PIN_R, W + 0.0105 + 0.004).translate([x, y, PLATE_Z[0]])
        m.add(pin, "pin")
    m.add(ml.cyl(0.02, 0.24).translate([0, 0, PLATE_Z[0] - 0.24]), "carrier")
    return m


def ml_xy(r, deg):
    a = math.radians(deg)
    return [r * math.cos(a), r * math.sin(a)]


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "models", "planetary")
    os.makedirs(out, exist_ok=True)
    for name, fn in [("sun", build_sun), ("planet", build_planet), ("ring", build_ring),
                     ("carrier", build_carrier)]:
        size, tris = fn().export(os.path.join(out, name + ".glb"))
        print(f"{name}.glb  {size / 1024:.0f} KB  {tris} triangles")
