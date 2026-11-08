"""Compare the arpeggiator port (tools/cli/arptest) with the original (arp_oracle.py) on generated scenarios.

Usage: python -I arp_compare.py <Hypersonic.dll> <arptest.exe> [--cases name1,name2|all] [--work DIR]
       [--content "C:/Hypersonic 2 Content"] [--list] [--seeds N]

Each case writes DIR/<case>.txt (scenario), runs both, and compares the logs line by line: every MIDI filter result,
every event the arp hands the part (frame offset inside the render call + 4 bytes), the CRC of the whole arp object
after every render call (pointer words as 0/1), the rand() state and call count at the end. On a difference it prints
the first differing line of each; re-run that scenario with `--full` on both sides (arp_oracle.py / arptest) to get
whole-object dumps.

Cases:
  patch_B_P        a factory patch with its own arp settings: held chords, added / released notes, an arpeggiated
                   line, sustain pedal, transport stop / start / jump, tempo change, CC 0x7b (84/1, 84/49, 86/10,
                   86/33: Phrase mode with the phrase embedded in the patch)
  mode_M           patch 0/3 forced to mode M (0..8, 10..15) with octaves / steps / gate / swing / sync / rate varied,
                   latch on and off, swing / rate / gate changes while running
  phrase_B_P       mode 9 (Phrase) with phrase bank B entry P, chords and single notes, a phrase change while running
  rand_S           random patch / params / events (seed S); --seeds N adds rand_0..rand_N-1
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

SR = 44100
PATCHES = [(0, 3), (3, 65), (37, 91), (84, 78), (83, 37)]
EMBEDDED = [(84, 1), (84, 49), (86, 10), (86, 33)]  # Phrase mode with a phrase embedded in the patch (trailer 2)
PHRASES = [(0, 1), (0, 20), (1, 0), (1, 5), (2, 2), (2, 10), (4, 1), (5, 3), (5, 10), (6, 0), (6, 30), (7, 2),
           (7, 20), (8, 2), (9, 0), (10, 0)]


def f2h(v):
    return '%08x' % struct.unpack('<I', struct.pack('<f', v))[0]


def d2h(x):
    return '%016x' % struct.unpack('<Q', struct.pack('<d', x))[0]


def pval(n, steps):
    """A setParam value giving trunc((steps + 0.9) v) = n."""
    return (n + 0.5) / (steps + 0.9)


STEPS = {0x11: 1, 0x12: 1, 0x13: 15, 0x14: 3, 0x15: 16, 0x16: 7, 0x17: 20, 0x1c: 48, 0x1d: 128, 0x1e: 128, 0x21: 12}


class Scenario:
    def __init__(self, bank, prog, seed=0, bpm=120.0):
        self.lines = [f'patch {bank} {prog}', f'sr {SR}']
        self.rng = random.Random(seed)
        self.bpm = bpm
        self.bps = bpm / 60.0 / SR
        self.beat = 0.0
        self.t = 0  # frames
        self.events = []  # (frame, order, line)
        self.order = 0

    # ---- ops at a time (seconds) -----------------------------------------------------------------------------
    def at(self, sec, line):
        self.events.append((int(sec * SR), self.order, line))
        self.order += 1

    def param(self, sec, idx, v):
        self.at(sec, ('P', idx, v))

    def pn(self, sec, idx, n):
        self.param(sec, idx, pval(n, STEPS[idx]))

    def note(self, sec, length, key, vel=100):
        self.at(sec, ('M', 0x90, key, vel))
        if length is not None:
            self.at(sec + length, ('M', 0x80, key, 0))

    def midi(self, sec, b0, b1, b2):
        self.at(sec, ('M', b0, b1, b2))

    def sync(self, sec, flags, bpm=None, ppq=None):
        self.at(sec, ('S', flags, bpm, ppq))

    # ---- render ----------------------------------------------------------------------------------------------
    def build(self, end_sec, blocks=(16, 32, 64, 100, 128, 256, 333, 512, 1024)):
        self.events.sort(key=lambda e: (e[0], e[1]))
        out = list(self.lines)
        end = int(end_sec * SR)
        ei = 0
        while self.t < end:
            while ei < len(self.events) and self.events[ei][0] <= self.t:
                op = self.events[ei][2]
                ei += 1
                if op[0] == 'P':
                    out.append(f'P {op[1]:x} {f2h(op[2])}')
                elif op[0] == 'M':
                    out.append(f'M {op[1]:02x} {op[2]:02x} {op[3]:02x}')
                elif op[0] == 'S':
                    _, flags, bpm, ppq = op
                    if bpm is not None:
                        self.bpm = bpm
                        self.bps = bpm / 60.0 / SR
                    if ppq is not None:
                        self.beat = ppq
                    out.append(f'S {flags:x} {d2h(self.bpm)} {d2h(self.beat)}')
            nxt = self.events[ei][0] if ei < len(self.events) else end
            n = self.rng.choice(blocks)
            if self.rng.random() < 0.15:
                n = self.rng.randint(1, 700)
            n = max(1, min(n, nxt - self.t, end - self.t)) if nxt > self.t else 1
            out.append(f'R {n} {d2h(self.beat)} {d2h(self.bps)}')
            self.beat = n * self.bps + self.beat
            self.t += n
        return '\n'.join(out) + '\n'


# ---- cases ---------------------------------------------------------------------------------------------------
def common_play(s, t0=0.3, chord=(60, 64, 67)):
    """Held chord, added and released notes, a line, pedal, transport and tempo events (about 16 s)."""
    s.sync(0.0, 0x21, 120.0, 0.0)
    for k in chord:
        s.note(t0, 3.2, k, 90 + k % 30)
    s.note(t0 + 1.7, 1.0, 71, 70)
    s.at(t0 + 2.4, ('M', 0x80, chord[1], 0))
    s.at(t0 + 2.41, ('M', 0x90, chord[1], 0))  # note-off as a zero-velocity note-on (already released)
    # arpeggiated line with overlaps
    for i, k in enumerate((48, 55, 60, 63, 67, 70, 72, 75)):
        s.note(4.2 + 0.31 * i, 0.55, k, 60 + 8 * i)
    # sustain pedal over released notes
    s.midi(7.3, 0xb0, 0x40, 0x7f)
    s.note(7.4, 0.4, 57, 100)
    s.note(7.5, 0.4, 61, 80)
    s.note(7.6, 0.4, 64, 110)
    s.midi(8.9, 0xb0, 0x40, 0x00)
    # transport stop / start with a jump
    s.note(9.5, 2.0, 50, 100)
    s.note(9.5, 2.0, 62, 100)
    s.sync(10.2, 0x2)
    s.sync(10.6, 0x5, ppq=33.0)
    s.note(10.8, 2.5, 52, 100)
    s.note(10.9, 2.4, 59, 90)
    s.note(11.0, 2.3, 64, 80)
    s.sync(11.6, 0x21, 97.0)
    s.sync(12.3, 0x9, ppq=40.5)
    s.midi(13.0, 0xb0, 0x7b, 0)
    s.note(13.6, 1.3, 65, 100)
    s.note(13.7, 1.2, 69, 100)
    s.sync(14.1, 0x21, 140.0)
    s.note(15.2, 0.7, 72, 127)
    return 16.5


def case_patch(bank, prog, seed):
    s = Scenario(bank, prog, seed)
    end = common_play(s)
    return s.build(end)


def case_mode(m, seed):
    s = Scenario(0, 3, seed)
    r = s.rng
    s.pn(0.0, 0x11, 1)
    s.pn(0.0, 0x13, m)
    s.pn(0.0, 0x14, r.randint(0, 3))
    s.pn(0.0, 0x15, r.choice([0, 3, 4, 7, 8, 16]))
    s.pn(0.0, 0x16, r.choice([0, 1, 2, 2, 3]))
    s.pn(0.0, 0x17, r.choice([7, 10, 12, 13, 16, 19]))
    s.param(0.0, 0x18, r.random())
    s.param(0.0, 0x19, r.choice([0.0, 0.3, 0.7, 1.0]))
    s.pn(0.0, 0x1c, r.randint(12, 36))
    s.pn(0.0, 0x1d, r.choice([0, 36, 48]))
    s.pn(0.0, 0x21, r.choice([0, 0, 1, 3, 8]))
    for i in range(16):  # step records
        for f in range(5):
            s.param(0.0, 0xac + 5 * i + f, r.random())
    if m in range(10, 16):
        s.param(0.0, 0x1a, pval(0, 128))
        s.param(0.0, 0x1b, pval(1, 128))
    end = common_play(s)
    # latch and parameter changes while running
    s.pn(2.0, 0x12, 1)
    s.pn(3.8, 0x12, 0)
    s.param(5.0, 0x19, 0.85)
    s.pn(5.6, 0x17, r.choice([9, 11, 14]))
    s.param(6.1, 0x18, 0.2)
    s.pn(6.6, 0x14, 2)
    s.pn(11.3, 0x16, 2)
    s.pn(12.8, 0x16, 1)
    return s.build(end)


def case_phrase(bank, prog, seed):
    s = Scenario(84, 78, seed)
    r = s.rng
    s.pn(0.0, 0x11, 1)
    s.pn(0.0, 0x13, 9)
    s.param(0.0, 0x1a, pval(bank, 128))
    s.param(0.0, 0x1b, pval(prog, 128))
    s.pn(0.0, 0x17, r.choice([10, 13, 13, 16]))
    s.param(0.0, 0x19, r.choice([0.0, 0.0, 0.4]))
    s.pn(0.0, 0x16, r.choice([0, 1, 2]))
    s.pn(0.0, 0x21, r.choice([0, 0, 5]))
    end = common_play(s)
    # phrase changes while running, rate and swing changes
    nb, np_ = PHRASES[(PHRASES.index((bank, prog)) + 3) % len(PHRASES)]
    s.param(5.05, 0x1a, pval(nb, 128))
    s.param(5.05, 0x1b, pval(np_, 128))
    s.param(6.2, 0x1a, pval(bank, 128))
    s.param(6.2, 0x1b, pval(prog, 128))
    s.pn(8.0, 0x17, 12)
    s.param(8.5, 0x19, 0.5)
    s.pn(9.0, 0x12, 1)
    s.pn(10.0, 0x12, 0)
    return s.build(end)


def case_rand(seed):
    r = random.Random(1000 + seed)
    bank, prog = r.choice(PATCHES)
    s = Scenario(bank, prog, seed, bpm=r.choice([90.0, 120.0, 133.0, 174.0]))
    s.sync(0.0, 0x21, s.bpm, 0.0)
    phrase = r.random() < 0.4
    mode = 9 if phrase else r.randint(0, 8)
    s.pn(0.0, 0x11, 1)
    s.pn(0.0, 0x13, mode)
    if phrase:
        pb, pp = r.choice(PHRASES)
        s.param(0.0, 0x1a, pval(pb, 128))
        s.param(0.0, 0x1b, pval(pp, 128))
    for idx in (0x14, 0x15, 0x16, 0x17, 0x1c, 0x21):
        if r.random() < 0.6:
            s.pn(0.0, idx, r.randint(0, STEPS[idx]))
    for idx in (0x18, 0x19):
        if r.random() < 0.6:
            s.param(0.0, idx, r.random())
    dur = 14.0
    t = 0.2
    held = []
    while t < dur:
        x = r.random()
        if x < 0.45 or not held:
            k = r.randint(30, 96)
            held.append(k)
            s.midi(t, 0x90, k, r.randint(1, 127))
        elif x < 0.8:
            k = held.pop(r.randrange(len(held)))
            s.midi(t, 0x80, k, 0)
        elif x < 0.85:
            s.midi(t, 0xb0, 0x40, r.choice([0, 127]))
        elif x < 0.88:
            s.pn(t, 0x12, r.randint(0, 1))
        elif x < 0.91 and not phrase:
            s.pn(t, 0x13, r.randint(0, 8))
        elif x < 0.94:
            idx = r.choice([0x17, 0x18, 0x19, 0x14, 0x15])
            if idx in STEPS:
                s.pn(t, idx, r.randint(0, STEPS[idx]))
            else:
                s.param(t, idx, r.random())
        elif x < 0.96:
            s.sync(t, r.choice([0x21, 0x1, 0x5, 0x9, 0x2]), r.choice([None, 100.0, 125.0]),
                   r.choice([None, r.random() * 64]))
        elif x < 0.97:
            s.midi(t, 0xb0, 0x7b, 0)
            held.clear()
        t += r.expovariate(4.0)
    for k in held:
        s.midi(dur, 0x80, k, 0)
    return s.build(dur + 1.5)


def all_cases(seeds):
    cases = {}
    for b, p in PATCHES + EMBEDDED:
        cases[f'patch_{b}_{p}'] = lambda b=b, p=p: case_patch(b, p, b * 1000 + p)
    for m in list(range(0, 9)) + [10, 15]:
        cases[f'mode_{m}'] = lambda m=m: case_mode(m, 77 + m)
    for b, p in PHRASES:
        cases[f'phrase_{b}_{p}'] = lambda b=b, p=p: case_phrase(b, p, 500 + b * 50 + p)
    for i in range(seeds):
        cases[f'rand_{i}'] = lambda i=i: case_rand(i)
    return cases


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dll')
    ap.add_argument('arptest')
    ap.add_argument('--cases', default='all')
    ap.add_argument('--work', default='.')
    ap.add_argument('--content', default=r'C:\Hypersonic 2 Content')
    ap.add_argument('--seeds', type=int, default=12)
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    import arp_oracle as ao
    cases = all_cases(a.seeds)
    if a.list:
        print('\n'.join(cases))
        return
    names = list(cases) if a.cases == 'all' else a.cases.split(',')
    os.makedirs(a.work, exist_ok=True)
    bad = 0
    for name in names:
        t0 = time.time()
        text = cases[name]()
        path = os.path.join(a.work, name + '.txt')
        with open(path, 'w', newline='\n') as f:
            f.write(text)
        lines = text.splitlines()
        hdr = {t[0]: t[1:] for t in (ln.split() for ln in lines) if t and t[0] in ('patch', 'sr')}
        bank, prog = (int(x) for x in hdr['patch'])
        o = ao.ArpOracle(a.dll, a.content, int(hdr['sr'][0]))
        o.load(bank, prog)
        o.emu.rand_state = 1
        o.emu.rand_log = []
        import zlib
        ref = [f'load {bank}/{prog} {zlib.crc32(o.state()):08x}']
        ao.run(o, lines, ref)
        port = subprocess.run([a.arptest, path, '--content', a.content], capture_output=True, text=True)
        got = port.stdout.splitlines()
        with open(os.path.join(a.work, name + '.oracle.log'), 'w', newline='\n') as f:
            f.write('\n'.join(ref) + '\n')
        with open(os.path.join(a.work, name + '.port.log'), 'w', newline='\n') as f:
            f.write('\n'.join(got) + '\n')
        nev = sum(ln.count(':') for ln in ref if ln.startswith('R'))
        diff = next((i for i, (x, y) in enumerate(zip(ref, got)) if x != y), None)
        if diff is None and len(ref) != len(got):
            diff = min(len(ref), len(got))
        dt = time.time() - t0
        if diff is None:
            print(f'{name:16s} IDENTICAL  {len(ref)} lines, {nev} events, {ref[-1]}  ({dt:.0f} s)')
        else:
            bad += 1
            print(f'{name:16s} DIFFERS at line {diff}:\n   oracle: {ref[diff] if diff < len(ref) else "<end>"}\n'
                  f'   port:   {got[diff] if diff < len(got) else "<end>"}' + (f'\n   stderr: {port.stderr[:300]}'
                                                                              if port.stderr else ''))
        sys.stdout.flush()
    print(f'{len(names) - bad}/{len(names)} identical')


if __name__ == '__main__':
    main()
