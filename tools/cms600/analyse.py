"""Analyse a duplex loopback WAV: fit 997 Hz and 1499 Hz jointly per 1024-sample block, count glitch blocks."""
import sys
import numpy as np

TONES = (997.0, 1499.0)


def read_float_wav(path):
    raw = open(path, "rb").read()
    nch = int.from_bytes(raw[22:24], "little")
    rate = int.from_bytes(raw[24:28], "little")
    i = raw.index(b"data") + 8
    x = np.frombuffer(raw[i:], dtype="<f4")
    x = x[: len(x) - len(x) % nch].reshape(-1, nch)
    return rate, x


def db(v):
    return 20 * np.log10(max(v, 1e-12))


def analyse(path):
    rate, x = read_float_wav(path)
    skip = int(rate * 0.5)
    x = x[skip:]
    print(f"{path}: {rate} Hz, {x.shape[1]} ch, {len(x)/rate:.1f} s analysed")
    blk = 1024
    for c in range(x.shape[1]):
        s = x[:, c].astype(np.float64)
        rms = np.sqrt(np.mean(s ** 2))
        line = f"  in{c+1}: rms {db(rms):6.1f} dBFS"
        if rms < 10 ** (-60 / 20):
            print(line + "  (silent)")
            continue
        bad, worst, amps = [], -200.0, None
        for b0 in range(0, len(s) - blk, blk):
            t = (np.arange(blk) + b0) / rate
            cols = [np.ones(blk)]
            for f in TONES:
                cols += [np.sin(2 * np.pi * f * t), np.cos(2 * np.pi * f * t)]
            A = np.column_stack(cols)
            seg = s[b0:b0 + blk]
            coef, *_ = np.linalg.lstsq(A, seg, rcond=None)
            res = seg - A @ coef
            a = [np.hypot(coef[1 + 2 * k], coef[2 + 2 * k]) for k in range(len(TONES))]
            if amps is None:
                amps = a
            sig = np.sqrt(np.mean((A @ coef - coef[0]) ** 2))
            r = db(np.max(np.abs(res)) / max(sig, 1e-12))
            worst = max(worst, r)
            if r > -20:
                bad.append(round((b0 + skip) / rate, 3))
        tones = ", ".join(f"{f:.0f} Hz {db(a / np.sqrt(2)):6.1f}" for f, a in zip(TONES, amps))
        line += f"  [{tones}]  worst block peak-residual {worst:6.1f} dB, glitch blocks {len(bad)}"
        if bad:
            line += f" at {bad[:10]}{'...' if len(bad) > 10 else ''}"
        print(line)


for p in sys.argv[1:]:
    analyse(p)
