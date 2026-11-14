# 撮影スタジオ風の環境マップ（正距円筒、Radiance .hdr）を手続きで作る。
# 上半分 = 明るめのグラデーション + 大きなソフトボックス、下半分 = 暗い床。
import math, sys
import numpy as np
W, H = 512, 256
out = sys.argv[1]
v = (np.arange(H) + 0.5) / H          # 0 = 真上
u = (np.arange(W) + 0.5) / W
lat = (0.5 - v) * math.pi             # +90 .. -90
lon = (u - 0.5) * 2 * math.pi
LON, LAT = np.meshgrid(lon, lat)
dx = np.cos(LAT) * np.sin(LON); dy = np.sin(LAT); dz = np.cos(LAT) * np.cos(LON)
img = np.zeros((H, W, 3))
up = np.clip(dy, 0, 1)
sky = np.stack([0.55 + 0.25 * up, 0.58 + 0.27 * up, 0.62 + 0.30 * up], -1)
floor = np.stack([0.10 + 0.05 * (1 + dy), 0.10 + 0.05 * (1 + dy), 0.11 + 0.05 * (1 + dy)], -1)
img = np.where(dy[..., None] >= 0, sky, floor)
# 地平線の柔らかい帯（壁）
band = np.exp(-(dy / 0.08) ** 2)
img += band[..., None] * np.array([0.25, 0.25, 0.26])
def softbox(cx, cy, cz, half_w, half_h, power):
    c = np.array([cx, cy, cz]); c /= np.linalg.norm(c)
    t = np.cross(c, [0, 1, 0]); 
    if np.linalg.norm(t) < 1e-6: t = np.array([1.0, 0, 0])
    t /= np.linalg.norm(t); b = np.cross(t, c)
    d = np.stack([dx, dy, dz], -1)
    dot = d @ c
    px = (d @ t) / np.maximum(dot, 1e-6); py = (d @ b) / np.maximum(dot, 1e-6)
    m = (dot > 0) & (np.abs(px) < half_w) & (np.abs(py) < half_h)
    edge = np.clip(1 - np.maximum(np.abs(px) / half_w, np.abs(py) / half_h), 0, 1)
    return np.where(m, power * np.clip(edge * 6, 0, 1), 0.0)
L = softbox(-0.6, 0.8, 0.5, 0.45, 0.25, 9.0) + softbox(0.8, 0.6, -0.4, 0.35, 0.35, 6.0) + softbox(0.0, 1.0, 0.0, 0.5, 0.5, 4.0) + softbox(-0.3, 0.35, -1.0, 0.6, 0.12, 5.0)
img += L[..., None] * np.array([1.0, 0.98, 0.95])
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
