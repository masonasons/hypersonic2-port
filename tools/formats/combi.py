"""Hypersonic 2 combi (a whole 16-part setup) and Combi Chain (.h2c) parser.

A combi is a VST 2 bank file (.fxb: 'CcnK' / 'FBCh', fxID 'StSS', opaque chunk at 0xa0, chunk size at 0x9c) whose
chunk is an 'StSS' bank chunk, loaded by the plugin's setChunk (FUN_1008c270) with isPreset = 0. The factory combis
are the files of "Patches/120 Combis" (bank 120; user combi banks 121..128 are folders too). A patch chunk has 'P' at
+0xc; a combi has 'B', 1 there.

Chunk layout (offsets from the chunk start, little-endian):
  0x00 'StSS'
  0x04 u32 version (accepted 0x990EDF..0xD1CEF00; the factory combis have 0x990F46 and 0x990F3E)
  0x0c u8 'B', u8 1
  0x10 u32 offsets[18]: [0..15] the part patches (0 = an empty part), [16] the global send FX, [17] the settings
  part patch k at offsets[k]: a patch blob without its 16-byte header (setChunk rebuilds it: 'StSS', the chunk's
       version, 0, 'P', 1, 0, 0; a blob that already starts with 'StSS' at -0x10 + 3 'S' is used as is). Its patch
       params carry the part settings (mute 0x00, level 0x43, pan 0x3c, output 0x3d, FX mode 0x3e, sends 0x3f..0x42,
       key / velocity ranges 0x31..0x36, transpose 0x37, fine 0x38, ...).
  global FX at offsets[16]: 4 x (char[0x20] name + f32[0x100] effect element params) = the global part's send FX
       slots (setter elements 0x41..0x44; param 1 (type) is forced to 0 = Effect)
  settings at offsets[17]: u32 settings version v, f32 master volume (VST param 0),
       u8[16] link flags at +8 (+8 + k: part k is linked to part k - 1; k = 1..15 are read),
       v > 0x33: char[0x20] combi name at +0x18
       v > 0x3f: u32 vel sens (float bits, FUN_10083ea0), u32 touch sens (FUN_10083fb0), u32 memory mode
                 (FUN_10089340: patch param 1 of every part) at +0x38
       v > 0x4f: s32 master tune semitones, f32 master fine tune, f32 hype (FUN_10089380), u32 GM mode at +0x44
       v > 0x5f: s32 combi chain[128] at +0x54, then u32 preload mode (+0x254)
       v > 600:  u32 next CC, u32 prev CC (+0x258)
Usage: python -I combi.py <file.fxb> [--json]   or   python -I combi.py <file.h2c>
"""
import json
import os
import struct
import sys


def _cstr(b):
    return b.split(b'\0')[0].decode('latin1')


def fxb_chunk(data):
    """The opaque chunk of a VST 2 bank (.fxb) or program (.fxp) file, or data itself if it is a raw chunk."""
    if data[:4] == b'CcnK':
        if data[8:12] == b'FBCh':
            size = struct.unpack('>I', data[0x9c:0xa0])[0]
            return data[0xa0:0xa0 + size]
        if data[8:12] == b'FPCh':
            size = struct.unpack('>I', data[0x38:0x3c])[0]
            return data[0x3c:0x3c + size]
    return data


def parse_combi(chunk):
    if chunk[:4] != b'StSS' or chunk[0xc:0xe] != b'B\x01':
        raise ValueError('not a combi chunk')
    c = {'version': struct.unpack_from('<I', chunk, 4)[0]}
    offs = struct.unpack_from('<18I', chunk, 0x10)
    c['offsets'] = list(offs)
    parts = []
    for k in range(16):
        o = offs[k]
        if not o:
            parts.append(None)
            continue
        if chunk[o + 3:o + 4] == b'S':  # starts with its own header
            blob = chunk[o:]
        else:
            blob = b'StSS' + struct.pack('<I', c['version']) + b'\0\0\0\0' + b'P\x01\0\0' + chunk[o:]
        n = struct.unpack_from('<I', blob, 0x14)[0]
        nfx = sum(1 for f in blob[0x18:0x1c] if f)
        size = 0x620 + (n + nfx) * 0x420
        parts.append({'offset': o, 'name': _cstr(blob[0x20:0x40]), 'elements': n, 'send_fx_flags': list(blob[0x18:0x1c]),
                      'blob': blob, 'size_min': size,
                      'params': struct.unpack_from('<256f', blob, 0x220)})
    c['parts'] = parts
    g = offs[16]
    fx = []
    for i in range(4):
        o = g + i * 0x420
        fx.append({'name': _cstr(chunk[o:o + 0x20]), 'params': struct.unpack_from('<256f', chunk, o + 0x20)})
    c['global_fx'] = fx
    s = offs[17]
    v = struct.unpack_from('<I', chunk, s)[0]
    st = {'version': v, 'master_volume': struct.unpack_from('<f', chunk, s + 4)[0],
          'links': [chunk[s + 8 + k] for k in range(16)]}
    p = s + 0x18
    if v > 0x33:
        st['name'] = _cstr(chunk[p:p + 0x20])
        p += 0x20
    if v > 0x3f:
        st['vel_sens'], st['touch_sens'], st['memory'] = struct.unpack_from('<3f', chunk, p)
        if v > 0x4f:
            st['tune_semis'], st['tune_fine'], st['hype'], st['gm'] = struct.unpack_from('<ifff', chunk, p + 0xc)
            st['gm'] = struct.unpack_from('<I', chunk, p + 0x18)[0]
            if v > 0x5f:
                st['chain'] = list(struct.unpack_from('<128i', chunk, p + 0x1c))
                st['preload'] = struct.unpack_from('<I', chunk, p + 0x1c + 0x200 + 4)[0]
    c['settings'] = st
    return c


def parse_chain(data):
    """Combi Chain (.h2c): text, 'HS2//COMBICHAIN' then 128 lines of bank * 256 + program (-1 = empty)."""
    lines = data.decode('latin1').split('\n')
    if lines[0].strip() != 'HS2//COMBICHAIN':
        raise ValueError('not a combi chain')
    return [int(x) for x in lines[1:129] if x.strip()]


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    if path.lower().endswith('.h2c'):
        for i, e in enumerate(parse_chain(data)):
            if e >= 0:
                print(f'{i:3d}: bank {e >> 8} combi {e & 0xff}')
        return
    c = parse_combi(fxb_chunk(data))
    if '--json' in sys.argv:
        for p in c['parts']:
            if p:
                p.pop('blob')
                p['params'] = [round(x, 6) for x in p['params']]
        for f in c['global_fx']:
            f['params'] = [round(x, 6) for x in f['params']]
        print(json.dumps(c, indent=1))
        return
    st = c['settings']
    print(f"{os.path.basename(path)}: chunk version {c['version']:#x}, settings version {st['version']:#x} "
          f"'{st.get('name', '')}', master {st['master_volume']:.3f}")
    for k, p in enumerate(c['parts']):
        if p:
            pp = p['params']
            print(f"  part {k + 1:2d}{' link' if k and st['links'][k] else '     '} {p['name']:24s} {p['elements']:2d} el"
                  f"  mute {pp[0]:.2f} level {pp[0x43]:.3f} pan {pp[0x3c]:.2f} out {pp[0x3d]:.3f} fxmode {pp[0x3e]:.1f}"
                  f" sends {pp[0x3f]:.2f}/{pp[0x40]:.2f}/{pp[0x41]:.2f}/{pp[0x42]:.2f}")
        elif k and st['links'][k]:
            print(f'  part {k + 1:2d} link (empty)')
    for i, f in enumerate(c['global_fx']):
        pp = f['params']
        print(f"  FX{i + 1} {f['name']:20s} mute {pp[0]:.0f} type {int(pp[0x13] * 128.9):3d} aux {pp[0x14]:.3f}"
              f" level {pp[0x8b]:.3f} pan {pp[0x8c]:.2f} send1 {pp[0x15]:.3f}")
    for k in ('vel_sens', 'touch_sens', 'memory', 'tune_semis', 'tune_fine', 'hype', 'gm'):
        if k in st:
            print(f'  {k} {st[k]}')


if __name__ == '__main__':
    main()
