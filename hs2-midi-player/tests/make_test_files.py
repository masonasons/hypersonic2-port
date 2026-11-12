"""Test material for HS2 MIDI Player (no Steinberg data): a synthetic content folder and MIDI files.

  <out>/content/Patches/000 Test Patches   "Test Lead", "Test Pad"
  <out>/content/Patches/078 GM             "GM 1".."GM 8" (programs 0-7; GM programs past 7 fall back to the family)
  <out>/content/Patches/079 GM Drums       "Standard Kit"
  <out>/midi/song.mid       format 1, 480 ppq: 120 BPM, 60 BPM from bar 3, 4 bars (12 s). Track 1: channel 1, program 3
                            (GM 4). Track 2: channel 10 drums (no program change: the GM kit). Track 3: channel 2,
                            program 5 (GM 6), program 6 (GM 7) at bar 2.
  <out>/midi/sixteen.mid    format 0, 96 ppq, 2 bars at 120 BPM: a held note on each of the 16 channels.
  <out>/midi/broken.mid     not a MIDI file.

Usage: python3 -I hs2-midi-player/tests/make_test_files.py <out folder>
"""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'mtc', os.path.join(HERE, '..', '..', 'schwung', 'tests', 'make_test_content.py'))
mtc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mtc)


def analog_patch(name):
    el = mtc.voice_element(5, {0x13: 1.5 / 7.9, 0x15: 0.8, 0x21: 0.5, 0x81: 0.5})  # 0x81: no sustain fade
    return name, mtc.patch_blob(name, [''] * 6, [('Osc', el)], mtc.common_patch_params())


def vlq(n):
    out = [n & 0x7f]
    n >>= 7
    while n:
        out.insert(0, 0x80 | (n & 0x7f))
        n >>= 7
    return bytes(out)


def track(events):
    """events: [(tick, bytes)] in any order; End of Track after the last."""
    events = sorted(events, key=lambda e: e[0])
    out = bytearray()
    last = 0
    for t, b in events:
        out += vlq(t - last) + b
        last = t
    end = max(last, events[-1][0]) if events else 0
    out += vlq(0) + b'\xff\x2f\x00'
    return b'MTrk' + struct.pack('>I', len(out)) + bytes(out)


def smf(fmt, ppq, tracks):
    return b'MThd' + struct.pack('>IHHH', 6, fmt, len(tracks), ppq) + b''.join(tracks)


def tempo(t, bpm):
    us = int(round(60e6 / bpm))
    return t, b'\xff\x51\x03' + us.to_bytes(3, 'big')


def note(t, ch, n, v, length):
    return [(t, bytes([0x90 | ch, n, v])), (t + length, bytes([0x80 | ch, n, 0]))]


def main():
    out = sys.argv[1]
    pat = os.path.join(out, 'content', 'Patches')
    os.makedirs(pat, exist_ok=True)
    os.makedirs(os.path.join(out, 'content', 'Samples'), exist_ok=True)
    os.makedirs(os.path.join(out, 'midi'), exist_ok=True)
    with open(os.path.join(pat, '000 Test Patches'), 'wb') as f:
        f.write(mtc.milan_bank([analog_patch('Test Lead'), analog_patch('Test Pad')]))
    with open(os.path.join(pat, '078 GM'), 'wb') as f:
        f.write(mtc.milan_bank([analog_patch(f'GM {i + 1}') for i in range(8)]))
    with open(os.path.join(pat, '079 GM Drums'), 'wb') as f:
        f.write(mtc.milan_bank([analog_patch('Standard Kit')]))

    ppq, bar = 480, 1920
    conductor = [tempo(0, 120), (0, b'\xff\x58\x04\x04\x02\x18\x08'), (0, b'\xff\x03\x09Test Song'),
                 tempo(2 * bar, 60), (4 * bar, b'\xff\x01\x01.')]
    t1 = [(0, bytes([0xc0, 3]))]
    for b in range(4):
        t1 += note(b * bar, 0, 60, 100, bar - 10)
    t2 = []
    for b in range(4):
        for q in range(4):
            t2 += note(b * bar + q * ppq, 9, 36, 110, 100)
    t3 = [(0, bytes([0xc1, 5])), (bar, bytes([0xc1, 6]))]
    for b in range(4):
        t3 += note(b * bar + ppq, 1, 67, 90, ppq)
    with open(os.path.join(out, 'midi', 'song.mid'), 'wb') as f:
        f.write(smf(1, ppq, [track(conductor + t1), track(t2), track(t3)]))

    ev = [tempo(0, 120)]
    for ch in range(16):
        ev += note(0, ch, 48 + ch, 100, 2 * 4 * 96 - 1)
    with open(os.path.join(out, 'midi', 'sixteen.mid'), 'wb') as f:
        f.write(smf(0, 96, [track(ev)]))

    with open(os.path.join(out, 'midi', 'broken.mid'), 'wb') as f:
        f.write(b'this is not a MIDI file')
    print(f'wrote test files to {out}')


if __name__ == '__main__':
    main()
