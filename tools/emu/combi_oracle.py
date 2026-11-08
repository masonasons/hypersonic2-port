"""Render a combi (or patches in several parts) with the ORIGINAL Hypersonic 2 engine (emulated): the reference for
hs2::Engine (core/src/engine.cpp, hs2render --combi / --part).

Usage:
  python -I combi_oracle.py <Hypersonic.dll> --combi "<file.fxb>" --out file.wav
         [--part "ch:bank:patch,..."] [--notes "t:len:pitch:vel[:ch],..."] [--events "t:kind[/ch]:a[:b],..."]
         [--host "t:bpm:play[:ppq],..."] [--block 32] [--lead 0.1] [--tail 1] [--outputs 1] [--memory 0.5]
         [--content "C:/Hypersonic 2 Content"] [--f32 raw.f32]
  --combi loads a combi into a fresh plugin; --part loads patches (bank/program of the user's banks) into parts
  (channel 1..16) after it (or into a fresh plugin without --combi). Notes and events are on MIDI channel ch (1..16,
  default 1); events: cc:num:value, pb:value14, at:value, on:note:vel, off:note, pc:program, raw:s:d1:d2[:d3] (the
  status carries its own channel), e.g. "0.5:cc/3:7:100" = CC 7 = 100 on channel 3; control events between blocks
  (the block ends there; MIDI at the same time follows them): load:ch:bank:patch (the plugin's patch loader),
  vst:index:value (setParameter, e.g. vst:0x99:1 = part 2 Mute), combi:INDEX (factory combi 120/INDEX).

What runs. Original code (in the emulator):
  content manager, banks (Samples, Wavetables, Drawbars, Tunings, Phrases), synth ctor 1007c690 (17 parts: the global
  part + one per channel), engine setSampleRate 1007ba30 (the plugin's resume), and on a FAKE PLUGIN OBJECT (the
  plugin's vtable 103b8f38 and the fields these functions read, set up as the plugin ctor 1008bad0 / init 100887f0
  leave them; the Syncrosoft licence gate byte 103c5538 is set, nothing is decrypted):
    master / hype FUN_10089380 (-> 1007c610 tone enhancer setup, content+0x12dc master gain),
    vel / touch tables 10083ea0 / 10083fb0,
    setChunk 1008c270 for the combi (-> 10089ef0 per part: Engine::loadPatch 1007bbb0 + the part settings through
    setParameter 1008a950, 10089c90 for empty parts, the global send FX through the setter on the global part,
    10086cf0, the settings: master volume, links, ...),
    10089ef0 for --part loads (as the plugin's patch loader with isRestore 0, bank -1),
    processEvents 1008d5e0 per host block (VstEvents built here: routing by channel, links, velocity tables, bank
    select / RPN / NRPN / program change bookkeeping),
    and the multi-part render 1007bcf0 (every channel's part split at its events, old parts, the global part's send FX
    1000f130 with offset -1, the overflow guard, the tone enhancer on used outputs).
  Mirrored in Python (plugin code that talks to the host): process 1008d400 (event list terminators, the song
  position) and the host sync 100884c0 (HostSync, calling 1007baf0 on the synth), and 1008cc20 (queued all-sound-off
  events, each through the original 1008b110; the final host notification 10002200 is skipped).
Output: output pair 1 (main), interleaved stereo float. Streaming: everything requested is made resident after each
load (as the plugin's streaming thread would before the notes).
"""
import argparse
import math
import os
import re
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'formats'))
from part_oracle import (PartOracle, find_bank, entry_read_hook_direct, parse_host, write_wav,  # noqa: E402
                         KIND_DIRS)
import combi as combi_fmt  # noqa: E402

PLUGIN_VTABLE = 0x103b8f38
LICENCE_GATE = 0x103c5538
SET_PARAMETER = 0x1008a950
SET_CHUNK = 0x1008c270
LOAD_INTO_PART = 0x10089ef0
PROCESS_EVENTS = 0x1008d5e0
SET_HYPE = 0x10089380
VEL_TABLE = 0x10083ea0
TOUCH_TABLE = 0x10083fb0
ENGINE_RENDER = 0x1007bcf0
ENGINE_SET_SR = 0x1007ba30
ENGINE_SYNC = 0x1007baf0
PLUGIN_SIZE = 0xc000


class CombiOracle(PartOracle):
    def __init__(self, dll, content, sr=44100, block=32, outputs=1, heap_mb=1536, memory=0.5, hype=0.33):
        super().__init__(dll, content, sr, block=max(block, 32), heap_mb=heap_mb)
        emu = self.emu
        emu.user_stubs['strncmp'] = self._strncmp
        self.outputs = outputs
        self.add_all_sample_banks()
        d = os.path.join(content, 'Phrases')  # kind 6 (arp phrases, FUN_1001afb0(6, ...))
        if os.path.isdir(d):
            for n in sorted(os.listdir(d)):
                if re.match(r'^\d{3} ', n) and os.path.isfile(os.path.join(d, n)):
                    self.add_bank(6, os.path.join(d, n))
        # synth ctor (FUN_1007c690) with `outputs` stereo outputs, then the plugin's resume: FUN_1007ba30(sr, block)
        synth = emu.alloc(0x370)
        emu.call(0x1007c690, (self.cm, outputs), ecx=synth, max_insns=200_000_000)
        self.synth = synth
        emu.call(ENGINE_SET_SR, (float(sr), self.block), ecx=synth, max_insns=500_000_000)
        self.outbuf = [emu.alloc(self.block * 4) for _ in range(2 * outputs)]
        self.outs = emu.alloc(8 * outputs)
        for i, b in enumerate(self.outbuf):
            emu.w32(self.outs + 4 * i, b)
        self._plugin(memory, hype)
        self.pending_streams = True

    @staticmethod
    def _strncmp(emu, esp):
        a, b, n = (emu.u32(esp + 4 + 4 * i) for i in range(3))
        x, y = bytes(emu.uc.mem_read(a, n)), bytes(emu.uc.mem_read(b, n))
        for i in range(n):
            if x[i] != y[i]:
                return (x[i] - y[i]) & 0xffffffff
            if x[i] == 0:
                return 0
        return 0

    def _plugin(self, memory, hype):
        """The fields of the plugin object (FUN_1008bad0 ctor, FUN_100887f0 init, FUN_10088ab0 defaults)."""
        emu = self.emu
        P = emu.alloc(PLUGIN_SIZE)
        emu.uc.mem_write(P, b'\0' * PLUGIN_SIZE)
        self.P = P
        emu.w32(P, PLUGIN_VTABLE)
        emu.uc.mem_write(LICENCE_GATE, b'\x01')
        emu.w32(P + 0xaf0, self.synth)
        emu.w32(P + 0xaf4, self.synth + 8)
        emu.w32(P + 0xaf8, emu.u32(self.synth + 4))
        emu.w32(P + 0xafc, self.cm)
        emu.wf32(P + 0x32d8, float(self.sr))      # +0xcb6*4: sample rate
        emu.wf32(P + 0x32dc, 1.0 / float(self.sr))
        emu.uc.mem_write(P + 0xb00, struct.pack('<d', 1.0 / (float(np.float32(self.sr)) * 60.0)))
        emu.wf32(P + 0xa78, memory)                # "Memory Use" (0 ECO, 0.5 MID, 1 XXL)
        emu.wf32(P + 0xae8, hype)                  # +0x2ba*4: Hype (0.33 default)
        emu.w32(P + 0xa88, 4)                      # MIDI program changes
        emu.w32(P + 0xa8c, 1)                      # MIDI combi select
        emu.w32(P + 0x2d0, 0x18)                   # default scene root
        for i in range(128):
            emu.w32(P + 0xb8 + 4 * i, 0xffffffff)  # combi chain: empty
        emu.w32(P + 0x2c4, 0xffffffff)
        emu.w32(P + 0x2c0, 0xffffffff)
        emu.w32(P + 0x2b8, 0xffffffff)
        emu.w32(P + 0x2c8, 0x3f)
        # VST parameters (+0xb54 + 4 p), ctor defaults
        pv = {0: 0.5}
        for ch in range(16):
            b = 3 + ch * 0x98
            for o, v in ((0, 0.5), (1, 0.05), (0xd, 1.0), (0x10, 1.0), (0x12, 0.5), (0x13, 0.5), (0x14, 0.5),
                         (0x17, struct.unpack('<f', struct.pack('<I', 0x3f356d5d))[0]), (0x47, 0.5), (0x48, 0.5),
                         (0x5f, 0.5), (0x60, 0.5), (0x77, 0.5), (0x78, 0.5), (0x8f, 0.5), (0x90, 0.5)):
                pv[b + o] = v
        for k in range(4):
            pv[0x991 + 0x18 * k] = 0.05
            pv[0x992 + 0x18 * k] = 0.5
            pv[0x993 + 0x18 * k] = 0.5
        for p, v in pv.items():
            emu.wf32(P + 0xb54 + 4 * p, v)
        # Hyperknob stores (+0x724, 6 per part) = 0.5 (FUN_100887f0)
        for i in range(16 * 6):
            emu.wf32(P + 0x724 + 4 * i, 0.5)
        # per-channel MIDI event lists (+0xb588 bases, +0xb5c8 write pointers), 0x400 bytes each
        for ch in range(16):
            lst = emu.alloc(0x400)
            emu.w32(P + 0xb588 + 4 * ch, lst)
            emu.w32(P + 0xb5c8 + 4 * ch, lst)
            emu.w32(lst, 99999999)
        emu.w32(P + 0xb760, emu.alloc(0x400))      # display event buffer (FUN_1008b110)
        emu.call(VEL_TABLE, (0.5,), ecx=P)
        emu.call(TOUCH_TABLE, (0.5,), ecx=P)
        # master volume (FUN_1008a950(0, v) -> FUN_10089380(hype)): content+0x12dc and the tone enhancer
        self.set_parameter(0, 0.5)

    # ---- plugin calls -----------------------------------------------------------------------------------------
    def set_parameter(self, p, v):
        self.emu.call(SET_PARAMETER, (p, float(v)), ecx=self.P, max_insns=500_000_000)

    def load_combi(self, chunk):
        emu = self.emu
        buf = emu.alloc(len(chunk) + 16)
        emu.uc.mem_write(buf, chunk)
        emu.call(SET_CHUNK, (buf, len(chunk), 0), ecx=self.P, max_insns=4_000_000_000)
        self.stream_all()

    def load_patch(self, ch, bank, prog):
        """The plugin's patch loader FUN_10089ef0(blob, ch, 0, -1) with a patch of the user's banks."""
        emu = self.emu
        if (1, bank) not in self.banks:
            path = find_bank(self.content, 1, bank)
            if not path:
                raise SystemExit(f'no patch bank {bank}')
            self.add_bank(1, path)
        blob = entry_read_hook_direct(emu, self.banks[(1, bank)], prog)
        if not blob:
            raise SystemExit(f'patch {bank}/{prog}: empty entry')
        emu.call(LOAD_INTO_PART, (blob, ch, 0, 0xffffffff), ecx=self.P, max_insns=2_000_000_000)
        self.stream_all()

    def process_events(self, events):
        """processEvents FUN_1008d5e0 with a VstEvents of VstMidiEvents (deltaFrames, 4 MIDI bytes)."""
        emu = self.emu
        if not events:
            # still resets the write pointers (as every call does)
            for ch in range(16):
                emu.w32(self.P + 0xb5c8 + 4 * ch, emu.u32(self.P + 0xb588 + 4 * ch))
            return
        n = len(events)
        ve = emu.alloc(8 + 4 * n)
        emu.w32(ve, n)
        for i, (ofs, m) in enumerate(events):
            e = emu.alloc(0x20)
            emu.uc.mem_write(e, struct.pack('<iiii', 1, 0x18, ofs, 0) + struct.pack('<ii', 0, 0) + bytes(m[:4]) +
                             b'\0\0\0\0')
            emu.w32(ve + 8 + 4 * i, e)
        emu.call(PROCESS_EVENTS, (ve,), ecx=self.P, max_insns=500_000_000)

    def process(self, n, pos, first):
        """FUN_1008d400 (mirrored) minus the host sync: terminators, FUN_1007bcf0, write pointers reset."""
        emu, P = self.emu, self.P
        # FUN_1008cc20: the queued plugin events (all sound off after an unlink / mute) through FUN_1008b110 (original
        # code), without its final host call FUN_10002200
        i = 0
        while emu.u32(P + 0xb504) != emu.u32(P + 0xb500):
            rd = emu.u32(P + 0xb504)
            emu.call(0x1008b110, (emu.u32(P + 0xb508 + 4 * rd), i), ecx=P, max_insns=10_000_000)
            emu.w32(P + 0xb504, (rd + 1) & 0x1f)
            i += 1
        for ch in range(16):
            emu.w32(emu.u32(P + 0xb5c8 + 4 * ch), 99999999)
        z = b'\0' * (n * 4)
        for b in self.outbuf:
            emu.uc.mem_write(b, z)
        emu.call(ENGINE_RENDER, (P + 0xb588, self.outs, n, ('d', pos), 0 if first else 1), ecx=self.synth,
                 max_insns=4_000_000_000)
        for ch in range(16):
            emu.w32(P + 0xb5c8 + 4 * ch, emu.u32(P + 0xb588 + 4 * ch))
        l = np.frombuffer(bytes(emu.uc.mem_read(self.outbuf[0], n * 4)), '<f4')
        r = np.frombuffer(bytes(emu.uc.mem_read(self.outbuf[1], n * 4)), '<f4')
        return np.stack([l, r], axis=1)

    def engine_sync(self, bpm, bps, bpb, ppq, bar, flags):
        self.emu.call(ENGINE_SYNC, (('d', bpm), ('d', bps), ('d', bpb), ('d', ppq), ('d', bar), flags),
                      ecx=self.synth, max_insns=200_000_000)

    def synth_bps(self):
        return struct.unpack('<d', bytes(self.emu.uc.mem_read(self.synth + 0x130, 8)))[0]


class HostSync:
    """FUN_100884c0 for the whole engine (calls FUN_1007baf0 on a change); +0xb20 position."""

    def __init__(self, sr):
        self.spb = 1.0 / (float(np.float32(sr)) * 60.0)
        self.bpm, self.bps, self.bpb = 0.0, 120.0 * self.spb, 4.0  # as part_oracle / PatchPlayer
        self.pos, self.bar, self.playing = 0.0, 0.0, False

    def block(self, tr, sync):
        if tr is None:
            return self.pos
        f = 0
        if tr['bpm'] != self.bpm:
            self.bpm = tr['bpm']
            f = 0x20
            self.bps = self.bpm * self.spb
        if self.playing != tr['play']:
            self.playing = tr['play']
            f += (2 if self.playing else 0) + 2
        f6 = f
        bpb = 4 * 4.0 / 4
        if self.bpb != bpb:
            self.bpb = bpb
            f6 = f + 0x48
            self.bar = tr['bar']
            if not self.playing:
                self.pos = tr['bar']
                f6 = f + 0x50
        if self.playing:
            if abs(self.pos - tr['ppq']) > float(np.float32(0.2)):
                f6 += 8
            self.pos = tr['ppq']
            if self.pos < self.bar:
                while True:
                    self.bar -= self.bpb
                    if not self.pos < self.bar:
                        break
        if f6:
            if self.playing:
                f6 += 1
            sync(self.bpm, self.bps, self.bpb, self.pos, self.bar, f6)
        return self.pos

    def advance(self, n):
        self.pos = float(n) * self.bps + self.pos


def combi_file(content, bank, index):
    d = os.path.join(content, 'Patches')
    for n in sorted(os.listdir(d)):
        if re.match(r'^\d{3} ', n) and int(n[:3]) == bank and os.path.isdir(os.path.join(d, n)):
            for m in sorted(os.listdir(os.path.join(d, n))):
                if m.lower().endswith('.fxb') and re.match(r'^\d+', m) and int(re.match(r'^\d+', m).group()) == index:
                    return os.path.join(d, n, m)
    raise SystemExit(f'no combi {bank}/{index}')


def parse_notes(s):
    seq = []
    for item in s.split(','):
        f = item.split(':')
        seq.append((float(f[0]), float(f[1]), int(f[2]), int(f[3]), int(f[4]) if len(f) > 4 else 1))
    return seq


def parse_events(s):
    out = []
    for item in s.split(','):
        f = item.split(':')
        t, k = float(f[0]), f[1]
        ch = 1
        if '/' in k:
            k, c = k.split('/')
            ch = int(c)
        c4 = (ch - 1) & 15
        if k == 'load':  # load:ch:bank:patch = the plugin's patch loader between two blocks
            out.append((t, ('load', int(f[2]), int(f[3]), int(f[4]))))
            continue
        if k == 'combi':  # combi:INDEX = factory combi (bank 120) INDEX through setChunk between two blocks
            out.append((t, ('combi', int(f[2]))))
            continue
        if k == 'vst':  # vst:index:value = setParameter between two blocks (index may be hex)
            out.append((t, ('vst', int(f[2], 0), float(f[3]))))
            continue
        num = [int(x) for x in f[2:]] + [0, 0, 0, 0]
        if k == 'cc':
            m = (0xb0 | c4, num[0], num[1], 0)
        elif k == 'pb':
            m = (0xe0 | c4, num[0] & 127, (num[0] >> 7) & 127, 0)
        elif k == 'at':
            m = (0xd0 | c4, num[0], 0, 0)
        elif k == 'on':
            m = (0x90 | c4, num[0], num[1], 0)
        elif k == 'off':
            m = (0x80 | c4, num[0], 0, 0)
        elif k == 'pc':
            m = (0xc0 | c4, num[0], 0, 0)
        elif k == 'raw':
            m = tuple(num[:4])
        else:
            raise SystemExit(f'bad --events kind {k}')
        out.append((t, m))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dll')
    ap.add_argument('--combi', default=None)
    ap.add_argument('--part', default='', help='"ch:bank:patch,..." patches loaded into parts (ch 1..16)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--f32', default=None)
    ap.add_argument('--content', default=r'C:\Hypersonic 2 Content')
    ap.add_argument('--sr', type=int, default=44100)
    ap.add_argument('--block', type=int, default=32)
    ap.add_argument('--notes', default='0:1.5:60:100')
    ap.add_argument('--events', default=None)
    ap.add_argument('--host', default=None)
    ap.add_argument('--lead', type=float, default=0.1)
    ap.add_argument('--tail', type=float, default=1.0)
    ap.add_argument('--outputs', type=int, default=1)
    ap.add_argument('--memory', type=float, default=0.5)
    ap.add_argument('--vel-sens', type=float, default=None, help='FUN_10083ea0 after loading (0.5 = linear)')
    ap.add_argument('--touch-sens', type=float, default=None, help='FUN_10083fb0 after loading (0.5 = linear)')
    ap.add_argument('--hype', type=float, default=None, help='FUN_10089380 after loading (0.33 default)')
    ap.add_argument('--heap-mb', type=int, default=1536)
    a = ap.parse_args()

    t0 = time.time()
    o = CombiOracle(a.dll, a.content, a.sr, a.block, a.outputs, a.heap_mb, a.memory)
    if a.combi:
        o.load_combi(combi_fmt.fxb_chunk(open(a.combi, 'rb').read()))
    for item in [x for x in a.part.split(',') if x]:
        ch, b, p = (int(x) for x in item.split(':'))
        o.load_patch(ch - 1, b, p)
    if a.vel_sens is not None:
        o.emu.call(VEL_TABLE, (a.vel_sens,), ecx=o.P)
    if a.touch_sens is not None:
        o.emu.call(TOUCH_TABLE, (a.touch_sens,), ecx=o.P)
    if a.hype is not None:
        o.emu.call(SET_HYPE, (a.hype,), ecx=o.P, max_insns=10_000_000)
    print(f'loaded in {time.time() - t0:.0f} s, {o.emu.loaded_bytes / 1e6:.1f} MB of content, '
          f'heap {(o.emu.heap_top - o.emu.heap_base) / 1e6:.0f} MB')

    evs = []
    end = 0.0
    for t, ln, p, v, ch in parse_notes(a.notes):
        c4 = (ch - 1) & 15
        evs.append((int((a.lead + t) * a.sr), 0, (0x90 | c4, p, v, 0)))
        evs.append((int((a.lead + t + ln) * a.sr), 0, (0x80 | c4, p, 0, 0)))
        end = max(end, t + ln)
    if a.events:
        for t, m in parse_events(a.events):  # control events first at equal times
            evs.append((int((a.lead + t) * a.sr), -1 if isinstance(m[0], str) else 1, m))
            end = max(end, t)
    evs.sort(key=lambda e: (e[0], e[1]))
    total = int((a.lead + end + a.tail) * a.sr)
    out = np.zeros((total, 2), np.float32)
    host = parse_host(a.host) if a.host else None
    hs = HostSync(a.sr)
    if not host:  # as hs2render without --host: one sync after loading (120 bpm, flags 0), no transport
        o.engine_sync(120.0, 120.0 / 60.0 / a.sr, 4.0, 0.0, 0.0, 0)
    hbpm, hppq, hplay, hT, hi = 120.0, 0.0, False, 0, 0
    t, ei, first = 0, 0, True
    t1 = time.time()
    while t < total:
        bn = min(a.block - t % a.block, total - t)
        if ei < len(evs) and isinstance(evs[ei][2][0], str):  # a control event: ends the block before it
            if evs[ei][0] <= t:
                c = evs[ei][2]
                if c[0] == 'load':
                    o.load_patch(c[1] - 1, c[2], c[3])
                elif c[0] == 'combi':
                    o.load_combi(combi_fmt.fxb_chunk(open(combi_file(a.content, 120, c[1]), 'rb').read()))
                else:
                    o.set_parameter(c[1], c[2])
                ei += 1
                continue
        for j in range(ei, len(evs)):
            if evs[j][0] >= t + bn:
                break
            if isinstance(evs[j][2][0], str):
                bn = evs[j][0] - t
                break
        tr = None
        if host is not None:
            while hi < len(host) and math.floor((a.lead + host[hi][0]) * a.sr) <= t:
                if hplay:
                    hppq = hppq + float(t - hT) * (hbpm / 60.0 / a.sr)
                hT = t
                _, hbpm, hplay, jump = host[hi]
                if jump is not None:
                    hppq = jump
                hi += 1
            ppq = hppq + float(t - hT) * (hbpm / 60.0 / a.sr) if hplay else hppq
            tr = {'bpm': hbpm, 'play': hplay, 'ppq': ppq, 'bar': math.floor(ppq / 4.0) * 4.0}
        block_ev = []
        while ei < len(evs) and evs[ei][0] < t + bn:
            block_ev.append((evs[ei][0] - t, evs[ei][2]))
            ei += 1
        o.process_events(block_ev)
        pos = hs.block(tr, o.engine_sync)
        out[t:t + bn] = o.process(bn, pos, first)
        first = False
        hs.advance(bn)  # FUN_1008d400: +0xb20 += frames * +0xb10
        t += bn
    print(f'rendered {total} frames in {time.time() - t1:.0f} s; peak {np.abs(out).max():.4f}')
    write_wav(a.out, out, a.sr)
    if a.f32:
        out.astype('<f4').tofile(a.f32)


if __name__ == '__main__':
    main()
