"""Run an arpeggiator scenario on the ORIGINAL arp object (CHyperArp, part+0xc44) in the emulator.

Usage: python -I arp_oracle.py <Hypersonic.dll> <scenario.txt> [--content "C:/Hypersonic 2 Content"] [--out log.txt]
       [--full]   (dump the whole object instead of a CRC after every R / D line)

The patch is loaded by the original engine (PartOracle: content manager, synth ctor, Engine::loadPatch -> part ctor
(arp init 100467b0), part setSampleRate (arp 10046340), patch load 10016f70 -> setter 100121a0 for every patch
param, which routes 0x11..0x21 to the arp's 100460b0, writes the step records, and loads the phrase for 0x1b through
FUN_1001afb0(6, ...) from the phrase banks opened here (content kind 6, folder Phrases)). No sample banks are opened
(the arp does not need them). After the load the CRT rand() state is set to 1 and its calls are counted.
Then the scenario drives the arp object directly:
  P idx v        part setter 100121a0(0, idx, v) (v as float bits, hex): the real routing, phrase loads included
  S flags bpm ppq   arp sync 10045710 (as the part's host sync 1000ef10 calls it)
  M b0 b1 b2     arp MIDI filter 10046980 (as the part's 10017880 calls it for host events). Part plumbing kept:
                 an unconsumed CC 64 < 64 makes the part send itself (0x80, 0xff, 0, 0), which goes through the arp
                 filter again (10017880 case 0x40 -> recursion with flag 0).
  R n beat bps   one part render call as the wrapper 100191b0 runs it: ev = 0; r = next(n, beat, &ev); while r < n:
                 (r > 0: render r frames, offset += r, n -= r, beat += r * bps); if ev[0]: send ev; r = next(...).
                 Logged: every sent event with its frame offset in the call.
  D              dump the object
Numbers: P/M/flags in hex; doubles (bpm, ppq, beat, bps) as the 16 hex digits of their IEEE bits.
The load itself is Engine::loadPatch 1007bbb0: part ctor, part setSampleRate, part sync 1000ef10 with the synth's
stored tempo (arp sync(0, 120.0, 0.0) for a fresh synth), patch load. The log starts with the object CRC after it.
The C++ port runs the same file with tools/cli/arptest.cpp; tools/emu/arp_compare.py diffs the two logs.
"""
import argparse
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import part_oracle as po  # noqa: E402

ARP_SYNC = 0x10045710     # (flags, double bpm, double ppq)
ARP_MIDI = 0x10046980     # (msg*) -> al
ARP_NEXT = 0x100485d0     # (frames, double songPos, ev*) -> frames to render
ARP_SIZE = 0x468
PTR_WORDS = (0x3f0, 0x3f4, 0x3f8)  # pointers in the original; compared as 0/1


class ArpOracle(po.PartOracle):
    def __init__(self, dll, content, sr):
        super().__init__(dll, content, sr, block=256, heap_mb=256)
        po.KIND_DIRS.setdefault(6, 'Phrases')
        d = os.path.join(content, 'Phrases')
        for n in sorted(os.listdir(d)):
            if re.match(r'^\d{3} ', n) and os.path.isfile(os.path.join(d, n)):
                self.add_bank(6, os.path.join(d, n))
        self.ev = self.emu.alloc(16)
        self.msg = self.emu.alloc(16)

    def load(self, bank, prog):
        self.add_bank(1, po.find_bank(self.content, 1, bank))
        self.build_synth()
        self.load_patch(bank, prog)
        self.arp = self.emu.u32(self.part + 0xc44)

    def state(self):
        b = bytearray(self.emu.uc.mem_read(self.arp, ARP_SIZE))
        for o in PTR_WORDS:
            b[o:o + 4] = struct.pack('<I', 1 if struct.unpack_from('<I', b, o)[0] else 0)
        return bytes(b)

    def sync(self, flags, bpm, ppq):
        self.emu.call(ARP_SYNC, (flags, ('d', bpm), ('d', ppq)), ecx=self.arp)

    def midi_in(self, b0, b1, b2, b3=0):
        self.emu.uc.mem_write(self.msg, bytes((b0, b1, b2, b3)))
        return self.emu.call(ARP_MIDI, (self.msg,), ecx=self.arp) & 0xff

    def next(self, frames, beat):
        return self.emu.call(ARP_NEXT, (frames, ('d', beat), self.ev), ecx=self.arp)


def dbl(h):
    return struct.unpack('<d', struct.pack('<Q', int(h, 16)))[0]


def run(o, lines, out, full=False):
    chunk = 0
    for ln in lines:
        t = ln.split()
        if not t or t[0].startswith('#'):
            continue
        op = t[0]
        if op == 'P':
            idx, bits = int(t[1], 16), int(t[2], 16)
            v = struct.unpack('<f', struct.pack('<I', bits))[0]
            o.set_param(0, idx, v)
            out.append(f'P {idx:#x} {bits:08x}')
        elif op == 'S':
            flags, bpm, ppq = int(t[1], 16), dbl(t[2]), dbl(t[3])
            o.sync(flags, bpm, ppq)
            out.append(f'S {flags:#x}')
        elif op == 'M':
            b = [int(x, 16) for x in t[1:4]]
            r = o.midi_in(*b)
            s = f'M {b[0]:02x} {b[1]:02x} {b[2]:02x} -> {r}'
            if not r and b[0] == 0xb0 and b[1] == 0x40 and b[2] < 0x40:
                s += f' | 80 ff 00 -> {o.midi_in(0x80, 0xff, 0)}'
            out.append(s)
        elif op == 'R':
            n, beat, bps = int(t[1]), dbl(t[2]), dbl(t[3])
            o.emu.uc.mem_write(o.ev, b'\0\0\0\0')
            off = 0
            evs = []
            r = o.next(n, beat)
            while r < n:
                if r > 0:
                    off += r
                    n -= r
                    beat = r * bps + beat
                ev = bytes(o.emu.uc.mem_read(o.ev, 4))
                if ev[0]:
                    evs.append(f'{off}:{ev.hex()}')
                r = o.next(n, beat)
            st = o.state()
            line = f'R{chunk} ' + ' '.join(evs) + f' | {zlib.crc32(st):08x}'
            if full:
                line += ' ' + st.hex()
            out.append(line)
            chunk += 1
        elif op == 'D':
            out.append('D ' + o.state().hex())
        elif op in ('patch', 'sr', 'content'):
            pass
        else:
            raise ValueError(f'bad scenario line: {ln!r}')
    calls = len(o.emu.rand_log) if o.emu.rand_log is not None else -1
    out.append(f'end rand=0x{o.emu.rand_state:08x} calls={calls}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dll')
    ap.add_argument('scenario')
    ap.add_argument('--content', default=r'C:\Hypersonic 2 Content')
    ap.add_argument('--out', default=None)
    ap.add_argument('--full', action='store_true')
    a = ap.parse_args()
    lines = open(a.scenario).read().splitlines()
    hdr = {t[0]: t[1:] for t in (ln.split() for ln in lines) if t and t[0] in ('patch', 'sr', 'content')}
    content = ' '.join(hdr['content']) if 'content' in hdr else a.content
    sr = int(hdr.get('sr', ['44100'])[0])
    bank, prog = (int(x) for x in hdr['patch'])
    o = ArpOracle(a.dll, content, sr)
    o.load(bank, prog)
    o.emu.rand_state = 1
    o.emu.rand_log = []
    st = o.state()
    out = [f'load {bank}/{prog} {zlib.crc32(st):08x}' + (' ' + st.hex() if a.full else '')]
    run(o, lines, out, a.full)
    text = '\n'.join(out) + '\n'
    if a.out:
        with open(a.out, 'w', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
