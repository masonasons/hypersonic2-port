"""Checks the files of tests/run_test.sh's two hs2capture runs against the mock plugin."""
import os
import struct
import sys

B = sys.argv[1]
OUT = os.path.join(B, 'out')
fails = 0
count = 0


def check(cond, what):
    global fails, count
    count += 1
    if not cond:
        fails += 1
        print('FAIL:', what)


def read(p, mode='rb'):
    with open(p, mode) as f:
        return f.read()


run1, run2 = read(os.path.join(B, 'run1.log'), 'r'), read(os.path.join(B, 'run2.log'), 'r')
check('exit 3' in run1, 'first run exits 3 (one patch failed)')
check('FAILED' in run1 and 'bank 99' in run1, 'the refused bank is reported')
check('2 captured' not in run1 and '3 captured, 1 failed' in run1, 'three captured, one failed: ' + run1[-200:])
check('0 captured, 1 failed, 3 already there' in run2, 'second run skips the captured patches: ' + run2[-200:])

snaps = os.path.join(OUT, 'snapshots')
ep = os.path.join(snaps, '008-030 Electric Piano 2')
fxb, fxp = read(ep + '.fxb'), read(ep + '.fxp')
check(fxb[:4] == b'CcnK' and fxb[8:12] == b'FBCh', '.fxb header')
check(struct.unpack('>I', fxb[4:8])[0] == len(fxb) - 8, '.fxb size field')
check(fxb[16:20] == b'StSS' and struct.unpack('>I', fxb[20:24])[0] == 2100, '.fxb plugin id and version')
check(struct.unpack('>I', fxb[0x9c:0xa0])[0] == len(fxb) - 0xa0, '.fxb chunk size at 0x9c')
check(fxb[0xa0:].startswith(b'MOCKBANK 8:30 time=1'), '.fxb holds the bank chunk of 8:30, host time seen')
check(fxp[8:12] == b'FPCh' and fxp[28:44] == b'Electric Piano 2', '.fxp header and program name')
check(struct.unpack('>I', fxp[56:60])[0] == len(fxp) - 60 and fxp[60:].startswith(b'MOCKPROG 8:30'),
      '.fxp program chunk')
names = sorted(os.listdir(snaps))
check(any(n.startswith('015-000 Slash_Name_ _x_') for n in names), 'unsafe characters replaced: %s' % names)
check(not any(n.startswith('099') for n in names), 'no snapshot for the refused patch')
check(read(os.path.join(snaps, [n for n in names if n.startswith('015-000')][0]))[0xa0:].startswith(b'MOCKBANK 15:0'),
      'second patch loaded')

sdir = os.path.join(OUT, 'samples', '008-030 Electric Piano 2')
wavs = sorted(n for n in os.listdir(sdir) if n.endswith('.wav'))
check(wavs == ['008-030 Electric Piano 2 n060 C4 v040.wav', '008-030 Electric Piano 2 n060 C4 v100.wav',
               '008-030 Electric Piano 2 n072 C5 v040.wav', '008-030 Electric Piano 2 n072 C5 v100.wav'],
      'wav names: %s' % wavs)
w = read(os.path.join(sdir, wavs[1]))
check(w[:4] == b'RIFF' and w[8:12] == b'WAVE' and struct.unpack('<I', w[4:8])[0] == len(w) - 8, 'wav RIFF size')
fmt, ch, sr, _, _, bits = struct.unpack('<HHIIHH', w[20:36])
check((fmt, ch, sr, bits) == (3, 2, 44100, 32), 'wav format float stereo 44.1k')
i = w.index(b'smpl')
check(struct.unpack('<I', w[i + 4:i + 8])[0] == 36 and struct.unpack('<I', w[i + 20:i + 24])[0] == 60, 'smpl root')
d = w.index(b'data')
n = struct.unpack('<I', w[d + 4:d + 8])[0]
check(d + 8 + n == len(w), 'data chunk ends the file')
s = struct.unpack('<%df' % (n // 4), w[d + 8:d + 8 + n])
frames = len(s) // 2
check(0.25 * 44100 < frames < 1.4 * 44100, 'note length: hold 0.2 s + release, trimmed: %d frames' % frames)
pk = max(abs(x) for x in s)
check(abs(pk - 100 / 127 * 0.5) < 0.01, 'velocity 100 level: %.4f' % pk)
w40 = read(os.path.join(sdir, wavs[0]))
d4 = w40.index(b'data')
n4 = struct.unpack('<I', w40[d4 + 4:d4 + 8])[0]
s4 = struct.unpack('<%df' % (n4 // 4), w40[d4 + 8:d4 + 8 + n4])
check(abs(max(abs(x) for x in s4) - 40 / 127 * 0.5) < 0.01, 'velocity 40 level')
check(abs(s[-1]) < 1e-4 and abs(s[-2]) < 1e-4, 'ends faded')
sfz = read(os.path.join(sdir, '008-030 Electric Piano 2.sfz'), 'r')
check('lovel=1 hivel=70' in sfz and 'lovel=71 hivel=127' in sfz, 'velocity layers split halfway: ' + sfz)
check('pitch_keycenter=60 lokey=0 hikey=66' in sfz and 'pitch_keycenter=72 lokey=67 hikey=127' in sfz,
      'key ranges halfway between notes')
check(sfz.count('<region>') == 4, 'four regions')

silent_dir = os.path.join(OUT, 'samples', '008-005 Silent')
check(not [n for n in os.listdir(silent_dir) if n.endswith('.wav')], 'silent patch: no wavs')
res = read(os.path.join(OUT, 'results.tsv'), 'r').splitlines()
check(res[0].startswith('bank\tindex') and len(res) == 6,
      'results.tsv: header + 4 rows, + the failed patch again (retried by the second run)')
check(any(r.startswith('8\t5\tSilent\tok\t4 silent notes skipped') for r in res), 'silent notes reported')
check(any(r.startswith('99\t1\tRefused\tfailed\tthe plugin') for r in res), 'refusal reported')

print('%d passed, %d failed' % (count - fails, fails))
sys.exit(1 if fails else 0)
