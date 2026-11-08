"""Compare a port render with a part_oracle render of the same patch and note sequence, sample by sample.

Usage: python -I part_compare.py <oracle.wav> <port.wav> [--seg "start:end,..."] [--sr 44100]
Prints, overall and per segment: error RMS relative to the oracle RMS (dB; ~-100 dB or lower = float rounding),
level difference (dB), spectral centroid of both, the band levels (port - oracle, dB) and the first sample whose
difference exceeds 1e-4 of the peak.
"""
import argparse
import struct

import numpy as np


def read_wav(path):
    d = open(path, 'rb').read()
    pos, fmt, data = 12, None, None
    while pos < len(d):
        cid, size = d[pos:pos + 4], struct.unpack_from('<I', d, pos + 4)[0]
        if cid == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', d, pos + 8)
        elif cid == b'data':
            data = d[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    tag, ch, sr, _, _, bits = fmt
    if tag == 3:
        x = np.frombuffer(data, '<f4')
    elif bits == 16:
        x = np.frombuffer(data, '<i2') / 32768.0
    elif bits == 24:
        b = np.frombuffer(data, np.uint8).reshape(-1, 3)
        x = ((b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
             << 8 >> 8) / 8388608.0
    else:
        x = np.frombuffer(data, '<i4') / 2147483648.0
    return x.reshape(-1, ch).astype(np.float64), sr


def db(x):
    return 20 * np.log10(max(x, 1e-30))


BANDS = [(0, 250), (250, 1000), (1000, 4000), (4000, 8000), (8000, 22050)]


def spectrum_stats(x, sr):
    m = x.mean(axis=1)
    if len(m) < 256:
        return 0.0, [0.0] * len(BANDS)
    w = np.hanning(len(m))
    p = np.abs(np.fft.rfft(m * w)) ** 2
    f = np.fft.rfftfreq(len(m), 1 / sr)
    cen = float((p * f).sum() / max(p.sum(), 1e-30))
    bands = [float(p[(f >= a) & (f < b)].sum()) for a, b in BANDS]
    return cen, bands


def report(name, o, p, sr):
    n = min(len(o), len(p))
    o, p = o[:n], p[:n]
    ro, rp = np.sqrt(np.mean(o ** 2)), np.sqrt(np.mean(p ** 2))
    err = np.sqrt(np.mean((o - p) ** 2))
    peak = max(np.abs(o).max(), 1e-30)
    bad = np.nonzero(np.abs(o - p).max(axis=1) > 1e-4 * peak)[0]
    first = int(bad[0]) if len(bad) else -1
    co, bo = spectrum_stats(o, sr)
    cp, bp = spectrum_stats(p, sr)
    bd = ' '.join(f'{db(np.sqrt(b2)) - db(np.sqrt(b1)):+6.1f}' for b1, b2 in zip(bo, bp))
    print(f'{name:>14}  err {db(err) - db(ro):7.1f} dB  level {db(rp) - db(ro):+6.2f} dB  '
          f'centroid o {co:6.0f} p {cp:6.0f} Hz  bands(p-o) {bd}  first diff {first}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('oracle')
    ap.add_argument('port')
    ap.add_argument('--seg', default='', help='"start:end,..." seconds')
    a = ap.parse_args()
    o, sr = read_wav(a.oracle)
    p, sr2 = read_wav(a.port)
    assert sr == sr2
    print(f'bands: {", ".join(f"{a_}-{b_}" for a_, b_ in BANDS)} Hz')
    report('all', o, p, sr)
    for s in filter(None, a.seg.split(',')):
        t0, t1 = (float(v) for v in s.split(':'))
        report(s, o[int(t0 * sr):int(t1 * sr)], p[int(t0 * sr):int(t1 * sr)], sr)


if __name__ == '__main__':
    main()
