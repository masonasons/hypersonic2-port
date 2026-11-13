#!/usr/bin/env python3
"""List the Syncrosoft copy-protected (encrypted) patches in a Hypersonic 2 content folder.

The port cannot read the stored form: their data starts with "SyncrosoftContent" instead of "StSS" (docs/NOTES.md,
"not a StSS patch"). They load from captures taken by tools/hs2capture (<content>/Captures/patches). Writes a stand-alone Markdown file with each patch's bank number, bank name, index and name.

    python3 -I tools/formats/list_protected.py "<Hypersonic 2 Content>" [-o protected_patches.md]
                                               [--tsv patches.tsv [--all]]

--tsv also writes the patches as "bank<TAB>index<TAB>name" lines, the list tools/hs2capture reads; with --all it lists
every patch, not only the protected ones.
"""
import argparse
import datetime
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import milanbank  # noqa: E402

TAG = b'Syncrosoft'


def md_cell(s):
    return (s or '').replace('|', '\\|').strip()


def scan(content):
    pdir = os.path.join(content, 'Patches')
    banks, rows, every = [], [], []
    for fn in sorted(os.listdir(pdir)):
        path = os.path.join(pdir, fn)
        if not os.path.isfile(path) or os.path.splitext(fn)[1] or len(fn) < 4 or not fn[:3].isdigit():
            continue
        try:
            bank = milanbank.parse(path)
        except Exception as e:  # not a bank: noted, skipped
            print(f'skipped {fn}: {e}', file=sys.stderr)
            continue
        number, title = int(fn[:3]), fn[3:].strip()
        total = protected = 0
        for e in bank.entries:
            if not e.slot1.size:
                continue
            total += 1
            every.append((number, title, e.index, (e.name or '').removeprefix('[')))
            if bank.read(e.slot1)[:len(TAG)] == TAG:
                protected += 1
                rows.append((number, title, e.index, (e.name or '').removeprefix('[')))
        banks.append((number, title, total, protected))
    return banks, rows, every


def write(out, content, banks, rows):
    total = sum(b[2] for b in banks)
    lines = [
        '# Hypersonic 2: copy-protected patches',
        '',
        f'{len(rows)} of the {total} patches in {len(banks)} patch banks are stored Syncrosoft copy-protected '
        '(encrypted). The port does not decrypt them; they load in the plugin and the Move modules from captures '
        'taken by tools/hs2capture (<content>/Captures/patches).',
        '',
        f'Made by `tools/formats/list_protected.py` on {datetime.date.today().isoformat()}, from the content '
        f'folder "{os.path.basename(os.path.normpath(content))}". Index is the patch\'s position in its bank, '
        'counting from 0. In the bank files these names start with "[" (as do many unprotected patches); it is '
        'left out here.',
        '',
        '## By bank',
        '',
        '| Bank | Bank name | Protected | Patches in bank |',
        '|---|---|---|---|',
    ]
    for number, title, n, p in banks:
        if p:
            lines.append(f'| {number:03d} | {md_cell(title)} | {p} | {n} |')
    lines += ['', '## Patches', '', '| Bank | Bank name | Index | Patch |', '|---|---|---|---|']
    for number, title, index, name in rows:
        lines.append(f'| {number:03d} | {md_cell(title)} | {index} | {md_cell(name)} |')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('content', help='the Hypersonic 2 Content folder (with Patches/ inside)')
    ap.add_argument('-o', '--out', default='protected_patches.md')
    ap.add_argument('--tsv', help='also write the list as bank<TAB>index<TAB>name lines (for tools/hs2capture)')
    ap.add_argument('--all', action='store_true', help='--tsv lists every patch, not only the protected ones')
    a = ap.parse_args()
    banks, rows, every = scan(a.content)
    write(a.out, a.content, banks, rows)
    if a.tsv:
        with open(a.tsv, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# bank\tindex\tname (%s)\n' % ('every patch' if a.all else 'copy-protected patches'))
            for number, _, index, name in (every if a.all else rows):
                f.write('%d\t%d\t%s\n' % (number, index, name.replace('\t', ' ')))
        print(f'{len(every if a.all else rows)} patches -> {a.tsv}')
    print(f'{len(rows)} protected patches in {sum(1 for b in banks if b[3])} banks -> {a.out}')


if __name__ == '__main__':
    main()
