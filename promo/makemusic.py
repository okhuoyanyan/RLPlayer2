# -*- coding: utf-8 -*-
# makemusic.py — 离线合成 150 秒 ambient 床乐（仅写 WAV 文件，绝不打开音频设备）
# 结构：与分镜同步的 8 段和弦 pad + 场景切换噪声涌动 + 柔和低频脉冲 + 首尾淡化
import math, struct, wave, random

SR = 22050
DUR = 150.0
N = int(SR * DUR)
TAU = math.pi * 2

# 8 段与分镜边界一致：S0..S7
SEGS = [
    (0.0, 11.0, 'am'), (11.0, 29.0, 'fmaj'), (29.0, 50.0, 'cmaj'),
    (50.0, 76.0, 'gsus'), (76.0, 95.0, 'em'), (95.0, 115.0, 'dm'),
    (115.0, 133.0, 'cmaj2'), (133.0, 150.0, 'am2'),
]
CHORDS = {
    'am':   [110.00, 164.81, 220.00, 246.94, 329.63],
    'fmaj': [87.31, 130.81, 174.61, 220.00, 329.63],
    'cmaj': [130.81, 164.81, 196.00, 246.94, 293.66],
    'gsus': [98.00, 146.83, 196.00, 246.94, 293.66],
    'em':   [82.41, 123.47, 164.81, 196.00, 246.94],
    'dm':   [73.42, 110.00, 146.83, 174.61, 220.00],
    'cmaj2':[130.81, 164.81, 196.00, 261.63, 293.66],
    'am2':  [110.00, 164.81, 220.00, 329.63, 493.88],
}
BOUND = [11.0, 29.0, 50.0, 76.0, 95.0, 115.0, 133.0]

def smooth(x):
    x = 0.0 if x < 0 else (1.0 if x > 1 else x)
    return x * x * (3 - 2 * x)

bufL = [0.0] * N
bufR = [0.0] * N

# ---- 和弦 pad（每音左右轻微失谐展开声像，段间交叉淡化）----
for (t0, t1, name) in SEGS:
    freqs = CHORDS[name]
    for fi, f in enumerate(freqs):
        amp = 0.16 / (1 + fi * 0.35)
        for det, aL, aR, ph0 in [(+0.0013, 1.0, 0.52, 0.0), (-0.0013, 0.52, 1.0, 1.7)]:
            step = TAU * f * (1 + det) / SR
            ph = ph0
            s0, s1 = int(t0 * SR), int(min(t1 + 2.8, DUR) * SR)
            for i in range(s0, s1):
                tt = i / SR
                env = smooth((tt - t0) / 2.2) * (1 - smooth((tt - t1) / 2.8) if tt > t1 else 1.0)
                if env <= 0:
                    ph += step
                    continue
                shimmer = 0.85 + 0.15 * math.sin(TAU * tt * 0.11 + fi)
                v = math.sin(ph) * env * amp * shimmer
                bufL[i] += v * aL
                bufR[i] += v * aR
                ph += step
    # 低八度根音（居中）
    root = freqs[0] / 2
    step = TAU * root / SR
    ph = 0.0
    s0 = int(t0 * SR)
    s1 = int(min(t1 + 2.8, DUR) * SR)
    for i in range(s0, s1):
        tt = i / SR
        env = smooth((tt - t0) / 2.6) * (1 - smooth((tt - t1) / 2.8) if tt > t1 else 1.0)
        if env > 0:
            v = math.sin(ph) * env * 0.10
            bufL[i] += v
            bufR[i] += v
        ph += step

# ---- 空气感：低通白噪声 + 慢 LFO ----
random.seed(7)
lp = 0.0
for i in range(N):
    lp += 0.02 * ((random.random() * 2 - 1) - lp)
    v = lp * 0.05 * (0.7 + 0.3 * math.sin(TAU * i / SR / 17.0))
    bufL[i] += v
    bufR[i] += v

# ---- 场景切换涌动（每个分镜点 ±0.8s 高斯窗噪声）----
random.seed(21)
for b in BOUND:
    s0 = max(0, int((b - 0.9) * SR))
    s1 = min(N, int((b + 0.9) * SR))
    lp = 0.0
    for i in range(s0, s1):
        tt = i / SR
        g = math.exp(-((tt - b) / 0.34) ** 2)
        if g < 0.01:
            continue
        lp += 0.05 * ((random.random() * 2 - 1) - lp)
        v = lp * g * 0.16
        bufL[i] += v
        bufR[i] += v

# ---- 柔和低频脉冲（29s 起，76–115s 最强，141s 后停）----
BEAT = 60.0 / 72.0
def kick_amp(tt):
    if tt < 29.0: return 0.0
    if tt < 50.0: return 0.045
    if tt < 115.0: return 0.09
    if tt < 133.0: return 0.05
    if tt < 141.0: return 0.028
    return 0.0
k = int(29.0 / BEAT)
while True:
    tb = k * BEAT
    if tb >= 141.0:
        break
    ka = kick_amp(tb + 0.05)
    if ka > 0:
        s0 = int(tb * SR)
        dur = int(0.30 * SR)
        ph = 0.0
        for j in range(dur):
            i = s0 + j
            if i >= N:
                break
            tl = j / SR
            f = 50.0 + 35.0 * math.exp(-tl * 18.0)
            ph += TAU * f / SR
            env = math.exp(-tl * 11.0)
            v = math.sin(ph) * env * ka
            bufL[i] += v
            bufR[i] += v
    k += 1

# ---- 首尾淡化 + 归一化 ----
fade_in = int(1.4 * SR)
for i in range(fade_in):
    g = smooth(i / fade_in)
    bufL[i] *= g
    bufR[i] *= g
fade_out = int(3.2 * SR)
for j in range(fade_out):
    i = N - 1 - j
    g = smooth(j / fade_out)
    bufL[i] *= (1 - g)
    bufR[i] *= (1 - g)

peak = max(max(abs(x) for x in bufL), max(abs(x) for x in bufR))
scale = 0.45 / peak
print('peak before norm: %.4f, scale %.4f' % (peak, scale))

with wave.open('music.wav', 'wb') as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    frames = bytearray()
    for i in range(N):
        l = max(-1, min(1, bufL[i] * scale))
        r = max(-1, min(1, bufR[i] * scale))
        frames += struct.pack('<hh', int(l * 32767), int(r * 32767))
    w.writeframes(bytes(frames))
print('music.wav written: %d frames, %.1fs' % (N, N / SR))
