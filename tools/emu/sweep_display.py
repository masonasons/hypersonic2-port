"""Sweep every patch/element parameter through Hypersonic's own display code (emulated) and record
value -> text exactly.

Output docs/param_display.json:
  {"patch": {idx: entry}, "element": {"common": {idx: entry}, "type<N>": {idx: entry}}}
  entry = {"name", "kind": "enum", "segments": [[v_from, text], ...]}            (text changes at v_from)
        | {"kind": "numeric", "unit", "values": [1025 floats for v = k/1024]}  (display-rounded)
        | {"kind": "text", "values": [1025 strings]}                            (anything else)
Usage: python -I sweep_display.py <Hypersonic.dll> <docs dir>
"""
import json
import os
import re
import sys

from unicorn import UcError

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hs2emu import Emu, ImportCalled  # noqa: E402

N = 1024
ELEM_DISPLAY, PATCH_DISPLAY = 0x1001ecd0, 0x1000dac0
TYPES = range(7)
NUM_RE = re.compile(r'^\s*([+-]?\d+(?:\.\d+)?)\s*(ms|sec|s|Hz|kHz|%|dB|st|cents?|ct)?\s*$')


def classify(texts, name):
    distinct = []
    for t in texts:
        if not distinct or distinct[-1] != t:
            distinct.append(t)
    nums, units = [], set()
    for t in texts:
        m = NUM_RE.match(t)
        if not m:
            nums = None
            break
        v, u = float(m.group(1)), (m.group(2) or '')
        if u == 'ms':
            v, u = v / 1000, 's'
        elif u == 'sec':
            u = 's'
        elif u == 'kHz':
            v, u = v * 1000, 'Hz'
        nums.append(v)
        units.add(u)
    if len(set(texts)) <= 128 or nums is None and len(set(texts)) <= 256:
        segs, prev = [], None
        for k, t in enumerate(texts):
            if t != prev:
                segs.append([k / N, t])
                prev = t
        if len(segs) <= 128 or nums is None:
            return {'name': name, 'kind': 'enum', 'segments': segs}
    if nums is not None and len(units) == 1:
        return {'name': name, 'kind': 'numeric', 'unit': units.pop(), 'values': nums}
    return {'name': name, 'kind': 'text', 'values': texts}


def main():
    dll, docs = sys.argv[1], sys.argv[2]
    names = json.load(open(os.path.join(docs, 'params.json')))
    emu = Emu(dll)
    obj, buf = emu.alloc(0x4000), emu.alloc(512)
    errors = {}

    def run(fn, args, ecx):
        emu.uc.mem_write(buf, b'\0' * 128)
        try:
            emu.call(fn, args, ecx=ecx)
            return emu.cstr(buf)
        except (UcError, ImportCalled, RuntimeError) as e:
            errors[str(e)] = errors.get(str(e), 0) + 1
            return '<emu error>'

    def sweep_elem(idx, etype):
        emu.uc.mem_write(obj, b'\0' * 0x4000)
        emu.w32(obj + 0x424, etype)
        emu.wf32(obj + 0x18 + 4, (etype + 0.5) / 15.9)  # element type param itself
        out = []
        for k in range(N + 1):
            emu.wf32(obj + 0x18 + idx * 4, k / N)
            out.append(run(ELEM_DISPLAY, (idx, buf), obj))
        return out

    def ename(idx, etype):
        key = f'{idx:#04x}'
        if idx >= 0xac:
            return names['element_mod_matrix'].get(key)
        if 0x11 <= idx <= 0x30:
            return names['element_per_type'].get(str(etype), {}).get(key)
        return names['element_common'].get(key)

    res = {'patch': {}, 'element': {'common': {}}}
    for idx in range(0x100):
        emu.uc.mem_write(obj, b'\0' * 0x4000)
        texts = [run(PATCH_DISPLAY, (0, idx, buf, k / N), obj) for k in range(N + 1)]
        if any(t.strip() for t in texts):
            res['patch'][f'{idx:#04x}'] = classify(texts, names['patch'].get(f'{idx:#04x}'))
    print('patch done', len(res['patch']), flush=True)

    # common element params: sweep with type 2 (Sample); record per-type variants only where they differ
    for idx in range(0x100):
        per = {t: sweep_elem(idx, t) for t in TYPES} if not (0x11 <= idx <= 0x30) else None
        if per is None:
            continue
        base = per[2]
        if any(t.strip() for t in base):
            res['element']['common'][f'{idx:#04x}'] = classify(base, ename(idx, 2))
        for t, texts in per.items():
            if texts != base and any(x.strip() for x in texts):
                res['element'].setdefault(f'type{t}', {})[f'{idx:#04x}'] = classify(texts, ename(idx, t))
    print('element common done', flush=True)
    for t in TYPES:
        for idx in range(0x11, 0x31):
            texts = sweep_elem(idx, t)
            if any(x.strip() for x in texts):
                res['element'].setdefault(f'type{t}', {})[f'{idx:#04x}'] = classify(texts, ename(idx, t))
    print('element per-type done', flush=True)
    res['emu_errors'] = errors
    json.dump(res, open(os.path.join(docs, 'param_display.json'), 'w'), separators=(',', ':'))
    print('errors:', errors)


if __name__ == '__main__':
    main()
