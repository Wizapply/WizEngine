"""オーディオの見本クリップ（assets/audio/*.wav）を手続きで作る。

エンジンのビルド・実行には使わない（作った .wav はリポジトリに入れてある）。
音を変えたいときはここを直して作り直す:

    python3 samples/audio/gen_clips.py            # assets/audio/ へ書く
    python3 samples/audio/gen_clips.py --out DIR  # 別の場所へ

標準ライブラリだけ（wave + math + random）。どれもモノラル 22050 Hz の 16 bit PCM
（3D の音源は空間化でモノラルに畳まれるので、ステレオにしても意味が無い）。

  bgm_loop.wav     … 8 秒で継ぎ目なくループする柔らかいアルペジオ（2D の BGM 用）
  engine_loop.wav  … 1 秒のエンジンのうなり（基本 30 Hz の倍音 + 少しのノイズ。
                     ループの継ぎ目が合うよう周期の整数倍の長さ。pitch で回転数を表す）
  impact.wav       … 0.35 秒の「ゴッ」という衝突音（減衰する低音 + ノイズの立ち上がり）
  chime.wav        … 1.2 秒のベル（非整数倍音の減衰。イベントの合図用）
  beep.wav         … 0.15 秒の短いビープ（UI 音）
  combustion.wav   … 0.12 秒の燃焼 1 回ぶんの音（重低音寄り: 100 → 45 Hz へ下がるサブ低音の
                     「ドン」+ その倍音とブロックの鳴り + 排気の噴き出しのノイズ + 短い打音）。V6 の見本
                     （v6_engine.xml）が気筒ごとに点火のたびに一発鳴らす。続けて鳴らすと
                     重なりがそのまま点火の周波数のエンジン音になる（ループの音を
                     pitch で回すのではなく、音の頭の並びが回転数を表す）
"""

import argparse
import math
import os
import random
import struct
import wave

RATE = 22050


def write_wav(path, samples):
    peak = max(1e-9, max(abs(s) for s in samples))
    gain = 0.9 / peak if peak > 0.9 else 1.0
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * gain)) * 32767))
                               for s in samples))
    print("wrote", path, "%.2f s" % (len(samples) / RATE))


def envelope(t, attack, release, length):
    a = min(1.0, t / attack) if attack > 0 else 1.0
    r = min(1.0, (length - t) / release) if release > 0 else 1.0
    return max(0.0, min(a, r))


def bgm_loop():
    # 8 秒 = 4 小節（120 BPM の 4 拍子 × 2 秒）。Am - F - C - G のアルペジオと薄いパッド。
    length = 8.0
    n = int(RATE * length)
    out = [0.0] * n
    chords = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]
    midi = lambda m: 440.0 * 2 ** ((m - 69) / 12.0)
    step = 0.25  # 16 分音符
    for bar, chord in enumerate(chords):
        t0 = bar * 2.0
        # パッド（小節いっぱい、両端をなだらかに）
        for note in chord:
            f = midi(note)
            for i in range(int(t0 * RATE), int((t0 + 2.0) * RATE)):
                t = i / RATE - t0
                e = envelope(t, 0.25, 0.25, 2.0) * 0.05
                out[i] += e * (math.sin(2 * math.pi * f * i / RATE) +
                               0.3 * math.sin(2 * math.pi * 2 * f * i / RATE))
        # アルペジオ（上がって下がる）
        seq = [chord[0] + 12, chord[1] + 12, chord[2] + 12, chord[1] + 24,
               chord[2] + 12, chord[1] + 12, chord[0] + 12, chord[2]]
        for k in range(8):
            f = midi(seq[k])
            start = int((t0 + k * step) * RATE)
            dur = int(0.45 * RATE)
            for j in range(dur):
                i = start + j
                if i >= n:
                    i -= n  # 最後の音の尾は頭へ回す = 継ぎ目なくループ
                t = j / RATE
                e = math.exp(-t * 7.0) * min(1.0, t / 0.005) * 0.18
                out[i] += e * (math.sin(2 * math.pi * f * t) +
                               0.25 * math.sin(2 * math.pi * 3 * f * t))
    return out


def engine_loop():
    # 基本 30 Hz（4 気筒の 900 rpm 相当の燃焼の周期）。長さは周期の整数倍 = ループの継ぎ目が合う。
    f0 = 30.0
    n = int(RATE / f0) * 30  # 30 周期 ≒ 1 秒
    rnd = random.Random(7)
    noise = [rnd.uniform(-1, 1) for _ in range(n)]
    # ノイズは周期ごとに同じ形を繰り返す（継ぎ目が合う）+ 軽くならす
    period = n // 30
    smooth = [0.0] * n
    acc = 0.0
    for i in range(n):
        acc += (noise[i % period] - acc) * 0.2
        smooth[i] = acc
    out = []
    for i in range(n):
        t = i / RATE
        s = 0.0
        for h, a in ((1, 1.0), (2, 0.6), (3, 0.45), (4, 0.3), (6, 0.15), (8, 0.08)):
            s += a * math.sin(2 * math.pi * f0 * h * t + h * 0.7)
        # 燃焼のパルス（周期ごとの立ち上がり）
        ph = (t * f0) % 1.0
        s += 0.8 * math.exp(-ph * 9.0) * smooth[i]
        out.append(s * 0.25)
    return out


def impact():
    length = 0.35
    n = int(RATE * length)
    rnd = random.Random(3)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / RATE
        body = math.exp(-t * 14.0) * (math.sin(2 * math.pi * 70 * t) +
                                      0.5 * math.sin(2 * math.pi * 113 * t + 1.0))
        lp += (rnd.uniform(-1, 1) - lp) * 0.35
        click = math.exp(-t * 60.0) * lp * 1.6
        out.append((body + click) * envelope(t, 0.001, 0.02, length))
    return out


def chime():
    length = 1.2
    n = int(RATE * length)
    f = 880.0
    partials = ((1.0, 1.0, 3.0), (2.76, 0.5, 5.0), (5.40, 0.25, 8.0), (8.93, 0.12, 12.0))
    out = []
    for i in range(n):
        t = i / RATE
        s = sum(a * math.exp(-t * d) * math.sin(2 * math.pi * f * r * t) for r, a, d in partials)
        out.append(s * 0.4 * envelope(t, 0.002, 0.05, length))
    return out


def beep():
    length = 0.15
    n = int(RATE * length)
    return [0.35 * math.sin(2 * math.pi * 1320 * i / RATE) * envelope(i / RATE, 0.005, 0.03, length)
            for i in range(n)]


def combustion():
    # 1 回の燃焼（重低音寄り）。キックドラムと同じ作りで、主役は「高めから低く落ちる」
    # サブ低音（100 → 45 Hz の下降、時定数 35 ms）= 腹に来る「ドン」。続けて鳴らすと音が
    # 重なる（3000 rpm で 1 回の長さより点火の間隔が短い）ので、低音だけだと互いに打ち
    # 消し合って中高回転で痩せる。そこでサブ低音の 2 倍音とブロックの鳴り（120 Hz）を
    # 強めに入れて低い倍音の芯を作り（小さなスピーカーでも低さが伝わる = ミッシング
    # ファンダメンタル）、頭の輪郭は排気の噴き出し（低めに丸めたノイズ）・中域のノイズ・
    # 打音を短く足して出す（無いとこもる）。
    # 長さ 0.12 秒: 6800 rpm では 1 気筒あたり 17.6 ms ごとに鳴るので約 7 声が重なる
    # （ミキサーは 1 音源 8 声まで。溢れたら古い = いちばん小さくなった尾から譲る）。
    length = 0.12
    n = int(RATE * length)
    rnd = random.Random(11)
    out = []
    lp = lp2 = lp3 = 0.0   # 排気のノイズ（3 段の一次遅れで低めに丸める）
    lm = 0.0               # 中域のノイズ（1 段だけ丸める）
    hp_prev = 0.0
    phase = 0.0
    f_hi, f_lo, sweep = 100.0, 45.0, 0.025   # サブ低音の下降（Hz、時定数 s）
    for i in range(n):
        t = i / RATE
        # サブ低音: 周波数を指数で下げ、位相を積分する（下降しても途切れない）+ 2 倍音
        f = f_lo + (f_hi - f_lo) * math.exp(-t / sweep)
        phase += 2 * math.pi * f / RATE
        sub = math.exp(-t / 0.035) * (math.sin(phase) + 0.8 * math.sin(2 * phase + 0.4))
        # ブロックの鳴り（低い倍音の芯）
        body = math.exp(-t / 0.016) * 1.1 * math.sin(2 * math.pi * 120 * t + 0.6)
        # 排気の噴き出し: 低く丸めたノイズが速く減衰
        w = rnd.uniform(-1, 1)
        lp += (w - lp) * 0.12
        lp2 += (lp - lp2) * 0.18
        lp3 += (lp2 - lp3) * 0.25
        bark = math.exp(-t / 0.008) * lp3 * 5.0
        # 打音（頭の「カッ」）: 高い成分をごく短く
        hp = w - hp_prev
        hp_prev = w
        knock = math.exp(-t / 0.0015) * hp * 0.5
        # 中域のノイズ（輪郭）: 短く
        lm += (w - lm) * 0.3
        mid = math.exp(-t / 0.005) * lm * 1.0
        # いちばん低いうなりの尾（30 Hz 台。次の燃焼へつながる）
        tail = math.exp(-t / 0.06) * 0.12 * math.sin(2 * math.pi * 33 * t + 1.2)
        out.append((sub + body + bark + knock + mid + tail) * envelope(t, 0.0008, 0.025, length))
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(here, "..", "..", "assets", "audio"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for name, fn in (("bgm_loop", bgm_loop), ("engine_loop", engine_loop), ("impact", impact),
                     ("chime", chime), ("beep", beep), ("combustion", combustion)):
        write_wav(os.path.join(args.out, name + ".wav"), fn())


if __name__ == "__main__":
    main()
