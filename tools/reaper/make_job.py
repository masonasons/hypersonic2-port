"""Write a render job (job.tsv) for hs2_render.lua.

Usage: python -I make_job.py [--content DIR] [--out DIR] [--test-sequence NAME] BANKSPEC [BANKSPEC ...]
  BANKSPEC: "7" (all patches of bank 007), "7:0-2" (patches 0..2), or "all" (every factory P bank)

job.tsv lines:  bank<TAB>program<TAB>name   (first line: "#out<TAB><render dir>")
Bank number = the NNN file prefix, sent as CC0=0, CC32=NNN, then program change (see docs/NOTES.md).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'formats'))
import milanbank  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))


def banks(content):
    pdir = os.path.join(content, 'Patches')
    out = {}
    for f in os.listdir(pdir):
        m = re.match(r'(\d{3}) ', f)
        p = os.path.join(pdir, f)
        if m and os.path.isfile(p):
            out[int(m.group(1))] = p
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--content', default=r'C:\Hypersonic 2 Content')
    ap.add_argument('--out', default=os.path.join(ROOT, 'reference', 'renders'))
    ap.add_argument('--job', default=os.path.join(HERE, 'job.tsv'))
    ap.add_argument('spec', nargs='+')
    a = ap.parse_args()

    all_banks = banks(a.content)
    rows = []
    for spec in a.spec:
        if spec == 'all':
            sel = [(b, None) for b in sorted(all_banks)]
        else:
            b, _, rng = spec.partition(':')
            sel = [(int(b), rng or None)]
        for b, rng in sel:
            bank = milanbank.parse(all_banks[b])
            idx = range(len(bank.entries))
            if rng:
                lo, _, hi = rng.partition('-')
                idx = range(int(lo), int(hi or lo) + 1)
            for i in idx:
                rows.append((b, i, bank.entries[i].name))

    os.makedirs(a.out, exist_ok=True)
    with open(a.job, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'#out\t{a.out}\n')
        for b, i, name in rows:
            f.write(f'{b}\t{i}\t{name}\n')
    print(f'{len(rows)} patches -> {a.job}')


if __name__ == '__main__':
    main()
