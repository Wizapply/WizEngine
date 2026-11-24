#!/usr/bin/env python3
"""ダブルウィッシュボーン式サスペンションの見本シーン（assets/scenes/double_wishbone.xml）を作る。

クォーターカーの加振試験台: 車体の 1/4（sprung mass）を縦の直動ジョイントで支え、
右前輪のダブルウィッシュボーン（アッパー / ロアの A アーム・アップライト・タイロッド・
コイルオーバー・ハブ）を Chrono の拘束で組み、タイヤの下の加振台を位置モータで
上下させる。リンクの拘束・ばね・ダンパの計算はすべて Chrono（ジョイントは
ChLinkLock* / ChLinkUniversal、ばねとダンパは ChLinkTSDA）。

このスクリプトが寸法の唯一の定義（ハードポイント・質量・ばね）。やること:
  1. ハードポイントから各ボディの中心と向き（オイラー角）を出す
  2. 機構の運動学を解いて（ロアアームの角度 → アッパーアーム → アップライト）
     レバー比・ホイールレート・キャンバー変化・バンプステアを表にする
  3. 設計姿勢で釣り合うコイルの予圧（ばねの自然長）とタイヤばねの自然長を
     仮想仕事から求める（シミュレートを始めても車体が沈まない・跳ねない）
  4. シーン文書の XML を書く。制御器の式（rig.txt の数式テキスト）は
     ノードのグラフへの変換が要るので、エンジンの変換器（formulaFromText）を
     通した <formula> 要素を --formula-xml で受け取って差し込む。

  python3 gen.py --report                         # 表だけ
  python3 gen.py --formula-xml rig_formula.xml    # XML を ../../assets/scenes へ
"""
import argparse
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "assets", "scenes", "double_wishbone.xml")

G = 9.81
# 座標: +Y 上、+X 右（車の外側）、前 = -Z。右前輪。単位は m。
Y0 = 0.10  # 加振台の上面の高さ（地面から少し上げて見えるように）

# ---- ハードポイント（設計姿勢 = 1G の静止状態）-------------------------------
R_TIRE = 0.33          # タイヤの半径
W_TIRE = 0.225         # タイヤの幅（見た目）
WC = np.array([0.80, Y0 + R_TIRE, 0.0])     # ホイールの中心
LBJ = np.array([0.72, Y0 + 0.16, 0.0])      # ロアのボールジョイント
UBJ = np.array([0.67, Y0 + 0.56, 0.0])      # アッパーのボールジョイント
LP = np.array([0.30, Y0 + 0.18, 0.0])       # ロアアームの車体側の回転軸（Z 軸まわり）
UP = np.array([0.45, Y0 + 0.53, 0.0])       # アッパーアームの車体側の回転軸
LEG_L = 0.16           # ロアアームの車体側の取り付けの前後の幅（半分、見た目）
LEG_U = 0.12           # アッパーアームの同じ値
TI = np.array([0.33, Y0 + 0.25, 0.14])      # タイロッドの内側（ステアリングラック、固定）
TO = np.array([0.71, Y0 + 0.26, 0.14])      # タイロッドの外側（ナックルアーム）
CL = np.array([0.58, Y0 + 0.19, 0.0])       # コイルオーバーの下端（ロアアーム上）
CU = np.array([0.49, Y0 + 0.80, 0.0])       # コイルオーバーの上端（車体）
TUBE_OFF = 0.12        # コイルオーバー上端 → 上のボディ（shock_rod = ピストンロッド）の中心
ROD_OFF = 0.12         # コイルオーバー下端 → 下のボディ（shock_body = ダンパ本体）の中心
PAD = np.array([WC[0], Y0 - 0.05, 0.0])     # 加振台の中心（上面 = Y0）
CHASSIS = np.array([0.16, Y0 + 0.50, 0.0])  # 車体（1/4）の中心

# ---- 質量 (kg) -----------------------------------------------------------------
M = dict(chassis=400.0, lower_arm=5.0, upper_arm=2.5, upright=8.0, wheel=22.0,
         tie_rod=1.0, shock_rod=1.0, shock_body=1.5)
M_PAD = 20.0

# ---- ばね・ダンパ ---------------------------------------------------------------
TARGET_RIDE_HZ = 1.4   # 車体のばね上の固有振動数の目標（タイヤを含まない）
DAMPING_RATIO = 0.30   # ばね上の減衰比の目標
K_TIRE = 250000.0      # タイヤの縦ばね (N/m)
C_TIRE = 150.0         # タイヤの減衰 (N s/m)


def unit(v):
    return v / np.linalg.norm(v)


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1.0]])


def rot_axis(axis, a):
    axis = unit(axis)
    x, y, z = axis
    c, s, C = math.cos(a), math.sin(a), 1 - math.cos(a)
    return np.array([[c + x * x * C, x * y * C - z * s, x * z * C + y * s],
                     [y * x * C + z * s, c + y * y * C, y * z * C - x * s],
                     [z * x * C - y * s, z * y * C + x * s, c + z * z * C]])


def rot_from_to(a, b):
    """単位ベクトル a を b へ回す最小の回転。"""
    a, b = unit(a), unit(b)
    v = np.cross(a, b)
    s = np.linalg.norm(v)
    c = float(np.dot(a, b))
    if s < 1e-12:
        return np.eye(3)
    return rot_axis(v, math.atan2(s, c))


def euler_deg(R):
    """R = Rz * Ry * Rx の (x, y, z) 度（エンジンの SceneMath と同じ並び）。"""
    y = math.asin(max(-1.0, min(1.0, -R[2, 0])))
    x = math.atan2(R[2, 1], R[2, 2])
    z = math.atan2(R[1, 0], R[0, 0])
    return np.degrees([x, y, z])


def circle_intersect(c1, r1, c2, r2, near):
    """XY 平面の 2 円の交点のうち near に近い方。"""
    d = np.linalg.norm(c2 - c1)
    a = (r1 * r1 - r2 * r2 + d * d) / (2 * d)
    h = math.sqrt(max(0.0, r1 * r1 - a * a))
    p = c1 + a * (c2 - c1) / d
    perp = np.array([-(c2 - c1)[1], (c2 - c1)[0]]) / d
    s1, s2 = p + h * perp, p - h * perp
    return s1 if np.linalg.norm(s1 - near) < np.linalg.norm(s2 - near) else s2


# ---- 運動学: ロアアームの角度 phi（車体に対して）→ 各点 -------------------------
R_UA = np.linalg.norm((UBJ - UP)[:2])
L_KP = np.linalg.norm(UBJ - LBJ)
L_TR = np.linalg.norm(TO - TI)


def solve(phi):
    """ロアアームを phi (rad, +Z まわり) 回した姿勢。車体は固定で見る。"""
    Rl = rot_z(phi)
    lbj = LP + Rl @ (LBJ - LP)
    # アッパーのボールジョイント: UP を中心の円と、lbj から L_KP の円（Z = 0 の面内）
    ubj2 = circle_intersect(UP[:2], R_UA, lbj[:2], L_KP, UBJ[:2])
    ubj = np.array([ubj2[0], ubj2[1], 0.0])
    # アップライト: キングピン（LBJ → UBJ）を合わせる回転のあと、キングピンまわりに
    # theta 回してタイロッドの長さを合わせる。
    R0 = rot_from_to(UBJ - LBJ, ubj - lbj)

    def pose(theta):
        return rot_axis(ubj - lbj, theta) @ R0

    def f(theta):
        to = lbj + pose(theta) @ (TO - LBJ)
        return np.linalg.norm(to - TI) - L_TR

    th = 0.0
    for _ in range(50):
        e = f(th)
        d = (f(th + 1e-7) - e) / 1e-7
        th -= e / d
        if abs(e) < 1e-13:
            break
    Ru = pose(th)
    to = lbj + Ru @ (TO - LBJ)
    wc = lbj + Ru @ (WC - LBJ)
    cl = LP + Rl @ (CL - LP)
    axle = Ru @ np.array([1.0, 0, 0])        # ホイールの軸（外向き）
    camber = math.degrees(math.atan2(-axle[1], axle[0]))  # + = 上が外へ（右輪）
    # トー: 前（-Z）が内（-X）を向く = トーイン（+）。軸（外向き）が後ろ（+Z）へ
    # 振れると前が内を向く。
    toe = math.degrees(math.atan2(axle[2], axle[0]))
    ua_ang = math.atan2((ubj - UP)[1], (ubj - UP)[0]) - math.atan2((UBJ - UP)[1], (UBJ - UP)[0])
    return dict(lbj=lbj, ubj=ubj, to=to, wc=wc, cl=cl, Ru=Ru, Rl=Rl,
                Ra=rot_z(ua_ang), camber=camber, toe=toe)


def centers(s):
    """各ボディの重心（ワールド、車体は固定）。"""
    cl = s["cl"]
    d = unit(CU - cl)
    return dict(
        lower_arm=0.5 * (LP + s["lbj"]),
        upper_arm=0.5 * (UP + s["ubj"]),
        upright=0.5 * (s["lbj"] + s["ubj"]),
        wheel=s["wc"],
        tie_rod=0.5 * (TI + s["to"]),
        shock_rod=CU - TUBE_OFF * d,
        shock_body=cl + ROD_OFF * d,
    )


def spring_len(s):
    c = centers(s)
    return np.linalg.norm(c["shock_rod"] - c["shock_body"])


def deriv(fn, h=1e-6):
    return (fn(h) - fn(-h)) / (2 * h)


def design():
    s0 = solve(0.0)
    assert np.allclose(s0["wc"], WC, atol=1e-9) and np.allclose(s0["ubj"], UBJ, atol=1e-9)
    dyw = deriv(lambda p: solve(p)["wc"][1])               # ホイール中心の上下 / phi
    dls = deriv(lambda p: spring_len(solve(p)))            # ばねの長さ / phi
    mr = abs(dls / dyw)                                    # レバー比（ばね / ホイール）
    m_unsprung = sum(v for k, v in M.items() if k != "chassis")
    m_total = sum(M.values())
    # ホイールレートの目標: ばね上の固有振動数（タイヤと直列）
    k_ride = M["chassis"] * (2 * math.pi * TARGET_RIDE_HZ) ** 2
    k_wheel = k_ride * K_TIRE / (K_TIRE - k_ride)
    k_spring = k_wheel / mr ** 2
    c_wheel = DAMPING_RATIO * 2 * math.sqrt(k_wheel * M["chassis"])
    c_damper = c_wheel / mr ** 2
    # 予圧: 仮想仕事 Σ(-m g dy) + Ft dyW + Fs dLs = 0（Fs = 押し広げる向きの力）
    ft = m_total * G
    q_grav = 0.0
    for k in M:
        if k == "chassis":
            continue
        q_grav += -M[k] * G * deriv(lambda p, k=k: centers(solve(p))[k][1])
    fs = -(q_grav + ft * dyw) / dls
    l0 = spring_len(s0)
    rest_spring = l0 + fs / k_spring
    rest_tire = (WC[1] - PAD[1]) + ft / K_TIRE
    return dict(s0=s0, mr=mr, k_wheel=k_wheel, k_spring=k_spring, c_damper=c_damper,
                c_wheel=c_wheel, fs=fs, l0=l0, rest_spring=rest_spring, rest_tire=rest_tire,
                ft=ft, m_unsprung=m_unsprung, m_total=m_total, dyw=dyw)


def sweep_table():
    rows = []
    for travel_mm in (-80, -60, -40, -20, 0, 20, 40, 60, 80):
        # ホイールの上下（バンプ +）から phi を逆算
        target = WC[1] + travel_mm / 1000.0
        p = 0.0
        for _ in range(40):
            e = solve(p)["wc"][1] - target
            p -= e / deriv(lambda q: solve(p + q)["wc"][1])
            if abs(e) < 1e-12:
                break
        s = solve(p)
        h = 1e-5
        mr = abs((spring_len(solve(p + h)) - spring_len(solve(p - h))) /
                 (solve(p + h)["wc"][1] - solve(p - h)["wc"][1]))
        rows.append((travel_mm, math.degrees(p), s["camber"], s["toe"],
                     (s["wc"][0] - WC[0]) * 1000, spring_len(s), mr))
    return rows


def report(d):
    print(f"lever (motion) ratio  {d['mr']:.3f}")
    print(f"wheel rate            {d['k_wheel']:.0f} N/m")
    print(f"spring rate           {d['k_spring']:.0f} N/m")
    print(f"damper                {d['c_damper']:.0f} N s/m  (wheel {d['c_wheel']:.0f})")
    print(f"spring preload        {d['fs']:.0f} N  (length {d['l0']:.4f} -> rest {d['rest_spring']:.4f})")
    print(f"tire preload          {d['ft']:.0f} N  (rest {d['rest_tire']:.4f})")
    print(f"masses                sprung {M['chassis']:.0f} kg, unsprung {d['m_unsprung']:.1f} kg")
    kw, ms, mu = d["k_wheel"], M["chassis"], d["m_unsprung"]
    ride = 1 / (2 * math.pi) * math.sqrt(kw * K_TIRE / (kw + K_TIRE) / ms)
    hop = 1 / (2 * math.pi) * math.sqrt((kw + K_TIRE) / mu)
    print(f"ride freq             {ride:.2f} Hz,  wheel hop {hop:.1f} Hz")
    print(" travel  phi(deg) camber(deg) toe(deg) scrub(mm) spring(m)  MR")
    for r in sweep_table():
        print(" %+5d  %8.3f  %9.3f  %8.3f  %8.2f  %8.4f  %.3f" % r)
    return ride, hop


# ---- XML ----------------------------------------------------------------------
def fmt(*v):
    out = []
    for x in v:
        x = float(x)
        if abs(x) < 5e-7:
            x = 0.0
        s = f"{x:.4f}".rstrip("0").rstrip(".")
        out.append("0" if s in ("-0", "") else s)
    return " ".join(out)


def link_frame(p, q, long_axis=(1.0, 0, 0)):
    """p → q を長手軸（ローカルの long_axis）にした中心と回転。"""
    return 0.5 * (p + q), rot_from_to(np.array(long_axis), q - p), np.linalg.norm(q - p)


def body_frames():
    """各ボディの設計姿勢（中心・回転）。メッシュ（meshes.py）もこの座標系で作る。"""
    I = np.eye(3)
    la_c, la_R, _ = link_frame(LP, LBJ)
    ua_c, ua_R, _ = link_frame(UP, UBJ)
    up_c, up_R, _ = link_frame(LBJ, UBJ, (0, 1.0, 0))
    tr_c, tr_R, _ = link_frame(TI, TO)
    sd = unit(CU - CL)
    st_R = rot_from_to(np.array([0, 1.0, 0]), sd)
    return dict(
        chassis=(CHASSIS, I),
        lower_arm=(la_c, la_R),
        upper_arm=(ua_c, ua_R),
        upright=(up_c, up_R),
        wheel=(WC, I),
        tie_rod=(tr_c, tr_R),
        shock_rod=(CU - TUBE_OFF * sd, st_R),
        shock_body=(CL + ROD_OFF * sd, st_R),
    )


# 部品の色（凡例）。メッシュの材質は meshes.py、ここは文書に直接書く物だけ。
NOCOL = ' layer="1" nocollide="0 1"'
MESH_DIR = "models/double_wishbone"


def build_xml(d, formula_xml):
    s0 = d["s0"]
    L = []
    a = L.append
    I = np.eye(3)

    F = body_frames()
    sd = unit(CU - CL)
    tube_c = F["shock_rod"][0]
    rod_c = F["shock_body"][0]
    # 見た目はボディごとに 1 個の .glb（samples/double_wishbone/meshes.py が作る。
    # 原点と向き = そのボディの座標系なので、部品は pos 0・倍率 1）
    prefab = {name: [f'      <part name="mesh" type="mesh" mesh="dw_{name}" size="1 1 1"/>']
              for name in F}

    ride, hop = report_quiet(d)
    a('<?xml version="1.0" encoding="UTF-8"?>')
    a(f'''<!-- ダブルウィッシュボーン式サスペンションの加振試験（クォーターカー）。
     車体の 1/4（chassis、400 kg）を縦の直動ジョイントで支え、右前輪のサスペンションを
     Chrono の拘束で組んである。リンクの拘束・コイルばね・ダンパ・タイヤばねは
     すべて Chrono が解く（制御器は加振台の動きを作って計測するだけ）。
       * ロアアーム / アッパーアーム … 車体に hinge（前後方向の軸）
       * アップライト（ナックル）    … 上下のアームに ball（ボールジョイント）
       * タイロッド                  … 車体（ステアリングラック）に universal、
                                       ナックルアームに ball（= キングピンまわりを止める）
       * ホイール                    … アップライトに hinge（ハブ、自由に回る）
       * コイルオーバー              … 上端を車体に universal、シリンダとロッドを slide、
                                       下端をロアアームに ball。ばね + ダンパは
                                       spring（ChLinkTSDA、自然長 = 予圧つき）
       * タイヤ                      … ホイールの中心と加振台の間の縦ばね
                                       （spring、K = {K_TIRE / 1000:.0f} kN/m。浮き上がりは
                                       表せない = 引っぱりも出る。scope:tire_N が負なら
                                       実車ではタイヤが浮いている）
       * 加振台 (pad)                … slide の位置モータ。制御器 rig が目標の高さを出す
     拘束の数はちょうど釣り合っている（車体の上下・サスペンションの上下・ホイールの
     回転・加振台の 4 自由度。アームは 1 本の hinge で、A アームの 2 点の取り付けは見た目）。
     部品は形（geom）の無いボディ（質量だけ。見た目は .glb のプレハブ）なので当たらない。
     色の凡例: 車体 = 紺、ロアアーム = 橙、アッパーアーム = 緑、ナックル = 銀、タイロッド = 青、
     コイル = 赤、ダンパ = 黒とクロム、ボールジョイント = 真鍮、ブッシュ = 黒、加振台 = 黄。
     カメラ: cam0 = 内側の斜め前、front = 真正面（キャンバー）、outside = 外側の斜め前。

     ジオメトリ（samples/double_wishbone/gen.py が唯一の定義。数値を変えたらそれで作り直す）:
       レバー比 {d['mr']:.3f}・ホイールレート {d['k_wheel'] / 1000:.1f} kN/m・
       コイル {d['k_spring'] / 1000:.1f} kN/m（予圧 {d['fs']:.0f} N）・ダンパ {d['c_damper']:.0f} N s/m、
       ばね上の固有振動数 {ride:.2f} Hz・ばね下（ホイールホップ）{hop:.1f} Hz・ばね上の減衰比 {DAMPING_RATIO}。
       アッパーアームを短くしてある（SLA）ので、バンプでネガティブキャンバーが付く。
       タイロッドの長さと高さで決まるバンプステア（トーの変化）も出る。

     使い方: シミュレートして、制御盤（Scene タブ「🎛 制御盤（コントローラ）」）の
     「加振のしかた」で 段差 / 正弦掃引 / 正弦 を選ぶ。振幅・周波数はスライダー。
     グラフ: 路面・車体・ホイールの変位、ストローク、キャンバーとトー、ばねとタイヤの力。
     設計姿勢 = 1G の釣り合い（コイルとタイヤの自然長は予圧ぶん長くしてある）ので、
     「静置」ではシミュレートを始めても沈まない。 -->''')
    a('<wizengine model="double_wishbone" version="4">')
    a('  <option gravity="0 -9.81 0" rate="480" substeps="2" iterations="100" envelope="0.002"')
    a('          recovery="2" friction="0.6" restitution="0" lineardamping="0"')
    a('          angulardamping="0" sleeping="false" solver="bb"/>')
    a('  <visual>')
    a('    <quality shadowMap="2048" cascades="3" shadow="pcf" msaa="4" fxaa="true"/>')
    a('    <postprocess ssao="true" bloom="0.02" vignette="0.15"/>')
    a('  </visual>')
    a('  <asset>')
    for line in formula_xml.rstrip().splitlines():
        a("    " + line)
    for name in prefab:
        a(f'    <mesh name="dw_{name}" file="{MESH_DIR}/{name}.glb" scale="1"/>')
    for name, parts in prefab.items():
        a(f'    <prefab name="{name}">')
        L.extend(parts)
        a('    </prefab>')
    a('  </asset>')
    a('  <worldbody>')
    a('    <environment hdr="env/engine_studio.hdr" intensity="22000" skybox="false"/>')
    a('    <ground size="6" visual="4" tile="1" rgba="0.34 0.35 0.37 1" roughness="0.8"/>')
    a('    <light name="key" type="sun" pos="0 5 0" euler="40 150 0" intensity="70000" castshadow="true"/>')
    a('    <light name="fill" type="point" pos="1.6 1.2 -1.4" intensity="30000" falloff="10" castshadow="false"/>')
    a('    <light name="rim" type="point" pos="0.4 1.6 1.2" intensity="20000" falloff="10" castshadow="false"/>')
    # cam0: 車の内側・斜め前から見下ろす（ナックル・両アーム・タイロッド・コイルオーバーが
    # ホイールに隠れない）。front: 真正面（キャンバー）。outside: 外側の斜め前（ホイール側）。
    # 前 = -Z なので方位 180 が真正面、180 より大きいと内側（車の中心側）から。
    a('    <camera name="cam0" target="0.5 0.42 0" azimuth="205" elevation="22" radius="1.9"/>')
    a('    <camera name="front" target="0.5 0.45 0" azimuth="180" elevation="0" radius="1.8"/>')
    a('    <camera name="outside" target="0.5 0.42 0" azimuth="140" elevation="14" radius="1.9"/>')

    def body(name, c, R, geom=None, mass=None, prefab_name=None, fixed=False, comment=None):
        if comment:
            a(f"    <!-- {comment} -->")
        e = euler_deg(R)
        s = f'    <body name="{name}" pos="{fmt(*c)}"'
        if np.max(np.abs(e)) > 1e-4:
            s += f' euler="{fmt(*e)}"'
        if fixed:
            s += ' fixed="true"'
        if geom is None:
            s += f' mass="{mass:g}"'
        a(s + ">")
        if geom:
            a("      " + geom)
        if prefab_name:
            a(f'      <prefab name="{prefab_name}"/>')
        a("    </body>")

    notes = dict(
        chassis="車体の 1/4（ばね上、紺）。縦の直動ジョイントで上下だけ動く",
        lower_arm="ロア A アーム（橙）。車体側は 2 つのブッシュ（黒）、外側はボールジョイント（真鍮）",
        upper_arm="アッパー A アーム（緑）。ロアより短い = SLA",
        upright="ナックル（銀）。スピンドル・ナックルアーム・ブレーキキャリパ（黄）",
        wheel="ホイール + タイヤ + ブレーキディスク（アップライトの hinge で自由に回る）",
        tie_rod="タイロッド（青）。ステアリングは固定 = ラックは車体の一部",
        shock_rod="コイルオーバーの上半分: ピストンロッド（クロム）・アッパーマウント・コイルの上半分（赤）",
        shock_body="コイルオーバーの下半分: ダンパ本体（黒）・下のばね座・コイルの下半分（赤）",
    )
    for name, (c, R) in F.items():
        body(name, c, R, mass=M[name], prefab_name=name, comment=notes[name])
    body("pad", PAD, I,
         f'<geom type="box" size="0.22 0.05 0.22" mass="{M_PAD:g}" rgba="0.85 0.6 0.05 1" roughness="0.5"{NOCOL}/>',
         comment="加振台（黄、位置モータで上下）")
    body("guide", np.array([-0.16, 0.75, 0.0]), I,
         f'<geom type="box" size="0.03 0.75 0.06" mass="10" rgba="0.3 0.31 0.34 1" metallic="1" roughness="0.5"{NOCOL}/>',
         fixed=True, comment="車体の縦ガイド（見た目だけ。拘束は chassis_slide）")
    a('  </worldbody>')

    def v(x):
        return fmt(*x)

    kp_axis = unit(UBJ - LBJ)
    a('  <equality>')
    J = [
        f'<joint name="chassis_slide" type="slide" body1="chassis" body2="world" anchor="{v(CHASSIS)}" axis="0 1 0"/>',
        f'<joint name="lower_pivot" type="hinge" body1="lower_arm" body2="chassis" anchor="{v(LP)}" axis="0 0 1"/>',
        f'<joint name="upper_pivot" type="hinge" body1="upper_arm" body2="chassis" anchor="{v(UP)}" axis="0 0 1"/>',
        f'<joint name="lower_ball" type="ball" body1="upright" body2="lower_arm" anchor="{v(LBJ)}" axis="{v(kp_axis)}"/>',
        f'<joint name="upper_ball" type="ball" body1="upright" body2="upper_arm" anchor="{v(UBJ)}" axis="{v(kp_axis)}"/>',
        f'<joint name="tie_rod_inner" type="universal" body1="tie_rod" body2="chassis" anchor="{v(TI)}" axis="{v(unit(TO - TI))}"/>',
        f'<joint name="tie_rod_outer" type="ball" body1="tie_rod" body2="upright" anchor="{v(TO)}" axis="{v(unit(TO - TI))}"/>',
        f'<joint name="hub" type="hinge" body1="wheel" body2="upright" anchor="{v(WC)}" axis="1 0 0"/>',
        f'<joint name="strut_top" type="universal" body1="shock_rod" body2="chassis" anchor="{v(CU)}" axis="{v(sd)}"/>',
        f'<joint name="strut_slide" type="slide" body1="shock_body" body2="shock_rod" anchor="{v(0.5 * (tube_c + rod_c))}" axis="{v(sd)}"/>',
        f'<joint name="strut_bottom" type="ball" body1="shock_body" body2="lower_arm" anchor="{v(CL)}" axis="{v(sd)}"/>',
        f'<joint name="coil" type="spring" body1="shock_body" body2="shock_rod" anchor="{v(tube_c)}" axis="{v(sd)}"\n'
        f'           distance="{d["rest_spring"]:.5f}" stiffness="{d["k_spring"]:.0f}" damping="{d["c_damper"]:.0f}"/>',
        f'<joint name="tire" type="spring" body1="wheel" body2="pad" anchor="{v(PAD)}" axis="0 1 0"\n'
        f'           distance="{d["rest_tire"]:.5f}" stiffness="{K_TIRE:.0f}" damping="{C_TIRE:.0f}"/>',
        f'<joint name="pad" type="slide" body1="pad" body2="world" anchor="{v(PAD)}" axis="0 1 0"\n'
        f'           motor="position" target="0"/>',
    ]
    for j in J:
        a("    " + j)
    a('  </equality>')
    a('  <events/>')
    a('  <control>')
    a('    <controller formula="rig" period="0"/>')
    a('    <param name="mode" value="1" min="0" max="3" step="1" label="加振のしかた" options="静置|段差|正弦掃引|正弦"/>')
    a('    <param name="amp" value="0.03" min="0" max="0.08" step="0.005" label="振幅 (m)"/>')
    a('    <param name="freq" value="1.5" min="0.2" max="20" step="0.1" label="正弦の周波数 (Hz)"/>')
    a('    <chart title="変位 (mm): 路面・車体・ホイール" signals="rig/scope:road_mm rig/scope:body_mm rig/scope:wheel_mm" span="10"/>')
    a('    <chart title="サスペンションのストローク (mm、+ = バンプ)" signals="rig/scope:travel_mm" span="10"/>')
    a('    <chart title="キャンバー変化とトー変化 (度)" signals="rig/scope:camber_deg rig/scope:toe_deg" span="10"/>')
    a('    <chart title="力 (N): コイル・タイヤ" signals="rig/scope:spring_N rig/scope:tire_N" span="10"/>')
    a('    <chart title="車体の上下加速度 (m/s²)" signals="rig/scope:body_acc" span="10"/>')
    a('    <chart title="掃引の周波数 (Hz)" signals="rig/scope:sweep_Hz" span="0"/>')
    a('  </control>')
    a('</wizengine>')
    return "\n".join(L) + "\n"


def report_quiet(d):
    kw, ms, mu = d["k_wheel"], M["chassis"], d["m_unsprung"]
    ride = 1 / (2 * math.pi) * math.sqrt(kw * K_TIRE / (kw + K_TIRE) / ms)
    hop = 1 / (2 * math.pi) * math.sqrt((kw + K_TIRE) / mu)
    return ride, hop


def rig_text(d):
    with open(os.path.join(HERE, "rig.txt"), encoding="utf-8") as f:
        t = f.read()
    ax = rot_from_to(np.array([0, 1.0, 0]), UBJ - LBJ).T @ np.array([1.0, 0, 0])
    for key, val in zip(("{AX}", "{AY}", "{AZ}"), ax):
        t = t.replace(key, f"{val:.6f}")
    return (t.replace("{K_TIRE}", f"{K_TIRE:.0f}").replace("{C_TIRE}", f"{C_TIRE:.0f}")
             .replace("{REST_TIRE}", f"{d['rest_tire']:.5f}"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--rig-text", help="制御器の数式テキスト（値を埋めたもの）を書き出す先")
    ap.add_argument("--formula-xml", help="エンジンの変換器で作った <formula> 要素")
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()
    d = design()
    if args.report:
        report(d)
    if args.rig_text:
        with open(args.rig_text, "w", encoding="utf-8") as f:
            f.write(rig_text(d))
    if args.formula_xml:
        with open(args.formula_xml, encoding="utf-8") as f:
            fx = f.read()
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(build_xml(d, fx))
        print("wrote", os.path.relpath(args.out))


if __name__ == "__main__":
    main()
