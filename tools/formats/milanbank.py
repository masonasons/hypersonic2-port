"""Container parser for Wizoo "Milan Bank" files (all Hypersonic 2 content banks).

Taken from Hypersonic.dll FUN_10019290 (bank open) and FUN_1001a700 (streaming load/unload).

File header (0x58 bytes, read in one fread):
  0x00  ASCII signature, e.g. "Milan Bank file P016.0 Copyright (c)2003 Wizoo\\0\\x1a".
        Tunings say "Hypersonic bank T000.0 ...".  The loader checks chars 0-2 (M/H, i/y, l/p)
        and 0x29 == 'W'.  Char 16 = content letter: S samples, P patches, H phrases (arp),
        W wavetables, D drawbars, T tunings.
  0x30  u16 kind     (stored as bank type; e.g. 1 = patches?)
  0x32  u16 version  (must be < 0x15; 13 => sizes ^0xDEADBEEF, offsets ^0xBADEAFFE)
  0x50  u32 info offset (rel. to directory block, 0 = none); the byte there (<16) indexes a slot
  0x54  u32 directory block size; the block is read from file offset 0x58

Directory block (at file 0x58, offsets inside it are relative to 0x58):
  +0x00 u32 count        +0x04 u32 name table offset
  +0x08 u32 secondary table offset   +0x0c u32 secondary count
  +0x10 entries[count], 0x30 bytes each:
     +0x00 name offset  +0x04 name2 offset    (into the name table, Pascal strings)
     +0x08 (runtime ptr)  +0x0c flag  +0x10 offset1  +0x14 size1      slot 1 (id = index)
     +0x18 (runtime ptr)  +0x1c flag  +0x20 offset2  +0x24 size2      slot 2 (id = index + 0x1000000)
     +0x28, +0x2c ?
  secondary records (0x20 bytes; id = index + 0x2000000; sample data in S banks):
     +0x00 (runtime ptr)  +0x04 flag  +0x08 offset  +0x0c size  +0x10.. ?
     record 0 is a sentinel (offset 0xff, size 0, never masked), so sample numbers are 1-based.
Verified: with this layout every bank in the factory content (125 files) tiles exactly from
0x58 + dir_size to EOF.
The streaming loader only counts a slot-1 size if > 4, and only loads slot 2 / secondaries
when secondary count >= 2.
"""
import struct
import sys
from dataclasses import dataclass, field

KINDS = {'S': 'samples', 'P': 'patches', 'H': 'phrases', 'W': 'wavetables', 'D': 'drawbars', 'T': 'tunings'}
OFF_MASK, SIZE_MASK = 0xBADEAFFE, 0xDEADBEEF


@dataclass
class Slot:
    flag: int
    offset: int
    size: int


@dataclass
class Entry:
    index: int
    name: str
    name2: str
    slot1: Slot
    slot2: Slot
    raw: bytes = field(repr=False)


@dataclass
class Secondary:
    index: int
    flag: int
    offset: int
    size: int
    raw: bytes = field(repr=False)


@dataclass
class Bank:
    path: str
    signature: str
    letter: str
    kind: int
    version: int
    info_offset: int
    entries: list
    secondaries: list
    data: bytes = field(repr=False)

    @property
    def masked(self):
        return self.version == 13

    def read(self, slot) -> bytes:
        return self.data[slot.offset:slot.offset + slot.size]


DIR = 0x58


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def _pstr(d, p):
    if not (0 <= p < len(d)):
        return None
    n = d[p]
    s = d[p + 1:p + 1 + n]
    return s.decode('latin1') if len(s) == n else None


def parse(path) -> Bank:
    d = open(path, 'rb').read()
    if not (d[0] in b'MH' and d[1] in b'iy' and d[2] in b'lp' and d[0x29:0x2a] == b'W'):
        raise ValueError(f'{path}: not a Milan bank')
    sig = d[:0x2e].split(b'\0')[0].decode('latin1')
    kind, version = struct.unpack_from('<HH', d, 0x30)
    if version >= 0x15:
        raise ValueError(f'{path}: version {version} rejected by loader')
    info_off, dir_size = struct.unpack_from('<II', d, 0x50)
    count, names_off, sec_off, sec_count = struct.unpack_from('<IIII', d, DIR)
    om, sm = (OFF_MASK, SIZE_MASK) if version == 13 else (0, 0)
    names = DIR + names_off

    def slot(o):
        flag, off, size = _u32(d, o), _u32(d, o + 4), _u32(d, o + 8)
        if size:  # empty slots / the secondary-0 sentinel (offset 0xff, size 0) are stored unmasked
            off ^= om
            size ^= sm
        return Slot(flag, off, size)

    entries = []
    for i in range(count):
        e = DIR + 0x10 + i * 0x30
        entries.append(Entry(i, _pstr(d, names + _u32(d, e)), _pstr(d, names + _u32(d, e + 4)),
                             slot(e + 0x0c), slot(e + 0x1c), d[e:e + 0x30]))
    secs = []
    for i in range(sec_count):
        r = DIR + sec_off + i * 0x20
        s = slot(r + 0x04)
        secs.append(Secondary(i, s.flag, s.offset, s.size, d[r:r + 0x20]))
    return Bank(str(path), sig, sig[16], kind, version, info_off, entries, secs, d)


def coverage(bank: Bank):
    """Return (ok, gaps): do all data ranges tile [0x58+dir_size, EOF) exactly?"""
    start = DIR + _u32(bank.data, 0x54)
    ranges = []
    for e in bank.entries:
        for tag, s in (('s1', e.slot1), ('s2', e.slot2)):
            if s.size:
                ranges.append((s.offset, s.offset + s.size, f'e{e.index}.{tag}'))
    for s in bank.secondaries:
        if s.size:
            ranges.append((s.offset, s.offset + s.size, f'x{s.index}'))
    ranges.sort()
    gaps, pos = [], start
    for a, b, tag in ranges:
        if a != pos:
            gaps.append((pos, a, tag))
        pos = max(pos, b)
    if pos != len(bank.data):
        gaps.append((pos, len(bank.data), 'EOF'))
    return not gaps, gaps


if __name__ == '__main__':
    for p in sys.argv[1:]:
        b = parse(p)
        ok, gaps = coverage(b)
        print(f'{b.signature!r} {KINDS.get(b.letter, b.letter)} kind={b.kind} ver={b.version} '
              f'entries={len(b.entries)} secondaries={len(b.secondaries)} coverage={"ok" if ok else gaps[:4]}')
        for e in b.entries:
            print(f'  {e.index:3d} s1={e.slot1.offset:#x}+{e.slot1.size:#x} f{e.slot1.flag} '
                  f's2={e.slot2.offset:#x}+{e.slot2.size:#x} f{e.slot2.flag}  {e.name!r} / {e.name2!r}')
