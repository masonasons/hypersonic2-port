"""Run an ORIGINAL Hypersonic 2 effect (emulated) on a test signal: the reference for porting effects.

Usage: python -I fx_oracle.py <Hypersonic.dll> <type> <out.f32> [--params p0,p1,...] [--sr 44100] [--seconds 2]
       [--signal impulse|noise|mix]
Writes raw little-endian float32 stereo interleaved (input signal file <out>.in.f32 alongside).
The C++ side (tools/cli/fxtest) reads the same input and writes its output for comparison (tools/emu/fx_compare.py).
"""
import argparse
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hs2emu import Emu  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, '..', '..', 'docs', 'engine', 'effects_table.json')
BLOCK = 32
LN2_12 = 0.057762265  # ln(2)/12, the part's note-pitch scale


def default_pitch(sr):
    """Note-pitch timeline [(frame, value)]: what part+0x968 holds (newest held note * ln2/12; -10 = none held)."""
    # starts with a note held: Tuned Resonators (tracking on) loops forever if processed before any note (scale 0)
    ev = [(0.0, 60), (0.5, 67), (1.0, None), (1.2, 48), (1.7, None)]
    return [(int(t * sr), -10.0 if n is None else float(n * LN2_12)) for t, n in ev]


def test_signal(kind, sr, seconds, seed=1):
    n = int(sr * seconds)
    x = np.zeros((n, 2), np.float32)
    rng = np.random.default_rng(seed)
    if kind in ('impulse', 'mix'):
        x[int(0.01 * sr)] = 0.8
    if kind in ('noise', 'mix'):
        burst = int(0.25 * sr)
        x[int(0.3 * sr):int(0.3 * sr) + burst] += (rng.standard_normal((burst, 2)) * 0.2).astype(np.float32)
    if kind == 'mix':
        t = np.arange(n) / sr
        sweep = 0.3 * np.sin(2 * np.pi * (50 * t + (8000 - 50) / (2 * seconds) * t * t))
        start = int(0.8 * sr)
        x[start:, 0] += sweep[start:].astype(np.float32)
        x[start:, 1] += 0.5 * sweep[start:].astype(np.float32)
    return x


def gate_signal(sr):
    """Bursts separated by silences longer than the default effect tail (200000 samples), for the gated protocol."""
    n = int(sr * 7.0)
    x = np.zeros((n, 2), np.float32)
    rng = np.random.default_rng(3)
    a, b = int(0.05 * sr), int(0.35 * sr)
    x[a:b] = (rng.standard_normal((b - a, 2)) * 0.2).astype(np.float32)
    x[int(5.3 * sr)] = 0.8
    c, d = int(5.6 * sr), int(5.8 * sr)
    t = np.arange(d - c) / sr
    x[c:d, 0] = (0.3 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    x[c:d, 1] = x[c:d, 0] * 0.5
    return x


def block_silent(blk):
    return 1 if not np.any(blk) else 0


def run_effect(dll, ftype, params, sr, x, pitch=None, tempo=None, gated=False, flags_out=None):
    table = {e['id']: e for e in json.load(open(TABLE))}
    e = table[ftype]
    emu = Emu(dll)
    obj = emu.alloc(0x400)
    emu.call(int(e['ctor'], 16), (), ecx=obj)
    vt = emu.u32(obj)
    method = lambda off: emu.u32(vt + off)
    if tempo is not None:  # element order: vtable +0xc (bpm, beats per sample), then +8, then setSampleRate
        emu.call(method(0x0c), (float(tempo), float(tempo / 60.0 / sr)), ecx=obj)
    pitch_ptr = None
    if pitch is not None:  # the element connects the pitch source (vtable +8) before setSampleRate
        pitch_ptr = emu.alloc(4)
        emu.uc.mem_write(pitch_ptr, struct.pack('<f', -10.0))
        emu.call(method(0x08), (pitch_ptr,), ecx=obj)
    emu.call(method(0x04), (float(sr),), ecx=obj)
    for i, v in enumerate(params):
        emu.call(method(0x14), (i, float(v)), ecx=obj)
    buf = emu.alloc(BLOCK * 8)
    flag = emu.alloc(4)
    out = np.zeros_like(x)
    for s in range(0, len(x), BLOCK):
        blk = x[s:s + BLOCK]
        n = len(blk)
        if pitch_ptr is not None:
            cur = [v for f, v in pitch if f <= s]
            if cur:
                emu.uc.mem_write(pitch_ptr, struct.pack('<f', cur[-1]))
        emu.uc.mem_write(buf, blk.astype('<f4').tobytes())
        # part render protocol: flag = 1 while nothing upstream made sound (here: an all-zero input block)
        emu.uc.mem_write(flag, bytes((block_silent(blk) if gated else 0, 0, 0, 0)))
        emu.call(method(0x10), (buf, n, 0, 0, flag), ecx=obj, max_insns=20_000_000)
        if flags_out is not None:
            flags_out.append(emu.uc.mem_read(flag, 1)[0])
        out[s:s + n] = np.frombuffer(bytes(emu.uc.mem_read(buf, n * 8)), '<f4').reshape(n, 2)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dll')
    ap.add_argument('type', type=int)
    ap.add_argument('out')
    ap.add_argument('--params', default='')
    ap.add_argument('--sr', type=int, default=44100)
    ap.add_argument('--seconds', type=float, default=2.0)
    ap.add_argument('--signal', default='mix')
    ap.add_argument('--pitch', action='store_true', help='connect a note-pitch source (default timeline)')
    ap.add_argument('--tempo', type=float, default=None, help='call the tempo method (vtable +0xc) with this bpm')
    a = ap.parse_args()
    params = [float(v) for v in a.params.split(',')] if a.params else []
    x = test_signal(a.signal, a.sr, a.seconds)
    x.astype('<f4').tofile(a.out + '.in.f32')
    y = run_effect(a.dll, a.type, params, a.sr, x, default_pitch(a.sr) if a.pitch else None, a.tempo)
    y.astype('<f4').tofile(a.out)
    print(f'type {a.type}: {len(x)} frames, out peak {np.abs(y).max():.4f}, rms {np.sqrt(np.mean(y ** 2)):.5f}')


if __name__ == '__main__':
    main()
