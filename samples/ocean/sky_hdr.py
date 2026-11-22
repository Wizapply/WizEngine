# 海の見本（assets/scenes/ocean.xml）の空の環境マップ（正距円筒、Radiance .hdr）を
# 手続きで作る。上半分 = 晴れた空のグラデーション（天頂は濃い青、地平線は白っぽい
# かすみ）、下半分 = 遠い海の色（水面の範囲の外が空の下半分として見えるので、
# 水面の縁が目立たないように深い水の色に寄せてある）。太陽の円盤は描かない
# （太陽の照り返しはシーンの平行光が出す。両方あると光る点が 2 つになる）。
#   python3 samples/ocean/sky_hdr.py assets/env/ocean_sky.hdr
import math, sys
import numpy as np
W, H = 512, 256
out = sys.argv[1]
v = (np.arange(H) + 0.5) / H          # 0 = 真上
u = (np.arange(W) + 0.5) / W
lat = (0.5 - v) * math.pi             # +90 .. -90
lon = (u - 0.5) * 2 * math.pi
LON, LAT = np.meshgrid(lon, lat)
dy = np.sin(LAT)
up = np.clip(dy, 0, 1)
t = np.power(up, 0.45)
zenith = np.array([0.16, 0.34, 0.78])
horizon = np.array([0.78, 0.86, 0.95])
sky = horizon * (1 - t[..., None]) + zenith * t[..., None]
sea = np.array([0.035, 0.075, 0.11])
down = np.clip(-dy, 0, 1)
below = sea * (1.0 - 0.4 * down[..., None]) + horizon * 0.35 * np.exp(-(down / 0.03) ** 2)[..., None]
img = np.where(dy[..., None] >= 0, sky, below)
# 地平線のかすみ（上下にまたがる柔らかい帯）
img += np.exp(-(dy / 0.05) ** 2)[..., None] * np.array([0.12, 0.12, 0.12])
# RGBE（フラット = 非 RLE）
def to_rgbe(c):
    m = c.max(-1)
    e = np.floor(np.log2(np.maximum(m, 1e-32))) + 1
    sc = np.where(m > 1e-32, 256.0 / np.power(2.0, e), 0)
    rgb = np.clip(c * sc[..., None], 0, 255).astype(np.uint8)
    ex = np.where(m > 1e-32, e + 128, 0).astype(np.uint8)
    return np.concatenate([rgb, ex[..., None]], -1)
data = to_rgbe(img.astype(np.float64))
with open(out, "wb") as f:
    f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n" + f"-Y {H} +X {W}\n".encode())
    f.write(data.tobytes())
print(out, data.nbytes)
