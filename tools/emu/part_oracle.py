"""Render one patch with the ORIGINAL Hypersonic 2 engine (emulated): the reference for the port's voice engine.

Usage:
  python -I part_oracle.py <Hypersonic.dll> --bank B --patch P --out file.wav
         [--content "C:/Hypersonic 2 Content"] [--sr 44100] [--block 32]
         [--notes "t:len:pitch:vel,..."] [--lead 4] [--tail 3] [--solo 0,3] [--sends] [--master X] [--f32 raw.f32]
Compare with the port: hs2render --bank B --patch P --dry [--notes ... --lead ... --tail ... --solo ...] --out p.wav,
then part_compare.py oracle.wav p.wav --seg "4:7.5,...". About 1-2 s of CPU per second of audio per layer.

What runs (all original code, nothing re-implemented):
  content manager ctor 1001b310 -> bank open 1001ad50/10019290 (fopen/fread stubbed with host reads)
  synth ctor 1007c690 (17 parts) -> bus buffers 1001b040 -> Engine::loadPatch 1007bbb0 (new part, part
  setSampleRate 100137f0, patch load 10016f70) -> sample streaming 1001b0e0/1001a700 until everything requested
  is resident. The entry reader 10019fb0 (behind the Syncrosoft wrappers) is replaced by a Python hook that reads
  the same byte range from the bank file. MIDI goes to the part's event handler 10017880; audio comes from the
  part render 100191b0 -> 100139c0 (voices, insert FX, element sends, the part's own send FX 1000f130).

Output = the part's main stereo out (part output 1), i.e. the dry bus * part gain (Level^2*Trim^2*8, pan law)
* master, where master = content+0x12dc (default here: the port's master constant b54^2*(8-2.2*drive)/4 with
b54 = 0.5, drive = 0.33; --master 1 for the raw part gain). NOT included: global send FX (their buses are
discarded), the master tone enhancer and the VST master. Patch-own send FX are included unless the patch send
levels are zeroed, which is the default (--sends keeps them): this matches `hs2render --dry`.

The default note sequence is hs2render's (C4 / C2 / C6 / C4 v30 / C4 v127 / chord after a 4 s lead).
Host blocks are --block samples (default 32), aligned to absolute time and split at note events (as the main
render 1007bcf0 splits at events); the patch LFOs advance once per call of >= 16 samples, as in the original.
--events "t:kind:a[:b],..." adds MIDI at t seconds after the lead-in (cc:num:value, pb:value14, at:value,
on:note:vel, off:note, raw:status:d1:d2[:d3], par:elem:idx:value = the part setter FUN_100121a0 between two
render calls, elem 0 = patch, elem >= 1 = element record elem - 1; content it requests is streamed in at once,
as the plugin's streaming thread would shortly after); at equal times the notes go first (as hs2render).
--host "t:bpm:play[:ppq],..." simulates a host transport (start: stopped, 120 bpm, ppq 0, 4/4): every host block
runs the plugin's host sync FUN_100884c0 (mirrored in HostSync below: resume() state, flags 0x20 tempo, +2/+4
stop/start, +8 jump, +1 playing) and calls the part sync 1000ef10 when it reports a change; without --host, --tempo
syncs once after loading (flags 0) and no transport is given (the song position just counts on).
The song position passed to the part render is the plugin's: the host position at the block start, advanced by
n * beats per sample (synth+0x130) at each event split (1007bcf0), and by block * beats per sample (plugin
+0xb10) per block (1008d400). hs2render does the same with PatchPlayer::processBlock.
--solo mutes the other element records in the blob before loading (element param 0), exactly as hs2render --solo.
Original artefact in the output: 1e-6 is added to the part bus's first frame at every call (and once more per
insert), i.e. ~3e-7 impulses every block after the part gain. The port adds the same (PatchPlayer, kBias), so
comparisons need no masking as long as hs2render calls the part with the same blocks (it does: 32-frame blocks
aligned to absolute time, split at note events, as here).
"""
import argparse
import math
import os
import re
import struct
import sys
import time

import numpy as np
from unicorn.x86_const import UC_X86_REG_ECX

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from hs2emu import Emu  # noqa: E402

# original functions
CM_CTOR = 0x1001b310        # content manager (0x12e8 bytes)
CM_ADD_BANK = 0x1001ad50    # (path, name, index, kind): new bank 0x450 + FUN_10019290 open
CM_BUSES = 0x1001b040       # (unused, block size): 10 stereo bus buffers at cm+0x44
CM_STREAM = 0x1001b0e0      # one streaming pass over all banks
ENTRY_READ = 0x10019fb0     # bank::readEntry(id) -> new[] buffer (replaced by a Python hook)
SYNTH_CTOR = 0x1007c690     # (content, stereo outputs)
LOAD_PATCH = 0x1007bbb0     # Engine::loadPatch(channel, blob, memory mode)
NOTE_EVENT = 0x10017880     # part MIDI event (bytes*, flag)
PART_RENDER = 0x100191b0    # (outs**, n, double beat position, offset)
TEMPO_UPDATE = 0x1000ef10   # part::sync(bpm, beats/sample, beats/bar, ppq, bar start ppq, flags)
ELEM_SETTER = 0x100121a0    # part::setParam(elem, idx, value) (elem 0 = patch)

KIND_DIRS = {1: 'Patches', 2: 'Samples'}
# hs2render's test sequence (tools/cli/hs2render.cpp kSeq): (start s, length s, pitch, velocity)
DEFAULT_SEQ = [(0.0, 2.0, 60, 100), (3.5, 1.0, 36, 100), (5.5, 1.0, 84, 100), (7.5, 1.0, 60, 30),
               (9.5, 1.0, 60, 127), (11.5, 1.5, 60, 100), (11.5, 1.5, 64, 100), (11.5, 1.5, 67, 100)]
SEQ_END = 13.0
PORT_MASTER = 0.5 * 0.5 * (8.0 - 2.2 * 0.33) * 0.25


class Files:
    """CRT FILE* stubs backed by host files (read-only)."""

    def __init__(self, emu):
        self.emu, self.open = emu, {}
        for name in ('fopen', 'fread', 'fseek', 'ftell', 'fclose'):
            emu.user_stubs[name] = getattr(self, name)

    def arg(self, esp, i):
        return self.emu.u32(esp + 4 + 4 * i)

    def fopen(self, emu, esp):
        path, mode = emu.cstr(self.arg(esp, 0), 1024), emu.cstr(self.arg(esp, 1))
        if 'w' in mode or 'a' in mode or '+' in mode:
            raise RuntimeError(f'fopen for writing refused: {path!r} {mode!r}')
        try:
            f = open(path.replace('/', os.sep), 'rb')
        except OSError:
            return 0
        h = emu.alloc(0x20)
        self.open[h] = f
        return h

    def fread(self, emu, esp):
        buf, size, count, h = (self.arg(esp, i) for i in range(4))
        data = self.open[h].read(size * count)
        emu.uc.mem_write(buf, data)
        return len(data) // size if size else 0

    def fseek(self, emu, esp):
        h, off, whence = self.arg(esp, 0), self.arg(esp, 1), self.arg(esp, 2)
        off = off - (1 << 32) if off & 0x80000000 else off
        self.open[h].seek(off, whence)
        return 0

    def ftell(self, emu, esp):
        return self.open[self.arg(esp, 0)].tell()

    def fclose(self, emu, esp):
        f = self.open.pop(self.arg(esp, 0), None)
        if f:
            f.close()
        return 0


def entry_read_hook(emu, esp):
    """Python replacement for FUN_10019fb0(id) on a file bank: entry i slot 1 / slot 2 (i + 0x1000000) /
    secondary (i + 0x2000000), read raw from the bank file at the (unmasked) offset."""
    ecx = emu.uc.reg_read(UC_X86_REG_ECX)
    ident = emu.u32(esp + 4)
    idx = ident & 0xfffff
    ver = emu.u32(ecx + 0x44c)
    om, sm = (0xBADEAFFE, 0xDEADBEEF) if ver == 13 else (0, 0)
    if ident & 0x2000000:
        if idx == 0 or idx >= emu.u32(ecx + 0x444):
            return 0
        rec = emu.u32(ecx + 0x448) + idx * 0x20
        off, size = emu.u32(rec + 8), emu.u32(rec + 0xc)
    else:
        if idx >= emu.u32(ecx + 0x438):
            return 0
        e = emu.u32(ecx + 0x43c) + idx * 0x30
        if ident & 0x1000000 and emu.u32(e + 0x24):
            off, size = emu.u32(e + 0x20), emu.u32(e + 0x24)
        else:
            off, size = emu.u32(e + 0x10), emu.u32(e + 0x14)
    if size == 0:
        return 0
    off, size = off ^ om, size ^ sm
    path = emu.cstr(ecx + 0x2d, 1024)
    with open(path.replace('/', os.sep), 'rb') as f:
        f.seek(off)
        data = f.read(size)
    if len(data) != size:
        return 0
    # as the original's reader (FUN_10083a50) returns it: the payload after the 'SyncrosoftContentHeader' licence blob
    # (8 + u32@27 bytes; e.g. every Drawbars entry has one). The payload itself is plain data.
    while data[:23] == b'SyncrosoftContentHeader':
        data = data[8 + struct.unpack_from('<I', data, 27)[0]:]
    p = emu.alloc(len(data))
    emu.uc.mem_write(p, data)
    emu.sizes[p] = len(data)
    emu.loaded_bytes += len(data)
    return p


def find_bank(content, kind, number):
    d = os.path.join(content, KIND_DIRS[kind])
    for n in sorted(os.listdir(d)):
        if re.match(r'^\d{3} ', n) and int(n[:3]) == number and os.path.isfile(os.path.join(d, n)):
            return os.path.join(d, n)
    return None


class PartOracle:
    def __init__(self, dll, content, sr=44100, block=256, heap_mb=1024, master=PORT_MASTER):
        emu = Emu(dll, heap_size=heap_mb << 20)
        emu.loaded_bytes = 0
        self.emu = emu
        self.files = Files(emu)
        emu.user_stubs['OutputDebugStringA'] = lambda e, esp: (0, 4)
        emu.hook_function(ENTRY_READ, entry_read_hook, ret_pop=4)
        self.content, self.sr, self.block = content, sr, block
        cm = emu.alloc(0x12e8)
        emu.call(CM_CTOR, (), ecx=cm)
        self.cm = cm
        self.banks = {}
        self.wf32(cm + 0x12dc, master)

    def wf32(self, a, v):
        self.emu.wf32(a, v)

    def add_bank(self, kind, path):
        name = os.path.basename(path)
        index = int(name[:3])
        emu = self.emu
        p_path = emu.alloc(1024)
        p_name = emu.alloc(256)
        emu.uc.mem_write(p_path, path.replace('\\', '/').encode('latin1') + b'\0')
        emu.uc.mem_write(p_name, name[4:].encode('latin1') + b'\0')
        emu.call(CM_ADD_BANK, (p_path, p_name, index, kind), ecx=self.cm)
        table = {1: 0x4bc, 2: 0x6bc, 3: 0x8bc, 4: 0xabc, 5: 0xcbc, 6: 0xebc}[kind]  # content-manager bank tables
        b = emu.u32(self.cm + table + index * 4)
        if not b:
            raise RuntimeError(f'bank open failed: {path}')
        self.banks[(kind, index)] = b
        return b

    def add_all_sample_banks(self):
        """Sample (kind 2), Wavetable (kind 3), Drawbar (kind 5) and Tuning (kind 4) banks: everything an element or
        the patch microtuning (FUN_10010770) can reference."""
        for kind, sub in ((2, 'Samples'), (3, 'Wavetables'), (5, 'Drawbars'), (4, 'Tunings')):
            d = os.path.join(self.content, sub)
            if not os.path.isdir(d):
                continue
            for n in sorted(os.listdir(d)):
                if re.match(r'^\d{3} ', n) and os.path.isfile(os.path.join(d, n)):
                    self.add_bank(kind, os.path.join(d, n))

    def build_synth(self, outputs=1):
        emu = self.emu
        synth = emu.alloc(0x370)
        emu.call(SYNTH_CTOR, (self.cm, outputs), ecx=synth, max_insns=200_000_000)
        self.synth = synth
        emu.call(CM_BUSES, (0, self.block), ecx=self.cm)
        self.outL = emu.alloc(self.block * 4)
        self.outR = emu.alloc(self.block * 4)
        self.outs = emu.alloc(8 * outputs)
        emu.w32(self.outs, self.outL)
        emu.w32(self.outs + 4, self.outR)
        for i in range(1, outputs):  # extra outputs share the main buffers
            emu.w32(self.outs + 8 * i, self.outL)
            emu.w32(self.outs + 8 * i + 4, self.outR)
        self.midi = emu.alloc(16)

    def load_patch(self, bank, prog, channel=0, memory_mode=0.5, solo=()):
        emu = self.emu
        pb = self.banks[(1, bank)]
        blob = entry_read_hook_direct(emu, pb, prog)
        if not blob:
            raise RuntimeError(f'patch {bank}/{prog}: empty entry')
        if solo:  # mute every element record but these (element param 0 = Mute; records at 0x620 + i*0x420)
            for i in range(emu.u32(blob + 0x14)):
                if i not in solo:
                    emu.wf32(blob + 0x620 + i * 0x420 + 0x20, 1.0)
        emu.call(LOAD_PATCH, (channel, blob, memory_mode), ecx=self.synth, max_insns=500_000_000)
        self.part = emu.u32(self.synth + 8 + channel * 4)
        self.stream_all()

    def stream_all(self, max_passes=10000):
        emu = self.emu
        for i in range(max_passes):
            if emu.u32(self.cm + 0x10c4) == emu.u32(self.cm + 0x10c0):
                return i
            emu.call(CM_STREAM, (), ecx=self.cm, max_insns=500_000_000)
        raise RuntimeError('streaming did not settle')

    def sync(self, bpm, bps, bpb, ppq, bar, flags):
        """FUN_1000ef10 with explicit arguments (as FUN_1007baf0 calls it)."""
        self.emu.call(TEMPO_UPDATE, (('d', bpm), ('d', bps), ('d', bpb), ('d', ppq), ('d', bar), flags),
                      ecx=self.part, max_insns=50_000_000)

    def set_tempo(self, bpm, sr, ppq=0.0, bar=0.0, bpb=4.0):
        """Host tempo/transport sync (FUN_1000ef10, called by the plugin on tempo or transport changes): sets
        part+0x958/0x95c, part+0xd90 (LFO3 tempo), calls every effect's tempo method (vtable +0xc) and, for patch
        LFOs in Tempo sync, their rate (and phase from ppq - bar for Tempo + Beat)."""
        self.emu.call(TEMPO_UPDATE, (('d', bpm), ('d', bpm / 60.0 / sr), ('d', bpb), ('d', ppq), ('d', bar), 0),
                      ecx=self.part, max_insns=50_000_000)

    def set_param(self, elem, idx, value):
        self.emu.call(ELEM_SETTER, (elem, idx, float(value)), ecx=self.part, max_insns=50_000_000)

    def patch_param(self, idx):
        return self.emu.f32(self.part + 0x4d0 + idx * 4)

    def event(self, status, d1, d2, d3=0):
        self.emu.uc.mem_write(self.midi, bytes((status, d1, d2, d3)))
        self.emu.call(NOTE_EVENT, (self.midi, 0), ecx=self.part, max_insns=50_000_000)

    def render(self, n, beat):
        emu = self.emu
        z = b'\0' * (n * 4)
        emu.uc.mem_write(self.outL, z)
        emu.uc.mem_write(self.outR, z)
        emu.call(PART_RENDER, (self.outs, n, ('d', beat), 0), ecx=self.part, max_insns=2_000_000_000)
        l = np.frombuffer(bytes(emu.uc.mem_read(self.outL, n * 4)), '<f4')
        r = np.frombuffer(bytes(emu.uc.mem_read(self.outR, n * 4)), '<f4')
        return np.stack([l, r], axis=1)


def entry_read_hook_direct(emu, bank, idx):
    """Read slot 1 of entry idx of a bank object, as FUN_10019fb0 would (used for the patch blob)."""
    esp = emu.alloc(16)
    emu.w32(esp + 4, idx)
    emu.uc.reg_write(UC_X86_REG_ECX, bank)
    return entry_read_hook(emu, esp)


class HostSync:
    """FUN_100884c0 (plugin, once per host block) for one part: returns the song position (+0xb20) and calls
    oracle.sync(...) = FUN_1000ef10 (as FUN_1007baf0 does) when the tempo, signature or transport changed."""

    def __init__(self, sr):
        self.spb = 1.0 / (float(sr) * 60.0)        # +0xb00 (resume)
        self.bpm, self.bps, self.bpb = 0.0, 120.0 * self.spb, 4.0  # resume() clears the tempo word
        self.pos, self.bar, self.playing = 0.0, 0.0, False
        self.sbps = 120.0 * self.spb               # synth+0x130 (beats per sample used by 1007bcf0)

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
            self.sbps = self.bps
        return self.pos

    def advance(self, n):
        self.pos = float(n) * self.bps + self.pos


def parse_events(s):
    """--events items -> [(t seconds, 4 MIDI bytes)]"""
    out = []
    for item in s.split(','):
        f = item.split(':')
        t, k = float(f[0]), f[1]
        if k == 'par':  # part setParam (FUN_100121a0) at time t: par:elem:idx:value (elem 0 = patch, idx may be hex)
            out.append((t, ('par', int(f[2]), int(f[3], 0), float(f[4]))))
            continue
        num = [int(x) for x in f[2:]] + [0, 0, 0, 0]
        if k == 'cc':
            m = (0xb0, num[0], num[1], 0)
        elif k == 'pb':
            m = (0xe0, num[0] & 127, (num[0] >> 7) & 127, 0)
        elif k == 'at':
            m = (0xd0, num[0], 0, 0)
        elif k == 'on':
            m = (0x90, num[0], num[1], 0)
        elif k == 'off':
            m = (0x80, num[0], 0, 0)
        elif k == 'raw':
            m = tuple(num[:4])
        else:
            raise SystemExit(f'bad --events kind {k}')
        out.append((t, m))
    return out


def parse_host(s):
    out = []
    for item in s.split(','):
        f = item.split(':')
        out.append((float(f[0]), float(f[1]), int(f[2]) != 0, float(f[3]) if len(f) > 3 else None))
    return out


def parse_notes(s):
    seq = []
    for item in s.split(','):
        t, ln, p, v = item.split(':')
        seq.append((float(t), float(ln), int(p), int(v)))
    return seq


def write_wav(path, lr, sr):
    data = lr.astype('<f4').tobytes()
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 3, 2, sr, sr * 8, 8, 32))
        f.write(b'data' + struct.pack('<I', len(data)) + data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dll')
    ap.add_argument('--bank', type=int, required=True)
    ap.add_argument('--patch', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--f32', default=None, help='also write raw float32 stereo interleaved')
    ap.add_argument('--content', default=r'C:\Hypersonic 2 Content')
    ap.add_argument('--sr', type=int, default=44100)
    ap.add_argument('--block', type=int, default=32, help='host block size (events split blocks)')
    ap.add_argument('--notes', default=None, help='"start:len:pitch:vel,..." in seconds (after --lead)')
    ap.add_argument('--lead', type=float, default=4.0)
    ap.add_argument('--tail', type=float, default=3.0)
    ap.add_argument('--sends', action='store_true', help='keep the patch send levels (own send FX audible)')
    ap.add_argument('--master', type=float, default=PORT_MASTER)
    ap.add_argument('--heap-mb', type=int, default=1024)
    ap.add_argument('--tempo', type=float, default=None, help='host tempo sync at start (bpm), as REAPER does')
    ap.add_argument('--solo', default='', help='mute every element record except these ("0" or "0,3"; 0-based)')
    ap.add_argument('--events', default=None, help='extra MIDI "t:kind:a[:b],..." (see the docstring)')
    ap.add_argument('--host', default=None, help='host transport "t:bpm:play[:ppq],..." (see the docstring)')
    a = ap.parse_args()

    o = PartOracle(a.dll, a.content, a.sr, block=max(a.block, 32), heap_mb=a.heap_mb, master=a.master)
    pbank = find_bank(a.content, 1, a.bank)
    if not pbank:
        sys.exit(f'no patch bank {a.bank}')
    o.add_bank(1, pbank)
    o.add_all_sample_banks()
    o.build_synth()
    o.load_patch(a.bank, a.patch, solo=[int(x) for x in a.solo.split(',') if x])
    print(f'{os.path.basename(pbank)} / patch {a.patch}: loaded, {o.emu.loaded_bytes / 1e6:.1f} MB of content read, '
          f'heap {(o.emu.heap_top - o.emu.heap_base) / 1e6:.0f} MB')
    if a.tempo and not a.host:
        o.set_tempo(a.tempo, a.sr)
    if not a.sends:
        for k in range(4):
            o.set_param(0, 0x3f + k, 0.0)

    seq = parse_notes(a.notes) if a.notes else DEFAULT_SEQ
    end = max(t + ln for t, ln, _, _ in seq) if a.notes else SEQ_END
    evs = []
    for t, ln, p, v in seq:
        evs.append((int((a.lead + t) * a.sr), 0, (0x90, p, v, 0)))
        evs.append((int((a.lead + t + ln) * a.sr), 0, (0x80, p, 0, 0)))
    end = a.lead + end
    if a.events:
        for t, m in parse_events(a.events):
            evs.append((int((a.lead + t) * a.sr), 1, m))
            end = max(end, a.lead + t)
    evs.sort(key=lambda e: (e[0], e[1]))  # stable: notes as hs2render's stable_sort, then the extra events
    total = int((end + a.tail) * a.sr)
    out = np.zeros((total, 2), np.float32)
    host = parse_host(a.host) if a.host else None
    hs = HostSync(a.sr)
    hbpm, hppq, hplay, hT, hi = 120.0, 0.0, False, 0, 0
    t, ei = 0, 0
    t0 = time.time()
    while t < total:
        bn = min(a.block - t % a.block, total - t)  # host block [t, t + bn)
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
        pos = hs.block(tr, o.sync)
        bend = t + bn
        while True:  # 1007bcf0: render up to each event of the block, then send it
            if ei < len(evs) and evs[ei][0] < bend:
                et = max(evs[ei][0], t)
                if et > t:
                    out[t:et] = o.render(et - t, pos)
                    pos = float(et - t) * hs.sbps + pos
                    t = et
                if evs[ei][2][0] == 'par':
                    o.set_param(*evs[ei][2][1:])
                    o.stream_all()  # content the setter requested (e.g. a tuning entry) becomes resident at once
                else:
                    o.event(*evs[ei][2])
                ei += 1
                continue
            if bend > t:
                out[t:bend] = o.render(bend - t, pos)
            t = bend
            break
        hs.advance(bn)
    print(f'rendered {total} frames in {time.time() - t0:.0f} s; peak {np.abs(out).max():.4f}')
    write_wav(a.out, out, a.sr)
    if a.f32:
        out.astype('<f4').tofile(a.f32)


if __name__ == '__main__':
    main()
