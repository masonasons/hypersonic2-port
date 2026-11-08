#!/usr/bin/env python3
"""
Decoder / exporter for Wizoo "Milan Bank" files (Steinberg Hypersonic sample banks).

Usage
-----
  python3 milan_decode.py BANK [BANK ...] [options]

Options
-------
  -o, --outdir DIR          output directory (default: <bank name>_decoded next to the current dir;
                            with several banks, one sub-directory per bank)
  -f, --format FMT          float32 (default) | pcm24 | pcm16.  The banks are float data end to end (the codec
                            accumulates in floating point and can store values above full scale; Hypersonic's
                            engine is float), so float32 is lossless for every sample; the integer formats can
                            only keep samples that fit in +-1.0 and need gain/attenuation for the rest.
  -g, --gain DB             fixed gain in dB (<= 0, e.g. --gain -0.2) applied to ALL samples of ALL banks,
                            PCM included, so relative levels are kept (default 0)
  --apply-zone-gain         also apply each sample's zone volume from the bank (a linear gain between 0.17 and
                            2.0 that Hypersonic applies at playback; the first zone that uses the sample is
                            used).  Opt-in: default exports the sample data as stored.  It multiplies with
                            --gain, and in integer formats can push samples into clipping prevention.
  --no-prevent-clipping     by default, after the gains are applied any sample that would still clip in an
                            integer format is turned down further (both channels equally) so its peak just
                            fits.  With this option only the gain(s) are applied.  float32 never clips.
  --stats-only (--dry-run)  decode and analyse everything (seek-checks, peaks, clipping report) but write no
                            files; handy to find out whether a --gain value is sufficient
  --no-numbers              do not prefix file names with the sample's number in the bank
                            (the bank itself has no numbers, only the names; default: numbers ON)
  --no-metadata             do not write smpl / inst chunks (root note, tuning, loop, key & velocity range)
  --tune-sign {1,-1}        see "Tuning" below (default 1)
  --sample-rate HZ          override the sample rate stored in the bank
  -q, --quiet               only print warnings and the summary

Level report
------------
  The summary after each bank gives the bank's peak level in dBFS *after* the gain(s) (so with
  --gain -0.2 a bank whose decoded peak is +0.32 dBFS reports +0.12 dBFS), which samples (if any) were
  turned down further to avoid clipping and by how much, and the peak of the files actually written.
  Samples that would only have clipped without the gain are not reported.  With --no-prevent-clipping the
  samples that will clip (integer formats) are listed as a warning instead.

Bank format notes (all reverse engineered from Hypersonic.dll and checked against the banks)
-------------------------------------------------------------------------------------------
Sample block = 16-byte header | [compressed only: seek table, 256*channels bytes] | audio data
  header[4:8], [8:12]  PCM:        frames (incl. 2 pad frames), sample rate (Hz)
                       compressed: two int32 a,b  ->  sample rate = 2*a + b
  header[12]  channels,  header[13]  format: 1 = compressed (below), 2 = int16 PCM (interleaved LE),
  header[14], header[15]  compressed only: initial exponent of left / right channel

Compressed codec (one byte per frame per channel, channels interleaved):
    per channel state: exponent k (int), accumulator y (float)
    c >= 0x80 :  k += 1 ;  val = (c - 0xC0) << k
    c <  0x80 :  val = (c - 0x40) << k ;  if ((c + 0x20) & 0x40) and k > 0:  k -= 1
    delta  = val * 2**-21     (Hypersonic computes float32_from_bits(0x40C00000 + val) - 6.0, identical unless
                              |val| >= 2**22; the encoder's seek tables use the exact linear value, so do we)
    output[n] = y ;  y += delta           (1.0 == full scale)
  The seek table holds (k << 24) + (y * 2**21 + 0x200000) for the state at frame (len>>12)*i*i, i = 1..63 (an
  addition: a state below -1.0 borrows from the exponent byte).  The script checks every sample against it
  ("seek-check") so a wrong decode is reported instead of silently written.

Zones (0x40 bytes, found through the instrument table; one block per instrument layer group)
  +1 low velocity  +2 low key  +3 top velocity  +4 high key  +5 root key  +9 sample number (1-based)  +13 name offset
  +17 frames  +21 loop length (0 = no loop)  +25 tune (float, semitones)
  Loop = last `loop length` frames (start = frames - length, end = frames - 1), the 16 frames stored after
  the end of a looped sample are a copy of the loop start (post-roll) and are not exported.

Bank variants handled
---------------------
  * S000 banks (2005, e.g. the kicks): the offset/size fields of the sample table and the (pointer, size) pairs
    of the instrument table are XOR-masked (offset ^ 0xBADEAFFE, size ^ 0xDEADBEEF), exactly the masks Hypersonic
    applies for bank version 13.  Detected automatically.
  * Some banks (e.g. the pianos) put a "SyncrosoftContentHeader" licence blob in front of the zones of each
    instrument block; it is skipped (zones start 8 + <length at +27> bytes into the block).
  * Samples that are used by several zones take root key / tuning / key range / loop from the first zone; all
    zones are listed in index.csv.

Tuning
------
  The tune value is a playback correction; measured on the harpsichord bank, a sample that is sharp by
  X cents has tune = -X.  So the pitch of the stored sample is  root - tune  semitones, which is what is
  written to the smpl chunk (MIDIUnityNote + MIDIPitchFraction).  --tune-sign -1 flips this convention.
  Root keys are MIDI note numbers (the bank's own "C3 = 60" naming is just a label).
"""
import sys, os, re, csv, struct, argparse, collections
import numpy as np

# ----------------------------------------------------------------------------------------------------
# compressed codec
# ----------------------------------------------------------------------------------------------------
def decode_channel(codes, k0):
    """returns (y, ks): decoder accumulator before each frame (1.0 = full scale) and exponent before each frame"""
    n = len(codes)
    vals = np.empty(n, dtype=np.int64)
    ks = np.empty(n, dtype=np.int32)
    k = k0
    for i, c in enumerate(codes.tolist()):
        ks[i] = k
        if c & 0x80:
            k += 1
            vals[i] = (c - 0xC0) << k
        else:
            vals[i] = (c - 0x40) << k
            if ((c + 0x20) & 0x40) and k:
                k -= 1
    # delta = val * 2**-21.  (Hypersonic builds this as float32_from_bits(0x40C00000 + val) - 6.0, which is the same
    # number for |val| < 2**22 but quantises differently for the rare larger steps; the seek tables written by the
    # encoder follow the exact linear value, so that is what is decoded here.)
    y = np.concatenate(([0.0], np.cumsum(vals.astype(np.float64) * 2.0 ** -21)[:-1]))
    return y, ks


def seek_pos(length, i):
    p = ((length >> 12) * i * i) if length & 0xffff0000 else ((length * i * i) >> 12)
    return p - (p & 1)


def seek_check(blob, ch, frames, ys, kss):
    """Compare the decoder state with the checkpoints stored in the sample.  A checkpoint is the 32-bit value
    (k << 24) + (state * 2**21 + 0x200000), added (not OR-ed) so a state below -1.0 borrows from the exponent byte.
    The stored positions are scaled by the sample's true length, which can be a few frames longer than the zone's
    frame count, so small offsets are tried.  Returns (matching, total, approximate): `approximate` counts
    checkpoints that differ by at most 32 units of 2**-21 (a rounding drift seen in a few very long samples);
    real decoding errors are orders of magnitude larger."""
    q = np.frombuffer(blob[16:16 + 256 * ch], '<u4').astype(np.int64)
    expect = []
    for c in range(ch):
        expect.append(((kss[c].astype(np.int64) << 24) + np.rint(ys[c] * 2.0 ** 21).astype(np.int64) + 0x200000) & 0xffffffff)
    best = (0, 63 * ch, 0)
    for extra in range(0, 9):
        ok = approx = tot = beyond = 0
        for c in range(ch):
            for i in range(1, 64):
                e = int(q[i * ch + c]); p = seek_pos(frames + extra, i)
                if p + 2 >= len(expect[c]):
                    beyond += 1                      # checkpoint lies past the data that is actually stored
                    continue
                tot += 1
                hit = None
                for d in (0, 1, -1, 2):
                    j = p + d
                    if 0 <= j < len(expect[c]):
                        diff = ((e - int(expect[c][j]) + 2 ** 31) & 0xffffffff) - 2 ** 31
                        if diff == 0:
                            hit = 0; break
                        if d == 0 and abs(diff) <= 32:
                            hit = 1
                if hit is not None:
                    ok += 1; approx += hit
        if beyond > 0.5 * 63 * ch:                  # too few verifiable checkpoints: count the rest as failures
            tot += beyond
        if ok == tot:
            return ok, tot, approx
        if ok > best[0]:
            best = (ok, tot, approx)
    return best

# ----------------------------------------------------------------------------------------------------
# bank parsing
# ----------------------------------------------------------------------------------------------------
def parse_bank(d):
    L = len(d)
    if L < 0x1000 or not d.startswith(b'Milan Bank file'):
        raise SystemExit('not a Milan bank file (missing "Milan Bank file" header)')
    hdr_words = struct.unpack('<%dI' % ((0xa0 - 0x40) // 4), d[0x40:0xa0])
    # sample table: chain of (offset, size) records, stride 32, last one ends exactly at EOF.
    # Newer banks (S000, 2005) XOR the offset with 0xBADEAFFE and the size with 0xDEADBEEF (as Hypersonic does
    # for bank version 13); both variants are tried.
    best = None
    for om, sm in ((0, 0), (0xbadeaffe, 0xdeadbeef)):
        for w in set(hdr_words):
            base = w + 0x80
            if not (0x100 < base < L - 64):
                continue
            o = base; n = 0; last = None
            while o + 36 <= L:
                a, b = struct.unpack('<II', d[o:o + 8]); a ^= om; b ^= sm
                nx, = struct.unpack('<I', d[o + 32:o + 36]); nx ^= om
                if b == 0 or a + b > L:
                    break
                n += 1; last = (a, b)
                if a + b != nx:
                    break
                o += 32
            # audio data must come after the table; short chains are accepted (small banks have only a few samples)
            if n >= 1 and last and last[0] + last[1] == L and (best is None or n > best[0]):
                first = struct.unpack('<I', d[base:base + 4])[0] ^ om
                if first >= base + 32 * n:
                    best = (n, base, om, sm)
    if not best:
        raise SystemExit('could not locate the sample table')
    n, base, om, sm = best
    recs = []
    for k in range(n):
        a, b = struct.unpack('<II', d[base + 32 * k:base + 32 * k + 8])
        recs.append((a ^ om, b ^ sm))
    first_data = recs[0][0]

    # zones: blocks of 0x40-byte records referenced by (pointer, size) pairs in the instrument table
    zones = collections.defaultdict(list)
    blocks = []
    SIG = b'SyncrosoftContentHeader'          # licence header in front of the zones of some banks (e.g. the pianos)
    for o in range(0x40, min(first_data, 0x4000), 4):
        p, s = struct.unpack('<II', d[o:o + 8])
        p ^= om; s ^= sm                                  # instrument table is masked in S000 banks too
        if not (0x100 < p < first_data and 0 < s <= 0x200000 and p + s <= first_data + 1 and p > o + 8):
            continue
        if d[p:p + len(SIG)] == SIG:
            skip = 8 + struct.unpack('<I', d[p + 27:p + 31])[0]
        elif s % 0x40 == 0:
            skip = 0
        else:
            continue
        blocks.append((o, p, s, skip))
    for bi, (o, p, s, skip) in enumerate(blocks):
        for k in range((s - skip) // 0x40):
            r = d[p - 1 + skip + 0x40 * k:p - 1 + skip + 0x40 * (k + 1)]
            idx, noff, fr, lp = struct.unpack('<IIII', r[9:25])
            if not (1 <= idx <= n) or not (0 < fr < 10 ** 9):
                continue
            zones[idx].append(dict(block=bi, lo=r[2], vel_lo=r[1], vel_hi=r[3], hi=r[4], root=r[5], noff=noff,
                                   frames=fr, loop_len=lp, tune=struct.unpack('<f', r[25:29])[0],
                                   volume=struct.unpack('<f', r[37:41])[0]))

    # sample names: zone name offsets are relative to (name table pointer + 89) which is one of the header words
    names = {}
    zl_first = [(idx, zl[0]['noff']) for idx, zl in zones.items()]
    best_b = None
    for w in sorted(set(hdr_words)):
        nb = w + 89
        good = 0
        for idx, noff in zl_first:
            p = nb + noff
            if not (1 <= p < L - 40):
                continue
            e = d.find(b'\0', p, p + 48)
            s_ = d[p:e] if e > 0 else b''
            if len(s_) >= 3 and re.fullmatch(rb'[ -~]+', s_):
                good += 1
        if best_b is None or good > best_b[0]:
            best_b = (good, nb)
    if best_b and zl_first and best_b[0] >= 0.5 * len(zl_first):
        for idx, noff in zl_first:
            p = best_b[1] + noff
            e = d.find(b'\0', p)
            names[idx] = d[p:e].decode('latin1').strip()
    if len(names) > 3 and len(set(names.values())) < 0.5 * len(names):
        names = {}                      # e.g. every zone points at one shared label: not a sample name
    return recs, zones, names

# ----------------------------------------------------------------------------------------------------
# audio extraction
# ----------------------------------------------------------------------------------------------------
class Skip(Exception):
    pass


def extract_audio(blob, zone_frames):
    """returns dict(audio=float64 [frames, ch] (1.0 = full scale), rate, ch, fmt, check, truncated).
    zone_frames: the bank's frame count(s) for this sample (an int, or a list when several zones disagree; the one
    that fits the sample's seek table is used)."""
    ch, fmt = blob[12], blob[13]
    if ch not in (1, 2):
        raise Skip('unsupported channel count %d' % ch)
    if isinstance(zone_frames, (list, tuple)):
        cands = [c for c in dict.fromkeys(zone_frames) if c]
    else:
        cands = [zone_frames] if zone_frames else []
    if fmt == 1:
        a, b = struct.unpack('<ii', blob[4:12])
        rate = 2 * a + b
        body = np.frombuffer(blob[16 + 256 * ch:], np.uint8)
        nfr = len(body) // ch
        if not cands:
            cands = [nfr - 1]
        ys, kss = [], []
        for c in range(ch):
            y, ks = decode_channel(body[c::ch][:min(nfr, max(cands) + 8)], blob[14 + c])
            ys.append(y); kss.append(ks)
        best = None
        for nominal in cands:                                   # keep the first candidate unless another fits better
            res = seek_check(blob, ch, nominal, ys, kss)
            key = (res[1] - res[0], -res[1])                     # fewest failures, then most verified checkpoints
            if best is None or key < best[0]:
                best = (key, nominal, res)
        nominal, check = best[1], best[2]
        frames = min(nominal, nfr)                              # what is actually stored
        audio = np.stack([y[:frames] for y in ys], 1)
        return dict(audio=audio, rate=rate, ch=ch, fmt=fmt, check=check, truncated=max(0, nominal - nfr))
    # raw PCM
    hdr_frames, rate = struct.unpack('<II', blob[4:12])
    body = blob[16:]
    bps = len(body) // (hdr_frames * ch) if hdr_frames else 0
    zone_frames = cands[0] if cands else None
    frames = zone_frames if zone_frames else max(hdr_frames - 2, 0)
    frames = min(frames, hdr_frames)
    if fmt == 2 or bps == 2:
        x = np.frombuffer(body[:hdr_frames * ch * 2], '<i2').reshape(-1, ch)[:frames].astype(np.float64) / 32768.0
    elif bps == 3:
        raw = np.frombuffer(body[:hdr_frames * ch * 3], np.uint8).reshape(-1, 3)
        v = (raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int32) << 16))
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = (v.reshape(-1, ch)[:frames] / 8388608.0)
    elif bps == 4:
        f = np.frombuffer(body[:hdr_frames * ch * 4], '<f4').astype(np.float64)
        if np.isfinite(f).all() and np.abs(f).max() < 64:
            x = f.reshape(-1, ch)[:frames]
        else:
            x = np.frombuffer(body[:hdr_frames * ch * 4], '<i4').astype(np.float64).reshape(-1, ch)[:frames] / 2147483648.0
        print('  note: PCM format %d with 4 bytes/sample is untested' % fmt, file=sys.stderr)
    else:
        raise Skip('unknown PCM layout (format %d, %d bytes/sample)' % (fmt, bps))
    return dict(audio=x, rate=rate, ch=ch, fmt=fmt, check=None, truncated=max(0, (zone_frames or 0) - hdr_frames))

# ----------------------------------------------------------------------------------------------------
# WAV writing (PCM 16/24, float32, with optional smpl + inst chunks)
# ----------------------------------------------------------------------------------------------------
def chunk(tag, payload):
    pad = b'\0' if len(payload) & 1 else b''
    return tag + struct.pack('<I', len(payload)) + payload + pad


def smpl_chunk(rate, unity, fraction, loop):
    loops = b''
    if loop:
        loops = struct.pack('<6I', 0, 0, loop[0], loop[1], 0, 0)       # id, forward, start, end(inclusive), fraction, count
    head = struct.pack('<9I', 0, 0, int(round(1e9 / rate)), unity, fraction, 0, 0, 1 if loop else 0, 0)
    return chunk(b'smpl', head + loops)


def inst_chunk(unity, lo, hi, vlo, vhi):
    return chunk(b'inst', struct.pack('<7B', unity, 0, 0, lo, hi, vlo, vhi) + b'\0')


def write_wav(path, audio, rate, fmt, smpl=None, inst=None):
    ch = audio.shape[1]
    if fmt == 'float32':
        data = audio.astype('<f4').tobytes(); tag, bits = 3, 32
    elif fmt == 'pcm24':
        v = np.clip(np.round(audio * 8388608.0), -8388608, 8388607).astype('<i4')
        data = v.astype('<i4').view(np.uint8).reshape(-1, 4)[:, :3].tobytes(); tag, bits = 1, 24
    else:
        data = np.clip(np.round(audio * 32768.0), -32768, 32767).astype('<i2').tobytes(); tag, bits = 1, 16
    block = ch * bits // 8
    if tag == 3:
        fmtc = chunk(b'fmt ', struct.pack('<HHIIHHH', 3, ch, rate, rate * block, block, bits, 0))
        extra = chunk(b'fact', struct.pack('<I', len(audio)))
    else:
        fmtc = chunk(b'fmt ', struct.pack('<HHIIHH', 1, ch, rate, rate * block, block, bits))
        extra = b''
    body = b'WAVE' + fmtc + extra + chunk(b'data', data) + (inst or b'') + (smpl or b'')
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)

# ----------------------------------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------------------------------
NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']

def note_name(n):
    return '%s%d' % (NOTE[n % 12], n // 12 - 2)       # C3 = 60, the bank's own convention


def clean(s):
    return re.sub(r'[^A-Za-z0-9_.#+-]+', '_', s).strip('_')


def db(x):
    return 20 * np.log10(x) if x > 0 else float('-inf')


def fmt_db(x):
    return '-inf' if x == float('-inf') else '%+.2f' % x


def process_bank(path, outdir, a):
    d = open(path, 'rb').read()
    recs, zones, names = parse_bank(d)
    if not a.stats_only:
        os.makedirs(outdir, exist_ok=True)
    stem = re.sub(r'^\d+_', '', os.path.basename(path))
    used = collections.Counter()
    rows = []; bad = 0; skipped = 0; trunc_list = []; zone_conflicts = 0; n_int_over = 0
    nblocks = 1 + max([z['block'] for zl in zones.values() for z in zl] or [0])
    extra_list = []                 # (index, name, extra dB, total dB) samples turned down beyond the fixed gain
    clip_list = []                  # (index, name, peak dBFS) samples that clip (prevention disabled)
    g = 10 ** (a.gain / 20.0)
    maxint = {'pcm16': 32767, 'pcm24': 8388607}.get(a.format)       # None for float32 (never clips)
    scale = (maxint + 1) if maxint else None
    peak_gain = (0.0, None); peak_out = (0.0, None)                 # (linear peak, (index, name))
    for i, (off, sz) in enumerate(recs, start=1):
        blob = d[off:off + sz]
        zl = zones.get(i, [])
        z0 = zl[0] if zl else None
        try:
            r = extract_audio(blob, [z['frames'] for z in zl] if zl else None)
        except Skip as e:
            print('%3d  skipped: %s' % (i, e), file=sys.stderr); skipped += 1
            continue
        audio = r['audio']; rate = a.sample_rate or r['rate']
        # --- name
        nm = names.get(i, '')
        if not nm:
            nm = os.path.splitext(stem)[0]
            if nblocks > 5 and z0:
                nm += '_i%02d' % (z0['block'] + 1)              # instrument / layer group the zone belongs to
            nm += '_%s' % (note_name(z0['root']) if z0 else 'x')
            if z0 and (z0['vel_lo'] > 1 or z0['vel_hi'] < 127):
                nm += '_v%d-%d' % (z0['vel_lo'], z0['vel_hi'])
        nm = clean(nm) or 'sample'
        if a.numbers:
            fn = '%03d_%s' % (i, nm)
        else:
            used[nm] += 1
            fn = nm if used[nm] == 1 else '%s_%d' % (nm, used[nm])
        fn += '.wav'
        # --- fixed gain, then clipping prevention for whatever still clips
        peak_src = float(np.abs(audio).max()) if audio.size else 0.0
        zg = 1.0
        if a.apply_zone_gain and z0:
            vols = {round(z['volume'], 5) for z in zl}
            if len(vols) > 1:
                zone_conflicts += 1
            if z0['volume'] > 0 and np.isfinite(z0['volume']):
                zg = float(z0['volume'])
        gg = g * zg
        if gg != 1.0:
            audio = audio * gg
        pk_g = peak_src * gg
        extra = 1.0
        if maxint and audio.size:
            pos, neg = float(audio.max()), float(-audio.min())
            if pos * scale > maxint + 0.5 or neg * scale > maxint + 1.5:        # rounding would overflow
                if a.prevent_clipping:
                    extra = min(maxint / scale / pos if pos > 0 else np.inf,
                                (maxint + 1) / scale / neg if neg > 0 else np.inf)
                    audio = audio * extra
                    extra_list.append((i, nm, db(extra), a.gain + db(zg) + db(extra)))
                else:
                    clip_list.append((i, nm, db(pk_g)))
        pk_out = pk_g * extra
        if maxint is None and pk_g * 8388608.0 > 8388607.5:
            n_int_over += 1
        if pk_g > peak_gain[0]:
            peak_gain = (pk_g, (i, nm))
        if pk_out > peak_out[0]:
            peak_out = (pk_out, (i, nm))
        total_db = a.gain + db(zg) + db(extra)
        # --- metadata
        smpl = inst = None; loop = None
        if a.metadata and z0:
            zf = z0['frames']                                   # loop is defined against the bank's frame count
            if z0['loop_len'] and z0['loop_len'] <= zf and zf - z0['loop_len'] < len(audio) - 1:
                loop = (zf - z0['loop_len'], min(zf - 1, len(audio) - 1))
            if not a.stats_only:
                pitch = z0['root'] - a.tune_sign * round(z0['tune'], 4)            # pitch of the stored sample, in semitones
                unity = int(np.floor(pitch + 1e-9)); frac = pitch - unity
                unity = min(max(unity, 0), 127)
                fraction = min(int(round(frac * 2 ** 32)), 2 ** 32 - 1)
                smpl = smpl_chunk(rate, unity, fraction, loop)
                inst = inst_chunk(z0['root'] & 127, z0['lo'] & 127, z0['hi'] & 127,
                                  max(z0['vel_lo'], 1 if z0['vel_hi'] else 0) & 127, z0['vel_hi'] & 127)
        if not a.stats_only:
            write_wav(os.path.join(outdir, fn), audio, rate, a.format, smpl, inst)
        chk = r['check']
        if chk and chk[0] != chk[1]:
            bad += 1
        if r['truncated']:
            trunc_list.append((i, nm, r['truncated']))
        msg = '%3d %-32s %d ch %6d Hz %8d fr %s%s%s%s' % (
            i, nm[:32], r['ch'], rate, len(audio),
            (('seek-check %d/%d' % chk[:2]) + (' (%d approx)' % chk[2] if chk[2] else '')) if chk else 'PCM',
            ('  loop %d-%d' % loop) if loop else '',
            ('  TRUNCATED: %d frames missing' % r['truncated']) if r['truncated'] else '',
            ('  FURTHER ATTENUATION %.3f dB (total %.3f dB)' % (db(extra), total_db)) if extra < 1 else '')
        if not a.quiet or (chk and chk[0] != chk[1]):
            print(msg, flush=True)
        rows.append(dict(sample=i, name=nm, file=fn, rate=rate, channels=r['ch'],
                         storage='compressed' if r['fmt'] == 1 else 'pcm', frames=len(audio),
                         seek_check=('%d/%d' % chk[:2]) if chk else '', peak_dbfs_decoded='%.3f' % db(peak_src),
                         gain_db='%.3f' % total_db, zone_gain_db=('%.2f' % db(z0['volume'])) if z0 and z0.get('volume', 0) > 0 else '', truncated_frames=r['truncated'], further_attenuation_db='%.3f' % db(extra), peak_dbfs_output='%.3f' % db(pk_out),
                         root_key=z0['root'] if z0 else '', tune_semitones='%.4f' % z0['tune'] if z0 else '',
                         low_key=z0['lo'] if z0 else '', high_key=z0['hi'] if z0 else '',
                         vel_lo=z0['vel_lo'] if z0 else '', vel_hi=z0['vel_hi'] if z0 else '',
                         loop=('%d-%d' % loop) if loop else '', zones=len(zl),
                         all_zones=';'.join('%d-%d/%d-%d/r%d/t%.3f' % (z['lo'], z['hi'], z['vel_lo'], z['vel_hi'], z['root'], z['tune']) for z in zl)))
    if rows and not a.stats_only:
        with open(os.path.join(outdir, 'index.csv'), 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader(); w.writerows(rows)
    # ------------------------------------------------------------------ report
    nloop = sum(1 for r in rows if r['loop'])
    print('%s: %d samples %s  (%d looped, %d skipped, %d failed seek-check%s)'
          % (os.path.basename(path), len(rows),
             'analysed, no files written' if a.stats_only else 'written to %s' % outdir, nloop, skipped, bad,
             (', %d truncated' % len(trunc_list)) if trunc_list else ''))
    if trunc_list:
        print('  truncated samples (stored data is shorter than the bank says; exported as stored): '
              + ', '.join('%d (-%d fr)' % (x[0], x[2]) for x in trunc_list[:12]) + (' ...' if len(trunc_list) > 12 else ''))

    def who(p):
        return "sample %d '%s'" % p[1] if p[1] else ''

    print(('  gain: %+.2f dB applied to all samples' % a.gain if a.gain else '  gain: none (0 dB)')
          + ('; zone gain applied (per-sample volume from the bank)' if a.apply_zone_gain else ''))
    if a.apply_zone_gain and zone_conflicts:
        print('  note: %d samples are used by zones with different volumes; the first zone\'s volume was used' % zone_conflicts)
    print('  bank peak after gain: %s dBFS  (%s)' % (fmt_db(db(peak_gain[0])), who(peak_gain)))

    def lst(items, f, n=12):
        s = ', '.join(f(x) for x in items[:n])
        return s + (', ... and %d more' % (len(items) - n) if len(items) > n else '')

    if maxint is None:
        print('  float32: no clipping is possible, peaks are written as decoded'
              + ('; in an integer format %d samples would exceed full scale at this gain' % n_int_over if n_int_over
                 else '; no sample would exceed full scale in an integer format either'))
    elif a.prevent_clipping:
        if extra_list:
            print('  clipping prevention: %d samples still clipped after the gain and were turned down further '
                  '(by up to %.3f dB); peak of the written files: %s dBFS'
                  % (len(extra_list), -min(x[2] for x in extra_list), fmt_db(db(peak_out[0]))))
            print('    ' + lst(extra_list, lambda x: '%d (%.3f dB)' % (x[0], x[2])))
        else:
            print('  clipping prevention: no sample clips, nothing turned down further')
    else:
        if clip_list:
            print('  WARNING: %d samples exceed full scale after the gain and WILL CLIP (clipping prevention is off):' % len(clip_list))
            print('    ' + lst(clip_list, lambda x: '%d (%s dBFS)' % (x[0], fmt_db(x[2]))))
        else:
            print('  no sample clips with this gain')
    return dict(bad=bad, peak_gain=peak_gain, peak_out=peak_out, name=os.path.basename(path),
                n_extra=len(extra_list), n_clip=len(clip_list), n=len(rows))


def main():
    ap = argparse.ArgumentParser(description='Decode Wizoo Milan banks (Steinberg Hypersonic) to WAV files',
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('banks', nargs='+')
    ap.add_argument('-o', '--outdir')
    ap.add_argument('-f', '--format', choices=['float32', 'pcm24', 'pcm16'], default='float32')
    ap.add_argument('-g', '--gain', '-gain', type=float, default=0.0, metavar='DB',
                    help='fixed gain in dB (<= 0) applied to all samples')
    ap.add_argument('--apply-zone-gain', action='store_true',
                    help='also apply each sample\'s zone volume from the bank')
    ap.add_argument('--no-prevent-clipping', dest='prevent_clipping', action='store_false')
    ap.add_argument('--stats-only', '--dry-run', dest='stats_only', action='store_true',
                    help='analyse only, write no files')
    ap.add_argument('--no-numbers', dest='numbers', action='store_false')
    ap.add_argument('--no-metadata', dest='metadata', action='store_false')
    ap.add_argument('--tune-sign', type=int, choices=[1, -1], default=1)
    ap.add_argument('--sample-rate', type=int)
    ap.add_argument('-q', '--quiet', action='store_true')
    a = ap.parse_args()
    if a.gain > 0:
        ap.error('--gain must be 0 or negative (a reduction in dB), e.g. --gain -0.2')
    bad = 0; results = []
    for b in a.banks:
        if not os.path.isfile(b):
            print('error: %s: no such file' % b, file=sys.stderr); bad += 1
            continue
        out = a.outdir
        if out is None:
            out = os.path.splitext(os.path.basename(b))[0] + '_decoded'
        elif len(a.banks) > 1:
            out = os.path.join(out, os.path.basename(b))
        try:
            res = process_bank(b, out, a)
            bad += res['bad']; results.append(res)
        except SystemExit as e:
            print('error: %s: %s' % (b, e), file=sys.stderr); bad += 1
    if len(results) > 1:
        pg = max(results, key=lambda r: r['peak_gain'][0]); po = max(results, key=lambda r: r['peak_out'][0])
        print('ALL BANKS: peak after gain %s dBFS (%s, sample %s);  peak of written files %s dBFS;  '
              '%d samples turned down further, %d clipping'
              % (fmt_db(db(pg['peak_gain'][0])), pg['name'], pg['peak_gain'][1][0] if pg['peak_gain'][1] else '-',
                 fmt_db(db(po['peak_out'][0])), sum(r['n_extra'] for r in results), sum(r['n_clip'] for r in results)))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
