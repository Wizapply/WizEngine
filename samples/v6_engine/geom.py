# VR30DDTT 風 V6 の幾何（シーン生成とオフライン検証で共有）
import math
R_CRANK = 0.043      # クランク半径 = ストローク 86 mm / 2
L_ROD = 0.144        # コンロッド長（仮定: VQ 系と同程度）
CY = 0.55            # クランク中心の高さ
BANK = 30.0          # バンク半角（60° V）
# 気筒: (番号, バンク角 alpha[deg], クランクピン角 beta[deg] (θ=0), z, 点火 TDC [deg in 720])
# 右バンク 1,3,5 は alpha=-30（+X 側）、左バンク 2,4,6 は +30。点火順 1-2-3-4-5-6、120° 等間隔。
CYL = []
FIRE = [0, 120, 240, 360, 480, 600]
PAIR_Z = [-0.108, 0.0, 0.108]
for i in range(6):
    alpha = -BANK if i % 2 == 0 else BANK
    tdc = FIRE[i] % 360
    beta = (alpha - tdc) % 360
    if beta > 180: beta -= 360
    z = PAIR_Z[i // 2] + (-0.0125 if i % 2 == 0 else 0.0125)
    CYL.append(dict(n=i + 1, alpha=alpha, beta=beta, z=z, fire=FIRE[i]))
def u(deg):
    a = math.radians(deg)
    return (-math.sin(a), math.cos(a))
def piston_dist(phi_deg):
    p = math.radians(phi_deg)
    return R_CRANK * math.cos(p) + math.sqrt(L_ROD**2 - (R_CRANK * math.sin(p))**2)

# ---- 動弁系（DOHC 4 バルブ）。数値は仮定（資料に無い）
DUR = 240.0          # 作用角（クランク角, deg）
IN_CENTER = -250.0   # 吸気の中心 = 点火 TDC から（= 排気上死点 -360 の 110° 後）
EX_CENTER = 250.0    # 排気の中心 = 排気上死点 +360 の 110° 前
LIFT_IN = 0.010
LIFT_EX = 0.0095
CAM_D = 0.355        # カム軸のシリンダ軸方向の距離（クランク中心から）
VALVE_D = 0.285      # バルブ（ステムの中心）の距離
LAT = 0.019          # 吸気 / 排気の横のずれ（吸気 = V の内側）
VZ = 0.017           # 1 気筒 2 本のバルブの z のずれ
BANKS = {"R": -BANK, "L": BANK}   # 右バンク = 1,3,5、左 = 2,4,6
BANK_Z = {"R": -0.0125, "L": 0.0125}
def outward(alpha):
    a = u(alpha)
    # シリンダ軸に垂直で V の外側を向く単位ベクトル
    n = (a[1], -a[0]) if alpha < 0 else (-a[1], a[0])
    return n
def bank_of(c): return "R" if c["alpha"] < 0 else "L"
def wrap360(x):
    return (x + 180.0) % 360.0 - 180.0

# ---- 実寸の動弁系（v3）。バルブは挟み角 32°（片側 16°）で傾け、カムはその延長上。
# 式（valvetrain / combustion）はカムとバルブのジョイントの角度・位置しか見ないので、
# ここを変えても計算は変わらない（見た目と拘束の向きだけ）。
DECK = 0.235                 # デッキ面（シリンダヘッドの合わせ面）のクランク中心からの距離
BORE_R = 0.043
THETA_V = 16.0               # バルブの傾き（シリンダ軸から）
SEAT_L = 0.0185              # バルブシートの中心の横のずれ
VZ = 0.0185                  # 1 気筒 2 本のバルブの z のずれ（上の VZ を上書き）
ROOF_TAN = math.tan(math.radians(THETA_V))
SEAT_S = DECK + (BORE_R - SEAT_L) * ROOF_TAN   # ペントルーフの上のシート面
VALVE_LEN = 0.098            # 傘の面 → バケット上面
RB = 0.018                   # カムのベース円の半径
CAM_DIST = VALVE_LEN + RB    # シート → カム軸
HEAD_IN = 0.032              # 吸気バルブの傘径
HEAD_EX = 0.027
CHAIN_Z_BANK = {"R": -0.190, "L": -0.203}   # バンクごとのチェーンの面
def valve_sign(kind):        # 吸気 = V の内側（-）、排気 = 外側（+）
    return -1.0 if kind in ("in", "i") else 1.0
def valve_dir(alpha, kind):
    a = u(alpha); n = outward(alpha); s = valve_sign(kind)
    c, sn = math.cos(math.radians(THETA_V)), math.sin(math.radians(THETA_V))
    return (c * a[0] + s * sn * n[0], c * a[1] + s * sn * n[1])
def valve_seat(alpha, kind):
    a = u(alpha); n = outward(alpha); s = valve_sign(kind)
    return (SEAT_S * a[0] + s * SEAT_L * n[0], SEAT_S * a[1] + s * SEAT_L * n[1])
def cam_center(alpha, kind):
    p = valve_seat(alpha, kind); d = valve_dir(alpha, kind)
    return (p[0] + CAM_DIST * d[0], p[1] + CAM_DIST * d[1])
def dir_deg(d):              # u(deg) = d となる角（euler z）
    return math.degrees(math.atan2(-d[0], d[1]))
def cam_lift(dpsi_rad, kind):  # カム角（ローブ中心から）→ リフト（valvetrain と同じ式）
    q = math.radians(60.0); L = LIFT_IN if kind in ("in", "i") else LIFT_EX
    return L * 0.5 * (1 + math.cos(math.pi * dpsi_rad / q)) if abs(dpsi_rad) < q else 0.0
