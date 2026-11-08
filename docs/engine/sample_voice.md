# Sample voice (PCM mono, render mode 0x11, `FUN_1004b490`)

Voice struct: 0x6B8 bytes. `v[i]` = 32-bit word i (byte offset 4i). Floats unless noted.

## Per sample (`1004b490`)
```
if v[1] (note delay, samples) > 0: consume it, skip output
if v[0x148] != element->paramVersion: refresh element-derived fields (10049eb0)
for each output frame:
    if --v[2] < 0: control_update()            # 1004ab60; reloads v[2] = 31, i.e. every 32 samples
    while phase(v[0x10]) > 1:
        phase -= 1
        if pos(v[5], int) == end(v[6], int):
            if loopLen(v[7]) == 0: pos = end - step; step(v[8]) = 0        # hold the last frame
            else: pos -= loopLen; v[0x87] += v[0xf]; v[0xf] = 0            # loop (v[0xf]: pending pitch delta)
        y0(v[0xa]) += slope(v[0xb])                                            # = previous sample
        s = int16 data[pos] / 32768   (bit trick: asfloat(0x43808000 + s) - 257)
        pos += step                    # step = channel count
        slope = s - y0
    x = slope * phase + y0             # linear interpolation
    phase += inc(v[0x11])
    # 2-pole SVF on the 2-sample sum, then a one-pole stage
    hp(v[0x1a]) = k(v[0x17]) * bp(v[0x19]) - x - xprev(v[0x1b]) + lp(v[0x18])
    bp -= hp * a(v[0x14])
    lp += bp * b(v[0x15])
    xprev = x
    t(v[0x1d]) -= ((t - lp) + u(v[0x1c])) * c(v[0x16])
    u += t * c
    out = *tap(v[0x27])                # tap points at u (0x70), t (0x74) or 0x8c (chosen by filter mode at note-on)
    if invert(v[0x26]): out = 2*xprev - out
    gainL(v[0x2c]) += incL(v[0x2d]);  outL += gainL * out
    gainR(v[0x2e]) += incR(v[0x2f]);  outR += gainR * out
```

## Per 32-sample block: `control_update` (`1004ab60`)
```
lfoA(v[0xae]), lfoB(v[0xaf]) = next two values from the shared stream (*v[0xb9]++)   # probably patch LFO1/LFO2
v[0xb2] = clamp(v[0xb2], 0, 10);  v[0xb0] = curve_10048e10(v[0xb1] * v[0xb2])       # LFO3? (TBD)
# Env 1: linear segments (multi-stage "Level/Time 1..3" env)
r = clamp(v[0x9e], 0, 10)  (rate multiplier from mods; reset to 1 each block)
time(v[0x9c]) += r;  level1(v[0x9a]) += r * slope(v[0x9b]);  if time > segEnd(v[0x9d]): next_stage(1, v[0x9f])
# Env 3 (filter): exponential
fenv(v[0xa0]) += (target(v[0xa2]) - fenv) * coef(v[0xa1]);  if fenv > hi(v[0xa3]) or < lo(v[0xa4]): next_stage(3, v[0xa6])
# Env 4 (amp): exponential
aenv(v[0xa7]) += (target(v[0xa9]) - aenv) * coef(v[0xa8]); if beyond v[0xaa]/v[0xab]: next_stage(4, v[0xad])
# Glide (exponential, on log pitch)
glide(v[0x91]) += (target(v[0x94]) - glide) * coef(v[0x92])
# Seed the modulation sums with their base values
pitch(v[0x11]) = *pitchSrc(v[0x93]) + pitchBase(v[0x88]) + glide
v[0x12] = v[0x89];  v[0x13] = v[0x90];  k(v[0x17]) = reso(v[0x8a]);  amp(v[0x2a]) = 1;  pan(v[0x2b]) = v[0x8f]
cutoff(v[0x15]) = fenvAmt(v[0x8d]) * fenv + cutoffBase(v[0x8b])
v[0xb2] = v[0x9e] = 1
# Mod matrix: v[0xc5] active slots, 8 words each from v[0xc6]
for slot: if slot.special(w7) == 0:  out = (*slot.src(w3) * slot.scale(w2) + slot.offset(w1)) * *slot.mod(w0)
                                     slot.last(w4) = out;  *slot.dest(w5) += out
          else FUN_10049090(slot)        # processed modifiers (S+H, peak hold, ...)
# Convert
lp_pitch(v[0xbf]) = pitch
inc(v[0x11]) = min(exp(pitch + rateLn(v[0x87])), maxInc(v[0x86]))     # maxInc = 16.0 for samples
f = exp(lp_pitch * keytrack(v[0x8c]) + cutoff);  b(v[0x15]) = f
c(v[0x16]) = min(f * v[0x3e], 1)
damping: k = 0.95 - k (k held the resonance value)
    if k < 0.15:                                   # near self-oscillation: amplitude-dependent damping
        k = max(k, f <= 1 ? (f - 1) * 0.05 : 0)
        e = 0.005 * (bp^2 + lp^2)  [max with the second state pair for modes > 0x12]
        env(v[0x30]) += (e - env) * 0.05;  k += env
    k = min(k, 1)
frequency cap:  modes -2 / -4 (analog): a = 3.25, b = min(b, 3.25)
                else: cap = 1.92 - 0.85 k;  a = cap
                      if f <= cap: a = f if flag v[0x31]
                      else: k = lerp(k, v[0x29], min(f - cap, 1)); a = 1.92 - 0.85 k; b = min(b, a)
amp = max(amp, 0) * velLevel(v[0x8e]) * aenv;   pan = clamp(pan, 0, 1)
R = amp * pan;  L = amp - R          # linear pan law
incR = (R - gainR) / 32;  incL = (L - gainL) / 32;  v[2] = 31
```
(`exp` here = x87 `f2xm1`/`fscale` of `x * log2(e)`, i.e. a natural exp.)

## Envelopes: stage setup `next_stage(env, stage)` = `FUN_1004a340`
Shared helpers:
- `exp(x)` = `FUN_1000c5b0` (natural exp via f2xm1/fscale).
- `coef(time, T) = 1 - exp(-3.45 * T / time)`, or 1 if time < 1e-6 (`FUN_100499b0`). T = element+0xc = 32/sr (block period).
  A segment covers about 97 % of its distance in `time`.
- `tempo = (hostTempoScale - 1) * TempoToEnvRates + 1`; it multiplies every rate.
- Env times (Attack/Decay/Release/F.*): `t(v) = 4v²` for v ≤ 0.5, else `32 v⁵` seconds (`FUN_10048da0`).
- Sustain: v clamped to [1e-8, 0.99]. Decay Curve: `exp(9v - 7)`. Velocity→Level `4v-2`. Velocity→Attack `4-8v`.

Filter env (3) and amp env (4) are the same machine (amp env fields: level v[0xa7], coef v[0xa8], target v[0xa9],
hi v[0xaa], lo v[0xab], stage v[0xad], peak v[0xac]); element fields shown are for the amp env (+0x578..), the filter env uses +0x548..:
| stage | target | ends when | time |
|---|---|---|---|
| 1 Attack | 3.4 × peak | level ≥ peak | `(attack + startLevelOpt) * exp(velToAttack*vel) * tempo`, coefficient uses T*0.1 |
| 2 Decay | `S - (start - S) * curve`, S = peak*sustain | level ≤ S | `decay * exp(velToDecay*vel + keyToDecay*key) * tempo`, coefficient uses `T * -0.3 * ln((curve+0.01)/(curve+1))`; skipped if sustain > 0.98 |
| 3 Sustain | S, or peak/0 when Fade Up/Down ≠ 0 | (envelope mode 3 = one-shot → straight to release) | `|fade| * exp(key*keyAmt*0.5) * tempo` |
| 4+ Release | 1e-10 | external | `release * exp(vel*relVel + key*relKey) * tempo`; stage 4 = voice-steal fast kill (elem+0x5a8, flag 0x534=2) |
Env 1 (pitch/mod, linear): 5 segments, `slope = (L_next - L_cur) / (time * tempo)` (elem+0x524..0x540 = t1, L1, t2, L2, t3, L3, …).

## LFO oscillator `FUN_10048e10` (state: phase, value, slope, wave, half)
phase += rate; wave 0 = sine as half-cycles `±sin(π·phase)` (sign flips every half); otherwise piecewise-linear segments
re-planned each half-cycle: 1 const 0, 2 const 1, 3 triangle (slope ±2), 4 saw, 5 square, 6 S+H random, 7 S+H alternate,
8 random drift (random slope), 9 slow drift (bounded random walk, step 0.2). (Mapping to the LFO Wave display names TBD.)

## Randomness
MSVC `rand()` (LCG `s = s*214013 + 2531011; (s>>16) & 0x7fff`): `rand01 = rand()/32768`, `randpm = rand()/16384 - 1`.

## Note-on modulation
A second mod-matrix list at voice+0x314 (`FUN_10049dd0` / `FUN_10049e40`) runs once at note-on for note-on
destinations (start point, initial level, …).

## Zone selection and note gain (`FUN_10021e20`, called from note handling `FUN_10017880`)
- gain = keyTable[note] × velTable[vel] × 1e-8 (tables from `FUN_10048b80`: unity in Low..High, linear ramps of
  10000/(1+trunc(xfade²·127.9)) per step outside; High < Low = split range). The patch-level tables multiply in too.
- fvel = Receive Velocity ? vel − 64 : 8. shift = trunc(fvel·formantVel + rand·formantRand) + formant.
- zone key = trunc((note − 60)·keytrack + semis + octave·12 + 60.5) − shift (if the instrument's first zone has flag 0x200:
  formant == 12 ? shift + note − 12 : shift + 48), clamped 0..127.
- zone vel = clamp(trunc(fvel·VelScale + rand·VelRnd) + VelOffs, 1, 127) (modes 1/5 cycle via `FUN_1001c0f0`: TODO).
- Note-off: env1 → stage 4, filter env → release; amp env → release only if note < "Undamped >" key, or Fade > 0, or
  Sustain > 0 with Fade = 0. Note Delay mode 3 ("Note Off"): the voice waits; at note-off it starts with level × exp(−3.45·held/
  timeCurve(Fade Time)); its amp env goes from sustain straight to release.

## Hyperknobs
- Patch knob i: value params 0x0e/0x0f/0x67/0x69/0x6b/0x6d, ofs/rng (0x71/72, 0x74/75, 0x77/78, 0x8b/8c, 0x8e/8f, 0x7c/7d),
  assignment (0x70, 0x73, 0x76, 0x8a, 0x8d, 0x7b) = trunc(v·15.9) − 1 → macro slot (Wave, LFO Rate, Cutoff, Reso, Filt Env,
  Filt Att, Filt Dec, Attack, Decay, Release, Start, Glide, –, Sustain, Filt Sus): offset = (value − ofs)·rng.
- An element with Receive CC/EE uses `clamp(v + offset[kind])` for the params that call FUN_1001c440(kind).
- Element quick knobs A/B (0x0e/0x0f, modes 0x04/0x05: Positive/Negative/Bipolar/Switch) route to 4 params
  (dest = 0x10 + trunc(v·235.9) at 0x06/0x08/0x0a/0x0c, depth 2v−1 at 0x07/0x09/0x0b/0x0d), applied before the macros.

## Mod matrix
Rows: src/mod trunc(v·71.9), param 2v−1, dest trunc(v·15.9), depth 4(v−½)|v−½|. Normal row (mod < 29):
`out = depth·src·((1−p) + p·mod)` (p > 0) or `depth·src·(1 + p·mod)`, added to the dest. The Amp dest is multiplicative
(`amp *= 1 + out`). Pitch-like dests ×ln2 (octaves); Cutoff ×20·ln2 unless the source is Key/Pitch. Modifiers ≥ 29 are processors
(Add … X-Fade): see `SampleVoice::processSlot` (port of `FUN_10049090`) and `setupModSlot` (`FUN_1001c570`).
Sources/dests: `SampleVoice::sourcePtr/destPtr` (`FUN_10010930` / `FUN_10010ba0`). A note-on pass of the matrix
(`FUN_10049dd0`) moves the start point through the Wave dest.

## Output chain
Part: `Level²·Trim²·8`, pan law `x=(p−p²)·0.83, L=(1−p+x)g, R=(p+x)g`. Master: `0.25·(8 − 2.2·drive)/4`, drive = 0.33.
Master tone enhancer (`FUN_1007bcf0`): `y = x + g·(x − x[n−k] + lp2[n−k])`, g = 2.2·drive, lp2 = two one-poles with a = 700/sr,
k = 1 (< 64 kHz) or 2.

## Still to read
`10049090` (processed mod modifiers), where v[0x88], v[0x8b], v[0x8c], v[0x8d], v[0x8e], v[0x3e] and v[0x27]
(filter tap per mode) are set, the compressed-codec render loop (modes 0x10/0x13/0x14), stereo variants.
