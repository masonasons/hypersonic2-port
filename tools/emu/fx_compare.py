"""Compare an effect port against the emulated original on several parameter sets.

Usage: python -I fx_compare.py <Hypersonic.dll> <fxtest.exe> <type> [--sets "p0,p1,..;p0,p1,.."] [--workdir DIR]
For each set: runs fx_oracle (original, emulated) and fxtest (port) on the same input and reports the error
relative to the reference RMS (dB). Below about -100 dB is float rounding; -60 dB or worse is a real difference.
"""
import argparse
import os
import random
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fx_oracle  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dll')
    ap.add_argument('fxtest')
    ap.add_argument('type', type=int)
    ap.add_argument('--sets', default='')
    ap.add_argument('--random', type=int, default=4, help='extra random parameter sets')
    ap.add_argument('--workdir', default='.')
    ap.add_argument('--seconds', type=float, default=2.0)
    ap.add_argument('--pitch', action='store_true', help='connect a note-pitch source (vtable +8) on both sides')
    ap.add_argument('--tempo', type=float, default=None, help='call the tempo method (vtable +0xc) with this bpm')
    ap.add_argument('--gated', action='store_true', help='silence-flag protocol (input with long silences)')
    ap.add_argument('--seed', type=int, default=None, help='random-set seed (default: the type id)')
    a = ap.parse_args()
    sets = [[float(v) for v in s.split(',')] for s in a.sets.split(';') if s]
    rnd = random.Random(a.type if a.seed is None else a.seed)
    sets += [[round(rnd.random(), 3) for _ in range(12)] for _ in range(a.random)]
    x = fx_oracle.gate_signal(44100) if a.gated else fx_oracle.test_signal('mix', 44100, a.seconds)
    inp = os.path.join(a.workdir, f'fx{a.type}.in.f32')
    x.astype('<f4').tofile(inp)
    worst = -999
    for ps in sets:
        pitch = fx_oracle.default_pitch(44100) if a.pitch else None
        rflags = []
        ref = fx_oracle.run_effect(a.dll, a.type, ps, 44100, x, pitch, a.tempo, a.gated, rflags)
        outp = os.path.join(a.workdir, f'fx{a.type}.port.f32')
        subprocess.run([a.fxtest, str(a.type), inp, outp, '--params', ','.join(map(str, ps))]
                       + (['--pitch', ';'.join(f'{f}:{v!r}' for f, v in pitch)] if pitch else [])
                       + (['--tempo', str(a.tempo)] if a.tempo else [])
                       + (['--gated'] if a.gated else []), check=True)
        got = np.fromfile(outp, '<f4').reshape(-1, 2)
        err = np.sqrt(np.mean((got - ref) ** 2)) / max(np.sqrt(np.mean(ref ** 2)), 1e-12)
        e_db = 20 * np.log10(max(err, 1e-12))
        first = int(np.argmax(np.abs(got - ref).max(axis=1) > 1e-4 * max(np.abs(ref).max(), 1e-9)))
        fl = ''
        if a.gated:
            pflags = np.fromfile(outp + '.flags', np.uint8)
            bad = int(np.sum(pflags[:len(rflags)] != np.array(rflags, np.uint8)))
            fl = f'  flags {bad} mismatches/{len(rflags)}'
            if bad:
                e_db = max(e_db, 0.0)  # a flag mismatch is a failure even when the audio happens to agree
        print(f'params {ps}: error {e_db:7.1f} dB  (first divergence at frame {first}){fl}')
        worst = max(worst, e_db)
    print(f'worst {worst:.1f} dB')


if __name__ == '__main__':
    main()
