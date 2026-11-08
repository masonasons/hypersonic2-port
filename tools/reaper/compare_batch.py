"""Render every reference in a batch folder with hs2render and summarize the A/B comparison.

Usage: python -I compare_batch.py <reference dir> <hs2render.exe> <port output dir>
Per patch: mean level offset (port - ref), spread across test notes, amplitude-envelope correlation,
mean spectral-centroid ratio, and the port's warnings (unported elements/effects).
"""
import glob
import os
import re
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_renders import read_wav, NOTES  # noqa: E402

LEAD = 4.0


def stats(m, rate, t0, t1):
    s = m[int(t0 * rate):int(t1 * rate)]
    rms = 20 * np.log10(max(np.sqrt(np.mean(s ** 2)), 1e-10))
    spec = np.abs(np.fft.rfft(s * np.hanning(len(s))))
    f = np.fft.rfftfreq(len(s), 1 / rate)
    return rms, float((spec * f).sum() / max(spec.sum(), 1e-12))


def main():
    refdir, exe, outdir = sys.argv[1:4]
    os.makedirs(outdir, exist_ok=True)
    rows = []
    for ref in sorted(glob.glob(os.path.join(refdir, 'B*_P*.wav'))):
        m = re.match(r'B(\d+)_P(\d+)\.wav$', os.path.basename(ref))
        if not m:
            continue
        bank, prog = int(m.group(1)), int(m.group(2))
        port = os.path.join(outdir, os.path.basename(ref))
        r = subprocess.run([exe, '--bank', str(bank), '--patch', str(prog), '--out', port], capture_output=True, text=True)
        name = r.stdout.split('\n')[0].split(' / ', 1)[-1].split('  (')[0]
        warns = [w.strip()[9:] for w in r.stdout.split('\n') if 'warning:' in w]
        ra, rr, _ = read_wav(ref)
        pa, pr, _ = read_wav(port)
        rm, pm = ra.mean(axis=1), pa.mean(axis=1)
        diffs, cr = [], []
        for _, s, l in NOTES:
            a, b = stats(rm, rr, LEAD + s, LEAD + s + l), stats(pm, pr, LEAD + s, LEAD + s + l)
            diffs.append(b[0] - a[0])
            cr.append(b[1] / max(a[1], 1))
        rs, ps = rm[int(LEAD * rr):], pm[int(LEAD * pr):]
        n = min(len(rs), len(ps))
        env = lambda x: np.sqrt(np.convolve(x[:n] ** 2, np.ones(441) / 441, 'same'))
        corr = np.corrcoef(env(rs), env(ps))[0, 1]
        rows.append((bank, prog, name, np.mean(diffs), np.std(diffs), corr, np.mean(cr), warns))
    print(f'{"patch":10} {"name":28} {"lvl":>6} {"sprd":>5} {"corr":>6} {"bright":>6}  missing in port')
    for b, p, name, md, sd, c, crat, w in rows:
        miss = '; '.join(sorted(set(re.sub(r"^(element|effect) '[^']*': ", '', x) for x in w)))
        print(f'B{b:03d}_P{p:03d} {name[:28]:28} {md:+6.1f} {sd:5.1f} {c:6.3f} {crat:6.2f}  {miss}')
    good = [r for r in rows if abs(r[3]) < 1.5 and r[4] < 1.5 and r[5] > 0.9]
    print(f'\n{len(good)}/{len(rows)} patches within 1.5 dB mean, 1.5 dB spread, correlation > 0.9')


if __name__ == '__main__':
    main()
