"""Sanity-check renders from hs2_render.lua.

Usage: python -I check_renders.py <render dir>
Per file: format, peak, RMS (dBFS) for each note window of the test sequence, and flags:
  SILENT  a note window is below -70 dBFS (sample not loaded yet / patch change failed)
  DUP     audio identical (within -80 dB) to another render (program change ignored)
Timing must match hs2_render.lua (LOAD_TIME and SEQ).
"""
import glob
import os
import struct
import sys

import numpy as np

LOAD_TIME = 4.0
NOTES = [('C4', 0.0, 2.0), ('C2', 3.5, 1.0), ('C6', 5.5, 1.0), ('C4v30', 7.5, 1.0), ('C4v127', 9.5, 1.0),
         ('chord', 11.5, 1.5)]


def read_wav(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'RIFF' and d[8:12] == b'WAVE', path
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, size = d[pos:pos + 4], struct.unpack_from('<I', d, pos + 4)[0]
        body = d[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', body)
        elif cid == b'data':
            data = body
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 0xFFFE:  # extensible: real tag in the subformat GUID
        tag = struct.unpack_from('<H', d, d.find(b'fmt ') + 8 + 24)[0]
    if tag == 3 and bits == 32:
        a = np.frombuffer(data, '<f4')
    elif tag == 1 and bits == 16:
        a = np.frombuffer(data, '<i2') / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(data, np.uint8).reshape(-1, 3)
        a = ((b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16)) << 8 >> 8) / 8388608.0
    else:
        raise ValueError(f'{path}: unsupported format tag={tag} bits={bits}')
    return a.reshape(-1, ch).astype(np.float64), rate, f'{"float" if tag == 3 else "pcm"}{bits}/{ch}ch/{rate}'


def db(x):
    return 20 * np.log10(max(x, 1e-10))


def main():
    files = sorted(glob.glob(os.path.join(sys.argv[1], 'B*_P*.wav')))
    if not files:
        sys.exit('no renders found')
    mono = {}
    print(f'{"file":16} {"format":18} {"peak":>6} {"pre":>6} ' + ' '.join(f'{n:>7}' for n, _, _ in NOTES) + '  flags')
    for f in files:
        a, rate, desc = read_wav(f)
        m = a.mean(axis=1)
        mono[f] = m

        def rms(t0, t1):
            s = m[int(t0 * rate):int(t1 * rate)]
            return db(np.sqrt(np.mean(s ** 2))) if len(s) else -200

        pre = rms(0.1, LOAD_TIME - 0.05)
        notes = [rms(LOAD_TIME + s, LOAD_TIME + s + min(l, 1.0)) for _, s, l in NOTES]
        flags = []
        if any(n < -70 for n in notes):
            flags.append('SILENT')
        if pre > -70:
            flags.append(f'PRE-NOISE')
        print(f'{os.path.basename(f)[:-4]:16} {desc:18} {db(np.abs(a).max()):6.1f} {pre:6.1f} '
              + ' '.join(f'{n:7.1f}' for n in notes) + '  ' + ' '.join(flags))
    keys = list(mono)
    for i, k in enumerate(keys):
        for k2 in keys[i + 1:]:
            n = min(len(mono[k]), len(mono[k2]))
            diff = np.sqrt(np.mean((mono[k][:n] - mono[k2][:n]) ** 2))
            same_patch = os.path.basename(k)[:9] == os.path.basename(k2)[:9]
            if db(diff) < -80 and not same_patch:
                print(f'DUP: {os.path.basename(k)} == {os.path.basename(k2)}')
            if same_patch:  # repeated render of one patch: should be identical (or nearly: random/LFO phase)
                print(f'REPEAT {os.path.basename(k)} vs {os.path.basename(k2)}: diff {db(diff):.1f} dBFS')


if __name__ == '__main__':
    main()
