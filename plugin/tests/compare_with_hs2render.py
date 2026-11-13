"""Plugin regression test: the built VST3, loaded by hs2plughost like a DAW, must render exactly what hs2render
(MSVC build in build_msvc, same engine sources, engine mode) renders. hs2render is itself verified against the
original (tools/emu/part_oracle.py, combi_oracle.py).

Covers: a patch loaded by MIDI bank select + program change into part 1 (replacing the new instance's default patch 84/0) and
into part 2; a combi loaded by MIDI; the same with the program change sent the VST3 way (bank select as MIDI, the
program through the plugin's program list parameter, as REAPER does); state save and restore of each (also a state
saved before the host parameters caught up); 48 kHz with 512-frame blocks; a Hyperknob and a part level moved by the
host; the ring-out setting off (the original's behaviour) and on (the plugin's default; hs2render --ring-out); a load
while the old patch sounds (cut-on-load, the plugin's default: hs2render --cut-on-load).

Build first: build_msvc (hs2render) and build_plugin (-DHS2_BUILD_PLUGIN=ON: HS2Port_VST3, hs2plughost).
Usage: python -I plugin/tests/compare_with_hs2render.py   (exit code 1 if anything differs)"""
import os
import subprocess
import sys
import tempfile

import numpy as np

S = tempfile.mkdtemp(prefix='hs2plugtest_')
# HS2_BUILD: the plugin build folder to test (default build_plugin; build_win7 is the JUCE 7 / Windows 7 build)
BUILD = os.path.join(r'C:\stuff\Hypersonic 2', os.environ.get('HS2_BUILD', 'build_plugin'))
HOST = os.path.join(BUILD, 'plugin', 'hs2plughost_artefacts', 'Release', 'hs2plughost.exe')
VST = os.path.join(BUILD, 'plugin', 'HS2Port_artefacts', 'Release', 'VST3', 'HS2 Port.vst3')
REN = r'C:\stuff\Hypersonic 2\build_msvc\tools\cli\hs2render.exe'
CDIR = r'C:\Hypersonic 2 Content\Patches\120 Combis'
COMMON = ['--lead', '0.1', '--tail', '1']
RO = ['--ring-out', '--cut-on-load']  # the plugin's defaults: tails ring out, a load stops the old patch
bad = 0


def load(p):
    d = open(p, 'rb').read()
    i = d.find(b'data')
    return np.frombuffer(d[i + 8:], '<f4').reshape(-1, 2).astype(np.float64)


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f'failed: {args}\n{r.stdout}\n{r.stderr}')
    return r.stdout


def compare(name, plug, ref):
    global bad
    a, b = load(plug), load(ref)
    n = min(len(a), len(b))
    d = np.abs(a[:n] - b[:n]).max()
    pk = np.abs(b[:n]).max() + 1e-30
    ok = d == 0 and len(a) == len(b) and pk > 1e-4
    bad += not ok
    print(f'{name:44s} frames {len(a)}/{len(b)}  max|diff| {d:.3g} ({20 * np.log10(d / pk + 1e-30):.1f} dB re peak '
          f'{pk:.3f})  {"IDENTICAL" if ok else "DIFFERS" if d else "SILENT OR LENGTH"}')


def combi_file(i):
    for n in os.listdir(CDIR):
        if n.endswith('.fxb') and int(n[:3]) == i:
            return os.path.join(CDIR, n)
    raise SystemExit(f'no combi {i}')


def f(name):
    return os.path.join(S, name)


NOTES = '0:1.5:60:100,0.5:1:64:90,1.0:1:67:80'
# 1. Patches into part 1 by MIDI (a new instance holds the default patch 84/0 in part 1), then the saved state restored.
# The last three are copy-protected patches (the "New" HS-2 additions): the plugin and hs2render both load their
# captures from <content>\Captures\patches, so these test that path (program change, state restore).
for b, p in [(31, 0), (9, 0), (15, 23), (83, 1), (0, 3), (13, 4), (10, 10), (37, 91), (82, 0), (82, 14), (3, 65),
             (8, 30), (17, 25), (33, 42)]:
    tag = f'{b}_{p}'
    run([REN, '--part', '1:84:0', '--load-bank', '--skip', '32', '--events', f'@32:load:1:{b}:{p}', '--notes', NOTES]
        + RO + COMMON + ['--host', '-1:120:0', '--block', '32', '--out', f(f'ref_pc_{tag}.wav')])
    run([REN, '--part', f'1:{b}:{p}', '--load-bank', '--notes', NOTES] + RO + COMMON +
        ['--host', '-1:120:0', '--block', '32', '--out', f(f'ref_st_{tag}.wav')])
    run([HOST, VST, '--bank', str(b), '--patch', str(p), '--notes', NOTES] + COMMON +
        ['--out', f(f'plug_{tag}.wav'), '--state-out', f(f'state_{tag}.bin')])
    run([HOST, VST, '--state-in', f(f'state_{tag}.bin'), '--notes', NOTES] + COMMON + ['--out', f(f'rest_{tag}.wav')])
    compare(f'{tag} program change, part 1', f(f'plug_{tag}.wav'), f(f'ref_pc_{tag}.wav'))
    compare(f'{tag} state restore', f(f'rest_{tag}.wav'), f(f'ref_st_{tag}.wav'))

# 2. A patch into part 2 (MIDI channel 2) next to the default patch in part 1; notes on both channels.
N2 = '0:1.5:60:100:2,0.5:1:64:90:2,0.2:1:48:80:1'
run([REN, '--part', '1:84:0', '--load-bank', '--skip', '32', '--events', '@32:load:2:15:23', '--notes', N2] + RO + COMMON +
    ['--host', '-1:120:0', '--block', '32', '--out', f('ref_part2.wav')])
run([REN, '--part', '1:84:0,2:15:23', '--load-bank', '--notes', N2] + RO + COMMON +
    ['--host', '-1:120:0', '--block', '32', '--out', f('ref_part2_st.wav')])
run([HOST, VST, '--channel', '2', '--bank', '15', '--patch', '23', '--notes', N2] + COMMON +
    ['--out', f('plug_part2.wav'), '--state-out', f('state_part2.bin')])
run([HOST, VST, '--state-in', f('state_part2.bin'), '--notes', N2] + COMMON + ['--out', f('rest_part2.wav')])
compare('15/23 into part 2, program change on channel 2', f('plug_part2.wav'), f('ref_part2.wav'))
compare('parts 1 and 2, state restore', f('rest_part2.wav'), f('ref_part2_st.wav'))

# 3. Combis by MIDI (bank select 120), notes on several channels; then the saved state restored.
NC = '0:1.2:60:100:1,0.1:1.2:48:90:2,0.2:1.2:64:100:3,0.3:1.2:36:100:4,0.6:0.8:67:110:5,0.7:0.8:55:90:6'
for ci in (3, 22, 44):
    run([REN, '--part', '1:84:0', '--load-bank', '--skip', '32', '--events', f'@32:combi:{ci}', '--notes', NC] +
        RO + COMMON + ['--host', '-1:120:0', '--block', '32', '--out', f(f'ref_c{ci}.wav')])
    run([REN, '--combi', combi_file(ci), '--notes', NC] + RO + COMMON +
        ['--host', '-1:120:0', '--block', '32', '--out', f(f'ref_c{ci}_st.wav')])
    run([HOST, VST, '--bank', '120', '--patch', str(ci), '--notes', NC] + COMMON +
        ['--out', f(f'plug_c{ci}.wav'), '--state-out', f(f'state_c{ci}.bin')])
    run([HOST, VST, '--state-in', f(f'state_c{ci}.bin'), '--notes', NC] + COMMON + ['--out', f(f'rest_c{ci}.wav')])
    compare(f'combi 120/{ci} by program change', f(f'plug_c{ci}.wav'), f(f'ref_c{ci}.wav'))
    compare(f'combi 120/{ci} state restore', f(f'rest_c{ci}.wav'), f(f'ref_c{ci}_st.wav'))

# 4. Host-like block size and rate.
for b, p in [(15, 23), (9, 0), (37, 91)]:
    run([REN, '--part', '1:84:0', '--load-bank', '--skip', '512', '--events', f'@512:load:1:{b}:{p}', '--notes', NOTES]
        + RO + COMMON + ['--sr', '48000', '--host', '-1:120:0', '--block', '512', '--out', f(f'r48_{b}_{p}.wav')])
    run([HOST, VST, '--bank', str(b), '--patch', str(p), '--notes', NOTES] + COMMON +
        ['--sr', '48000', '--block', '512', '--out', f(f'p48_{b}_{p}.wav')])
    compare(f'{b}/{p} 48 kHz, 512 blocks', f(f'p48_{b}_{p}.wav'), f(f'r48_{b}_{p}.wav'))

# 5. State saved before the host parameters caught up (no message loop at all), then restored.
run([HOST, VST, '--bank', '15', '--patch', '23', '--notes', NOTES] + COMMON +
    ['--out', f('nopump.wav'), '--state-out', f('state_nopump.bin'), '--no-pump'])
run([HOST, VST, '--state-in', f('state_nopump.bin'), '--notes', NOTES] + COMMON + ['--out', f('nopump_rest.wav')])
compare('state saved without message loop, restored', f('nopump_rest.wav'), f('ref_st_15_23.wav'))

# 6. Host parameters reach the engine: a Hyperknob, a part level and two of the part settings (Transpose, Arpeggiator);
# each also survives a state round trip.
for name, arg in [('Hyperknob 1 = 0.95', ['--knob', '1=0.95']), ('Part 1 Level = 0.3', ['--param', 'Part 1 Level=0.3']),
                  ('Part 1 Transpose = +12', ['--param', 'Part 1 Transpose=0.665']),
                  ('Part 1 Arpeggiator = ON', ['--param', 'Part 1 Arpeggiator=0.75'])]:
    out, st, rest = f('param.wav'), f('param.bin'), f('param_rest.wav')
    run([HOST, VST, '--bank', '15', '--patch', '23', '--notes', NOTES] + COMMON + ['--out', out, '--state-out', st] + arg)
    a, ref = load(out), load(f('ref_pc_15_23.wav'))
    n = min(len(a), len(ref))
    d = np.abs(a[:n] - ref[:n]).max()
    ok = d > 1e-3 and np.abs(a).max() > 1e-3
    bad += not ok
    print(f'{name} on 15/23: differs from default by {d:.3g}, peak {np.abs(a).max():.3f}  {"OK" if ok else "NO EFFECT"}')
    # restored with the changed value from the start: must differ from the plain restore the same way
    run([HOST, VST, '--state-in', st, '--notes', NOTES] + COMMON + ['--out', rest])
    r = load(rest)
    d2 = np.abs(r[:n] - load(f('ref_st_15_23.wav'))[:n]).max()
    ok = d2 > 1e-3
    bad += not ok
    print(f'{name} restored from the state: differs from default by {d2:.3g}  {"OK" if ok else "LOST"}')

# 7. Program changes the VST3 way (--pc-param): patches into part 1 and part 2, a combi.
for b, p, ch, notes in [(15, 23, 1, NOTES), (39, 2, 1, NOTES), (9, 0, 2, N2), (120, 22, 1, NC)]:
    tag = f'vst3pc_{b}_{p}_{ch}'
    ev = f'@64:combi:{p}' if b >= 120 else f'@64:load:{ch}:{b}:{p}'
    run([REN, '--part', '1:84:0', '--load-bank', '--skip', '64', '--events', ev, '--notes', notes] + RO + COMMON +
        ['--host', '-1:120:0', '--block', '32', '--out', f(f'ref_{tag}.wav')])
    run([HOST, VST, '--pc-param', '--channel', str(ch), '--bank', str(b), '--patch', str(p), '--notes', notes] + COMMON +
        ['--out', f(f'plug_{tag}.wav')])
    compare(f'{b}/{p} part {ch}, VST3 program parameter', f(f'plug_{tag}.wav'), f(f'ref_{tag}.wav'))

# 7b. Program changes on several channels at once (as a MIDI file with bank / program changes on every channel at the
# start), through the per-channel program lists of the patched JUCE.
MULTI = [(1, 15, 13), (2, 35, 5), (3, 35, 0), (4, 39, 10), (10, 3, 22)]
NM = '0:1.2:48:100:1,0.1:1.2:60:90:2,0.2:1.2:64:100:3,0.3:1.2:72:100:4,0.4:0.6:36:110:10,0.6:0.6:42:100:10'
ev = ','.join(f'@64:load:{c}:{b}:{p}' for c, b, p in MULTI)
run([REN, '--part', '1:84:0', '--load-bank', '--skip', '64', '--events', ev, '--notes', NM] + RO + COMMON +
    ['--host', '-1:120:0', '--block', '32', '--out', f('ref_multi.wav')])
run([HOST, VST, '--pc-channels', ','.join(f'{c}:{b}:{p}' for c, b, p in MULTI), '--notes', NM] + COMMON +
    ['--out', f('plug_multi.wav')])
compare('5 channels at once, per-channel program lists', f('plug_multi.wav'), f('ref_multi.wav'))

# 7c. Volume, pan and a send in the same block as the program change (a MIDI file's start): they go to the old part
# at once (as any MIDI) and again to the new patch when it is in place.
for b, p in [(15, 23), (39, 2)]:
    run([REN, '--part', '1:84:0', '--load-bank', '--skip', '32', '--events',
         f'@0:cc:7:40,@0:cc:10:20,@0:cc:91:100,@32:load:1:{b}:{p},@32:cc:7:40,@32:cc:10:20,@32:cc:91:100',
         '--notes', NOTES] + RO + COMMON +
        ['--host', '-1:120:0', '--block', '32', '--out', f(f'ref_initcc_{b}_{p}.wav')])
    run([HOST, VST, '--bank', str(b), '--patch', str(p), '--init-cc', '7=40,10=20,91=100', '--notes', NOTES] + COMMON +
        ['--out', f(f'plug_initcc_{b}_{p}.wav')])
    compare(f'{b}/{p} with CC7/10/91 at the program change', f(f'plug_initcc_{b}_{p}.wav'), f(f'ref_initcc_{b}_{p}.wav'))
# The same with the program change the VST3 way (it reaches the plugin after the controllers, as in REAPER).
run([REN, '--part', '1:84:0', '--load-bank', '--skip', '64', '--events',
     '@0:cc:7:40,@0:cc:10:20,@0:cc:93:0,@64:load:1:39:28,@64:cc:7:40,@64:cc:10:20,@64:cc:93:0',
     '--notes', NOTES] + RO + COMMON + ['--host', '-1:120:0', '--block', '32', '--out', f('ref_initcc_pc.wav')])
run([HOST, VST, '--pc-param', '--bank', '39', '--patch', '28', '--init-cc', '7=40,10=20,93=0', '--notes', NOTES] + COMMON +
    ['--out', f('plug_initcc_pc.wav')])
compare('39/28 with CC7/10/93 before a VST3 program change', f('plug_initcc_pc.wav'), f('ref_initcc_pc.wav'))

# 8. Ring-out off and on through the state, on 39/2 "Soloing" (when off, its delay tail is cut as in the original).
SOLO = '0:0.25:72:100'
run([HOST, VST, '--bank', '39', '--patch', '2', '--notes', SOLO] + COMMON +
    ['--out', f('solo.wav'), '--state-out', f('solo.bin')])
for on in (False, True):
    run([HOST, VST, '--state-in', f('solo.bin'), '--state-attr', f'ringOut={int(on)}', '--notes', SOLO, '--lead', '0.1',
         '--tail', '4',
         '--out', f(f'solo_rest_{on}.wav')])
    run([REN, '--part', '1:39:2', '--load-bank', '--notes', SOLO, '--lead', '0.1', '--tail', '4', '--host', '-1:120:0',
         '--block', '32', '--out', f(f'solo_ref_{on}.wav')] + (['--ring-out'] if on else []) + ['--cut-on-load'])
    compare(f'39/2 ring-out {"on " if on else "off"}, state restore', f(f'solo_rest_{on}.wav'), f(f'solo_ref_{on}.wav'))
a, b = load(f('solo_rest_False.wav')), load(f('solo_rest_True.wav'))
late = slice(int(1.0 * 44100), int(3.0 * 44100))
ok = np.abs(a[late]).max() == 0 and np.abs(b[late]).max() > 1e-3
bad += not ok
print(f'39/2 tail 1-3 s: ring-out off peak {np.abs(a[late]).max():.3g}, on peak {np.abs(b[late]).max():.3g}  '
      f'{"OK" if ok else "UNEXPECTED"}')

# 9. The cut itself (engine, hs2render): a long Analogy note from 0, then 39/2 loaded at 0.5 s. With cut-on-load the
# old patch fades out in 10 ms and the new one gets no note, so the rest is silent; without, Analogy sounds on.
NH = '0:2:48:110'
for cut in (True, False):
    run([REN, '--part', '1:31:0', '--load-bank', '--events', '@22050:load:1:39:2', '--notes', NH, '--lead', '0',
         '--tail', '1', '--host', '-1:120:0', '--block', '32', '--out', f(f'cut_{cut}.wav')] +
        (['--ring-out', '--cut-on-load'] if cut else ['--ring-out']))
a, b = load(f('cut_True.wav')), load(f('cut_False.wav'))
after = slice(22050 + 32 + 441 + 64, None)
ok = np.abs(a[:22050]).max() > 1e-3 and np.abs(a[after]).max() < 1e-4 < np.abs(b[after]).max()
bad += not ok
print(f'load while a note sounds: after the load, peak with cut-on-load {np.abs(a[after]).max():.3g}, without '
      f'{np.abs(b[after]).max():.3g}  {"OK" if ok else "UNEXPECTED"}')
# 10. Presets (PresetExport via hs2presets): .vstpreset and .nksf files load through the VST3 host's preset reader
# (Steinberg's PresetFile::loadPreset) and play exactly what the patch / combi plays when loaded directly.
PRESETS = os.path.join(BUILD, 'plugin', 'hs2presets_artefacts', 'Release', 'hs2presets.exe')
run([PRESETS, '--content', r'C:\Hypersonic 2 Content', '--vst3', f('presets_vst3'), '--nks', f('presets_nks')])


def preset_file(root, ext, combi, bank, patch):
    # the preset whose state names this bank / patch (its XML is plain text inside the file)
    key = (f'combiBank="{bank}" combiIndex="{patch}"' if combi else f'bank="{bank}" patch="{patch}"').encode()
    for d, _, files in os.walk(root):
        for n in files:
            if n.endswith(ext) and key in open(os.path.join(d, n), 'rb').read():
                return os.path.join(d, n)
    raise SystemExit(f'no preset for {bank}/{patch}')


for ext, root in (('.vstpreset', f('presets_vst3')), ('.nksf', f('presets_nks'))):
    for b, p in ([(39, 2), (15, 23)] if ext == '.vstpreset' else [(82, 0), (3, 65)]):
        tag = f'preset_{ext[1:]}_{b}_{p}'
        run([HOST, VST, '--preset', preset_file(root, ext, False, b, p), '--notes', NOTES] + COMMON +
            ['--out', f(tag + '.wav')])
        run([REN, '--part', f'1:{b}:{p}', '--load-bank', '--notes', NOTES] + RO + COMMON +
            ['--host', '-1:120:0', '--block', '32', '--out', f(tag + '_ref.wav')])
        compare(f'{b}/{p} from its {ext} preset', f(tag + '.wav'), f(tag + '_ref.wav'))
run([HOST, VST, '--preset', preset_file(f('presets_nks'), '.nksf', True, 120, 22), '--notes', NC] + COMMON +
    ['--out', f('preset_combi.wav')])
run([REN, '--combi', combi_file(22), '--notes', NC] + RO + COMMON +
    ['--host', '-1:120:0', '--block', '32', '--out', f('preset_combi_ref.wav')])
compare('combi 120/22 from its .nksf preset', f('preset_combi.wav'), f('preset_combi_ref.wav'))

sys.exit(1 if bad else 0)
