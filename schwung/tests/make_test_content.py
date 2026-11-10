"""Write a small synthetic Hypersonic 2 content folder for the module's host test (no Steinberg data):

  Patches/000 Test Patches   a patch bank (Milan bank, kind 1) with
      0 "Test Analog"    an Analog element + an insert Delay (effect 10), Hyperknob names, arp off
      1 "Test Sample"    a Sample element pointing at a sample bank that is not there (loads, plays nothing)
  Patches/001 More Tests     one patch, "Second Bank", for bank stepping
  Effects/Delay/Test Delay.fxp   an effect preset (1252 bytes: type at +0x130, 12 knobs at +0x148)

Formats: core/src/bank.cpp (MilanBank::open), core/src/patch.cpp (Patch::parse), PatchLibrary.cpp (EffectPreset).

Usage: python3 -I schwung/tests/make_test_content.py <out folder>
"""
import os
import struct
import sys


def patch_blob(name, knob_names, elements, params):
    count = len(elements)
    head = bytearray(0x620)
    head[0:4] = b'StSS'
    struct.pack_into('<I', head, 4, 0x0BEC874D)  # newer than the loader fixups
    struct.pack_into('<I', head, 0x14, count)
    head[0x20:0x20 + len(name)] = name.encode('latin1')
    for i, k in enumerate(knob_names):
        head[0x1c0 + 0x10 * i:0x1c0 + 0x10 * i + len(k)] = k.encode('latin1')
    p = [0.0] * 256
    for k, v in params.items():
        p[k] = v
    struct.pack_into('<256f', head, 0x220, *p)
    out = bytes(head)
    for rec_name, rp in elements:
        r = bytearray(0x420)
        r[0:len(rec_name)] = rec_name.encode('latin1')
        e = [0.0] * 256
        for k, v in rp.items():
            e[k] = v
        struct.pack_into('<256f', r, 0x20, *e)
        out += bytes(r)
    return out + bytes(12)


def milan_bank(entries, letter=b'P', kind=1):
    """entries: [(name, blob)]. Version 12 (offsets not masked)."""
    kdir = 0x58
    n = len(entries)
    dir_size = 0x10 + n * 0x30
    names = bytearray()
    name_offs = []
    for nm, _ in entries:
        name_offs.append(len(names))
        b = nm.encode('latin1')
        names += bytes([len(b)]) + b
    names_off = dir_size  # relative to kdir
    data_start = kdir + dir_size + len(names)
    data_start = (data_start + 15) & ~15
    out = bytearray(data_start)
    out[0:3] = b'Mil'
    out[16] = letter[0]
    out[0x29] = ord('W')
    struct.pack_into('<HH', out, 0x30, kind, 12)
    struct.pack_into('<IIII', out, kdir, n, names_off, 0, 0)
    blobs = bytearray()
    for i, (nm, blob) in enumerate(entries):
        e = kdir + 0x10 + i * 0x30
        off = data_start + len(blobs)
        struct.pack_into('<II', out, e, name_offs[i], name_offs[i])
        struct.pack_into('<III', out, e + 0x0c, 1, off, len(blob))
        blobs += blob
    out[kdir + dir_size:kdir + dir_size + len(names)] = names
    out += blobs
    if len(out) < 0x80:
        out += bytes(0x80 - len(out))
    return bytes(out)


def common_patch_params():
    return {
        0x31: 0.0, 0x32: 1.0, 0x33: 0.0, 0x34: 0.0, 0x35: 1.0, 0x36: 0.0,  # key / velocity ranges
        0x37: 0.5, 0x38: 0.5,                                               # transpose, fine tune
        0x3a: 1.0, 0x3b: 0.5, 0x3c: 0.5, 0x3d: 1.5 / 32.9, 0x43: 0.8,        # voices, trim, pan, output, level
        0x0e: 0.5, 0x0f: 0.5, 0x67: 0.5, 0x69: 0.5, 0x6b: 0.5, 0x6d: 0.5,   # Hyperknobs
        0x13: 0.05, 0x14: 0.1, 0x16: 0.9, 0x17: 0.65, 0x18: 0.5, 0x19: 0.0,  # arp (off)
        0x49: 2.5 / 15.9, 0x4a: 2.5 / 15.9, 0x94: 0.2,
    }


def voice_element(etype, extra):
    e = {
        0x01: (etype + 0.5) / 15.9, 0x31: 0.0, 0x32: 1.0, 0x34: 0.0, 0x35: 1.0, 0x3a: 1.0,
        0x46: 0.5, 0x47: 0.5, 0x48: 0.5, 0x49: 0.75,
        0x67: 0.0, 0x6a: 1.0, 0x7e: 0.0, 0x7f: 0.3, 0x80: 1.0, 0x82: 0.2,
        0x8a: 1.0, 0x8b: 0.7, 0x8c: 0.5,
        0x91: 0.5 / 7.9, 0x92: 1.5 / 7.9, 0x04: 0.1, 0x06: 0.5, 0x07: 0.6,
        0xac: 0.03, 0xaf: 0.15, 0xb0: 0.7,  # a mod-matrix row
    }
    e.update(extra)
    return e


def main():
    out = sys.argv[1]
    os.makedirs(os.path.join(out, 'Patches'), exist_ok=True)
    os.makedirs(os.path.join(out, 'Samples'), exist_ok=True)
    os.makedirs(os.path.join(out, 'Effects', 'Delay'), exist_ok=True)
    analog = voice_element(5, {0x13: 1.5 / 7.9, 0x15: 0.8, 0x21: 0.5})
    delay = {0x01: 0.0, 0x13: 10.5 / 128.9, 0x19: 0.3, 0x1a: 0.4, 0x1b: 0.5, 0x1c: 0.3, 0x8b: 0.7, 0x8c: 0.5}
    sample = voice_element(2, {0x12: 3.5 / 128.9, 0x13: 0.5 / 128.9})
    p0 = patch_blob('[Test Analog', ['Thump', 'Bright', 'Space', '', 'Drive', 'Wide'],
                    [('Osc', analog), ('Delay', delay)], common_patch_params())
    p1 = patch_blob('Test Sample', ['', '', '', '', '', ''], [('Piano L', sample)], common_patch_params())
    p2 = patch_blob('Second Bank', ['A', 'B', 'C', 'D', 'E', 'F'], [('Osc', analog)], common_patch_params())
    with open(os.path.join(out, 'Patches', '000 Test Patches'), 'wb') as f:
        f.write(milan_bank([('[Test Analog', p0), ('Test Sample', p1)]))
    with open(os.path.join(out, 'Patches', '001 More Tests'), 'wb') as f:
        f.write(milan_bank([('Second Bank', p2)]))
    fxp = bytearray(0x4e4)
    struct.pack_into('<f', fxp, 0x130, 11.5 / 128.9)  # Stereo Delay
    struct.pack_into('<12f', fxp, 0x148, *([0.4] * 12))
    with open(os.path.join(out, 'Effects', 'Delay', 'Test Delay.fxp'), 'wb') as f:
        f.write(bytes(fxp))
    print(f'wrote test content to {out}')


if __name__ == '__main__':
    main()
