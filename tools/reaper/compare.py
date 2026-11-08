"""Compare a reference render with a port render of the same patch and test sequence.

Usage: python -I compare.py <reference.wav> <port.wav> [--lead 4]
Per note window: RMS level (dB) of each, the level difference, decay (dB drop from the first 100 ms to the last
100 ms of the held note), and spectral centroid (Hz). Repeat renders of the original differ through randomness,
so look for systematic offsets, not nulls.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_renders import read_wav, NOTES  # noqa: E402


def db(x):
    return 20 * np.log10(max(x, 1e-10))


def stats(m, rate, t0, t1):
    s = m[int(t0 * rate):int(t1 * rate)]
    rms = db(np.sqrt(np.mean(s ** 2)))
    head = s[:int(0.1 * rate)]
    tail = s[-int(0.1 * rate):]
    decay = db(np.sqrt(np.mean(head ** 2))) - db(np.sqrt(np.mean(tail ** 2)))
    spec = np.abs(np.fft.rfft(s * np.hanning(len(s))))
    freqs = np.fft.rfftfreq(len(s), 1 / rate)
    cent = float((spec * freqs).sum() / max(spec.sum(), 1e-12))
    return rms, decay, cent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ref')
    ap.add_argument('port')
    ap.add_argument('--lead', type=float, default=4.0)
    a = ap.parse_args()
    ra, rr, _ = read_wav(a.ref)
    pa, pr, _ = read_wav(a.port)
    assert rr == pr, 'sample rates differ'
    rm, pm = ra.mean(axis=1), pa.mean(axis=1)
    print(f'{"note":8} {"ref dB":>7} {"port dB":>8} {"diff":>6} | {"ref decay":>9} {"port decay":>10} | '
          f'{"ref Hz":>7} {"port Hz":>8}')
    diffs = []
    for name, s, l in NOTES:
        t0, t1 = a.lead + s, a.lead + s + l
        r, p = stats(rm, rr, t0, t1), stats(pm, pr, t0, t1)
        diffs.append(p[0] - r[0])
        print(f'{name:8} {r[0]:7.1f} {p[0]:8.1f} {p[0] - r[0]:+6.1f} | {r[1]:9.1f} {p[1]:10.1f} | {r[2]:7.0f} {p[2]:8.0f}')
    print(f'mean level offset (port - ref): {np.mean(diffs):+.1f} dB, spread {np.std(diffs):.1f} dB')
    rs, ps = rm[int(a.lead * rr):], pm[int(a.lead * pr):]
    n = min(len(rs), len(ps))
    env = lambda x: np.sqrt(np.convolve(x[:n] ** 2, np.ones(441) / 441, 'same'))
    c = np.corrcoef(env(rs), env(ps))[0, 1]
    print(f'amplitude-envelope correlation over the whole sequence: {c:.3f}')


if __name__ == '__main__':
    main()
