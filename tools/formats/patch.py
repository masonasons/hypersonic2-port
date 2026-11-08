"""Hypersonic 2 patch ("StSS" blob) parser.

Layout (from FUN_10016f70, the patch loader; see docs/NOTES.md):
  0x000 u32 magic 'StSS' (also accepted at +0x3c, i.e. inside a VST fxp/fxb wrapper)
  0x004 u32 version (e.g. 0x990EDF). Loader fixups apply below 0xA87569 and 0xBEC874C.
  0x010 u32 ?            0x014 u32 element count (inserts included)
  0x018 u8[4] send-FX slot present flags (non-zero = a record follows the elements)
  0x020 char[0x20] name   0x040 char[0x40] short name   0x080 char[0x80] description
  0x100 char[0x80] controller notes ("MW: ...|AT : ...")  0x180 char[0x40] author
  0x1c0 char[6][0x10] quick-control (Hyperknob) names
  0x220 f32[0x100] patch parameters
  0x620 records[count + n_send_fx], 0x420 bytes each:
        char[0x20] record name, f32[0x100] parameters (element or effect)
  then 12 trailing bytes (most patches; ~50 factory patches carry more, TBD)
Element param 1 = type: trunc(v * 15.9) -> 0 Effect, 1 Drawbar, 2 Sample, 3 Wavetable, 4 FM, 5 Analog, 6 Model.
Element params 0x11..0x30 are type-specific; >= 0xac is the 16-slot mod matrix.
"""
import json
import os
import struct

ELEMENT_TYPES = ['Effect', 'Drawbar', 'Sample', 'Wavetable', 'FM', 'Analog', 'Model']
_HERE = os.path.dirname(os.path.abspath(__file__))
_NAMES_PATH = os.path.join(_HERE, '..', '..', 'docs', 'params.json')
_names = None


def names():
    global _names
    if _names is None:
        _names = json.load(open(_NAMES_PATH))
    return _names


def _cstr(b):
    return b.split(b'\0')[0].decode('latin1')


def element_type(v):
    return int(v * 15.9)  # MSVC _ftol truncates


def param_name(level, idx, etype=None):
    n = names()
    key = f'{idx:#04x}'
    if level == 'patch':
        return n['patch'].get(key)
    if idx >= 0xac:
        return n['element_mod_matrix'].get(key)
    if 0x11 <= idx <= 0x30:
        return n['element_per_type'].get(str(etype), {}).get(key)
    return n['element_common'].get(key)


def _named(values, level, etype=None):
    out = {}
    for i, v in enumerate(values):
        nm = param_name(level, i, etype)
        out[f'{i:#04x}' + (f' {nm}' if nm else '')] = round(v, 6)
    return out


def parse(blob):
    d = blob
    if d[:4] != b'StSS' and d[0x3c:0x40] == b'StSS':
        d = d[0x3c:]
    if d[:4] != b'StSS':
        raise ValueError('not a StSS patch')
    version, _r8, _rc, u10, count = struct.unpack_from('<IIIII', d, 4)
    fx_flags = list(d[0x18:0x1c])
    p = {
        'version': version, 'u10': u10, 'element_count': count, 'send_fx_flags': fx_flags,
        'name': _cstr(d[0x20:0x40]), 'short_name': _cstr(d[0x40:0x80]),
        'description': _cstr(d[0x80:0x100]), 'controllers': _cstr(d[0x100:0x180]),
        'author': _cstr(d[0x180:0x1c0]),
        'quick_controls': [_cstr(d[0x1c0 + 0x10 * i:0x1d0 + 0x10 * i]) for i in range(6)],
        'params': list(struct.unpack_from('<256f', d, 0x220)),
        'elements': [], 'send_fx': [],
    }
    o = 0x620
    for i in range(count + sum(1 for f in fx_flags if f)):
        if o + 0x420 > len(d):
            p['truncated'] = True
            break
        rec = {'name': _cstr(d[o:o + 0x20]), 'params': list(struct.unpack_from('<256f', d, o + 0x20))}
        rec['type'] = ELEMENT_TYPES[element_type(rec['params'][1])] if i < count else 'SendFX'
        (p['elements'] if i < count else p['send_fx']).append(rec)
        o += 0x420
    p['trailer'] = d[o:].hex()
    return p


def to_json(p):
    """Readable form: parameters keyed by index + name."""
    q = {k: v for k, v in p.items() if k not in ('params', 'elements', 'send_fx')}
    q['params'] = _named(p['params'], 'patch')
    q['elements'] = [{'name': e['name'], 'type': e['type'],
                      'params': _named(e['params'], 'element', element_type(e['params'][1]))}
                     for e in p['elements']]
    q['send_fx'] = [{'name': e['name'], 'params': {f'{i:#04x}': round(v, 6) for i, v in enumerate(e['params'])}}
                    for e in p['send_fx']]
    return q


if __name__ == '__main__':
    import sys
    sys.path.insert(0, _HERE)
    import milanbank
    bank = milanbank.parse(sys.argv[1])
    idx = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    print(json.dumps(to_json(parse(bank.read(bank.entries[idx].slot1))), indent=1))
