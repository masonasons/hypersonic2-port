"""Bit-exact regression check between two directories of port renders (same file names).

Usage: python -I regress_identical.py <baseline dir> <new dir>
Exit code 1 if any file differs or is missing. Used to prove that a change leaves existing patches untouched,
e.g. render batch 2 with your build: compare_batch.py reference/renders/batch2 <your hs2render.exe> <your out dir>,
then compare <your out dir> against reference/port/batch3.
"""
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_renders import read_wav  # noqa: E402


def main():
    base, new = sys.argv[1], sys.argv[2]
    bad = 0
    files = sorted(glob.glob(os.path.join(base, '*.wav')))
    for f in files:
        g = os.path.join(new, os.path.basename(f))
        if not os.path.exists(g):
            print('missing:', os.path.basename(f))
            bad += 1
            continue
        a, _, _ = read_wav(f)
        b, _, _ = read_wav(g)
        if a.shape != b.shape or np.any(a != b):
            n = min(len(a), len(b))
            print(f'differs: {os.path.basename(f)} max |diff| {np.abs(a[:n] - b[:n]).max():.3g}')
            bad += 1
    print(f'{len(files) - bad}/{len(files)} identical')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
