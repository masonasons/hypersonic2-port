"""Record every effect's knob names and value texts with the original's own code (emulated): for each of the 52 effect
types, construct it, set the sample rate, then per knob call its paramName and, for v = k/N, setParam(knob, v) and
paramDisplay. Each knob is swept on a freshly constructed effect, so the other knobs are at their defaults (a display
that depends on another knob, e.g. a delay time shown as a note value when Sync is on, is recorded for the defaults).

Output docs/effect_display.json:
  {"<type>": {"name": effect name, "knobs": [entry or null, ...]}}   (the effect's own knob index 0..; null = unused)
  entry = {"name", "segments": [[k, text], ...]}: the original's text for v = k/N up to the next segment's k (the
          exact display text, run-length coded), plus
          "cases": {"on": knob j, "segments": [[k, entry], ...]} when the knob reads differently depending on switch
          knob j (2..8 texts; one case per text of j, starting at j's sample k)
Usage: python -I sweep_fx_display.py <Hypersonic.dll> <docs dir> [N (default 1024)] [type ...]
"""
import json
import os
import sys

from unicorn import UcError

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hs2emu import Emu, ImportCalled  # noqa: E402


def rle(texts, name):
    segs = []
    for k, t in enumerate(texts):
        if not segs or segs[-1][1] != t:
            segs.append([k, t])
    return {'name': name, 'segments': segs}

TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'engine', 'effects_table.json')
MAX_KNOBS = 12  # Effect objects store 12 knobs (EffectBase p_[12]); the elements forward 0x19..0x24 only


def main():
    dll, docs = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 1024
    only = {int(a) for a in sys.argv[4:]}
    table = json.load(open(TABLE))
    out_path = os.path.join(docs, 'effect_display.json')
    res = json.load(open(out_path)) if only and os.path.exists(out_path) else {}
    errors = {}
    for e in table:
        t = e['id']
        if only and t not in only:
            continue
        emu = Emu(dll)
        obj, buf = emu.alloc(0x400), emu.alloc(512)

        def call(fn, args):
            emu.uc.mem_write(buf, b'\0' * 256)
            try:
                emu.call(fn, args, ecx=obj)
                return emu.cstr(buf)
            except (UcError, ImportCalled, RuntimeError) as ex:
                errors[str(ex)] = errors.get(str(ex), 0) + 1
                return None

        def fresh():  # a new effect object, every knob at its constructor default
            emu.uc.mem_write(obj, b'\0' * 0x400)
            emu.call(int(e['ctor'], 16), (), ecx=obj)
            vt = emu.u32(obj)
            emu.call(emu.u32(vt + 0x04), (44100.0,), ecx=obj)
            return emu.u32(vt + 0x14)

        name_fn, disp_fn = int(e['methods']['paramName'], 16), int(e['methods']['paramDisplay'], 16)
        def sweep(i, preset=None):  # knob i's texts on a fresh object, with preset = (knob, value) set first
            set_param = fresh()
            if preset is not None:
                emu.call(set_param, (preset[0], preset[1]), ecx=obj)
            texts = []
            for k in range(n + 1):
                emu.call(set_param, (i, k / n), ecx=obj)
                txt = call(disp_fn, (i, buf))
                texts.append(txt if txt is not None else '<emu error>')
            return texts

        knobs, raw = [], []
        for i in range(MAX_KNOBS):  # unused knob numbers may sit between used ones
            fresh()
            name = call(name_fn, (i, buf))
            if name is None or not name.strip():
                knobs.append(None)
                raw.append(None)
                continue
            texts = sweep(i)
            raw.append(texts)
            knobs.append(rle(texts, name.strip()))
        # A switch (an enum knob with 2..8 choices: Sync, Mode...) may change how other knobs read: re-sweep every
        # other knob at each of the switch's choices; a knob that reads differently records them all as cases.
        for j, kj in enumerate(knobs):
            if kj is None or not 2 <= len(kj['segments']) <= 8:
                continue
            segs = kj['segments']
            for i, ki in enumerate(knobs):
                if ki is None or i == j or 'cases' in ki:
                    continue
                cases, differs = [], False
                for si, (a, _) in enumerate(segs):
                    b = segs[si + 1][0] if si + 1 < len(segs) else n + 1
                    texts = sweep(i, (j, (a + b) / 2 / n))
                    differs |= texts != raw[i]
                    cases.append([a, rle(texts, ki['name'])])
                if differs:
                    ki['cases'] = {'on': j, 'segments': cases}
                    print(f'    {ki["name"]} depends on {kj["name"]}', flush=True)
        while knobs and knobs[-1] is None:
            knobs.pop()
        res[str(t)] = {'name': e['name'], 'knobs': knobs}
        print(f'{t:3d} {e["name"]}: ' + ', '.join(f'{i + 1} {k["name"]}' for i, k in enumerate(knobs) if k), flush=True)
    res['emu_errors'] = errors
    json.dump(res, open(out_path, 'w'), separators=(',', ':'))
    print('errors:', errors)


if __name__ == '__main__':
    main()
