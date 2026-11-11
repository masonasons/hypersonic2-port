"""Checks of the patch editor and global settings through hs2plughost: patch editor edits (the state's EDIT entries, added
with --state-edit), their state round trip, the global settings (Hype, sensitivities, master tune, preload; --state-attr)
and the Keep Settings / Level Trim host parameters. The plugin reads its usual content folder (patch 15/23 is used).

Build first: build_plugin (-DHS2_BUILD_PLUGIN=ON: HS2Port_VST3, hs2plughost); HS2_BUILD names another build folder.
Usage: python -I plugin/tests/edit_checks.py   (exit code 1 if a check fails)"""
import os
import subprocess
import sys
import tempfile

import numpy as np

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
R = os.path.join(ROOT, os.environ.get('HS2_BUILD', 'build_plugin'), 'plugin')
HOST = os.path.join(R, 'hs2plughost_artefacts', 'Release', 'hs2plughost' + ('.exe' if os.name == 'nt' else ''))
VST = os.path.join(R, 'HS2Port_artefacts', 'Release', 'VST3', 'HS2 Port.vst3')
S = tempfile.mkdtemp(prefix='hs2edit_')
NOTES = '0:1.5:60:100,0.5:1:64:90,1.0:1:67:80'
COMMON = ['--lead', '0.1', '--tail', '1']
bad = 0


def f(n):
    return os.path.join(S, n)


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        print('failed:', args, r.stdout, r.stderr)
        sys.exit(1)
    return r.stdout


def load(p):
    d = open(p, 'rb').read()
    i = d.find(b'data')
    return np.frombuffer(d[i + 8:], '<f4').reshape(-1, 2).astype(np.float64)


def check(name, ok, detail=''):
    global bad
    print(f'{name:58s} {detail}  {"OK" if ok else "FAILED"}')
    if not ok:
        bad += 1


def render(out, *extra, state=None):
    args = [HOST, VST] + (['--state-in', state] if state else ['--bank', '15', '--patch', '23']) + \
        ['--notes', NOTES] + COMMON + ['--out', f(out)] + list(extra)
    run(args)
    return load(f(out))


def diff(a, b):
    n = min(len(a), len(b))
    return float(np.max(np.abs(a[:n] - b[:n]))) if n else 1.0


def peak(a):
    return float(np.max(np.abs(a)))


base = render('base.wav', '--state-out', f('base.bin'))
same = render('base_rest.wav', state=f('base.bin'))
check('baseline state restore: same peak', abs(peak(base) - peak(same)) < 1e-4, f'{peak(base):.4g} / {peak(same):.4g}')

# Mute every element (element parameter 0 = Mute) -> silence
mute = ','.join(f'1:{e}:0=1' for e in range(1, 17))
m = render('mute.wav', '--state-edit', mute, '--state-out', f('mute.bin'), state=f('base.bin'))
check('elements muted by edits: silent', peak(m) < 1e-4 * peak(base), f'peak {peak(m):.3g} vs {peak(base):.3g}')
m2 = render('mute_rest.wav', state=f('mute.bin'))
check('muted edits saved and restored', diff(m, m2) == 0, f'max diff {diff(m, m2):.3g}')

# Octave +1 on element 1 (parameter 70): sound changes, not silent; restored identically
octv = render('oct.wav', '--state-edit', '1:1:70=0.7', '--state-out', f('oct.bin'), state=f('base.bin'))
check('element 1 Octave edit changes the sound', diff(octv, same) > 1e-3 and peak(octv) > 0.1 * peak(base),
      f'diff {diff(octv, same):.3g}, peak {peak(octv):.3g}')
o2 = render('oct_rest.wav', state=f('oct.bin'))
check('Octave edit saved and restored', diff(octv, o2) == 0, f'max diff {diff(octv, o2):.3g}')

# An edit to a patch parameter (elem 0) and a patch send effect (elem -1, j 56 + 18 = send effect 1 Level)
pe = render('patch.wav', '--state-edit', '1:-1:74=0,1:-1:98=0,1:-1:122=0,1:-1:146=0', '--state-out', f('pe.bin'),
            state=f('base.bin'))
pe2 = render('patch_rest.wav', state=f('pe.bin'))
check('send effect level edits restored identically', diff(pe, pe2) == 0, f'diff vs unedited {diff(pe, same):.3g}')

# Global settings
for attr, val, expect_same in [('hype', '0.33', True), ('velocitySensitivity', '0.5', True),
                               ('tuneSemitones', '0', True), ('tuneSemitones', '12', False),
                               ('tuneCents', '-50', False), ('hype', '1', False),
                               ('velocitySensitivity', '0', False), ('preloadSamples', '1', True)]:
    a = render(f'set_{attr}_{val}.wav', '--state-attr', f'{attr}={val}', '--state-out', f(f'set_{attr}_{val}.bin'),
               state=f('base.bin'))
    d = diff(a, same)
    check(f'{attr}={val} {"same as default" if expect_same else "changes the sound"}',
          (d == 0) if expect_same else (d > 1e-3), f'diff {d:.3g}')
    if not expect_same:
        a2 = render(f'set_{attr}_{val}_rest.wav', state=f(f'set_{attr}_{val}.bin'))
        check(f'  {attr}={val} saved and restored', diff(a, a2) == 0, f'max diff {diff(a, a2):.3g}')

# Keep Settings / Level Trim host parameters
lt = render('trim.wav', '--param', 'Part 1 Level Trim=0', state=f('base.bin'))
check('Part 1 Level Trim = 0 changes the level', peak(lt) < peak(base), f'peak {peak(lt):.3g} vs {peak(base):.3g}')
out = run([HOST, VST, '--bank', '15', '--patch', '23', '--notes', NOTES] + COMMON + ['--out', f('dump.wav'), '--dump-params'])
check('host lists Keep Settings and Level Trim', 'Keep Settings' in out and 'Level Trim' in out, '')

print('FAILED: %d' % bad if bad else 'all OK')
sys.exit(1 if bad else 0)
