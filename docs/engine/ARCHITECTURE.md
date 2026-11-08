# Hypersonic 2 engine map

Taken from `Hypersonic.dll` (2.0.0.600, 04 Oct 2005). Addresses are Ghidra `FUN_xxxxxxxx`. `tools/ghidra/calltree.py` prints
call trees. `docs/engine/element_setter.c` is the element parameter setter with each `case` labelled by parameter.

## Plugin shell (VST 2 SDK)
| What | Address |
|---|---|
| `AudioEffect` ctor (AEffect: magic `VstP`, trampolines) | `100010e0` |
| `AudioEffectX` ctor | `10001a30` |
| Plugin ctor (logs "Hypersonic 2 2.0.0.600"; reads `HKCU\Software\Steinberg\Hypersonic 2\Debug` for logging) | `1008bad0` |
| Plugin vtable | `103b8f38` (slot 4 `process` = `10088690`, slot 5 `processReplacing` = `1008d400`) |
| MIDI event handler (CC, bank select, program change) | `1008d5e0` |

## Audio path
```
1008d400 processReplacing
  1008cc20  MIDI event queue
  1007bcf0  main render (per part)
    100191b0  part render
      100485d0 -> 10046da0  sample streaming / decoding (compressed codec + PCM)
      100139c0              voice render: switch on voice->mode (voices are 0x6B8 bytes, array at this+0x100c)
      10017880              note on/off -> 10014910 voice start (per element type) ; 1004a340 env/LFO?
```

### Voice render modes (`100139c0`), set by note-on `10014910`
| mode | function | meaning |
|---|---|---|
| 1 | `10051470` | Drawbar |
| 2 | `1004b2a0` | Sample (generic) |
| 3 | `1004c550` | Wavetable |
| 4 | `1004c920` | FM |
| 5 | `1004cc00` | Analog, plain path |
| 0x10 | `1004b0e0` | Sample, compressed codec, used when filter mode == 2 |
| 0x11 / 0x12 | `1004b490` / `1004b670` | Sample, PCM (header +0xd == 2 → 0x11, 4 → 0x12) |
| 0x13 / 0x14 | `1004b830` / `1004bb10` | Sample, compressed, stereo (0x14 − flag) |
| 0x15 / 0x16 / 0x17 | `1004be20` / `1004c0b0` / `1004c2f0` | Sample, PCM stereo variants |
| −4 | `1004fc10` | Analog, full path: Osc3→filter FM, filter distortion, highpass, Saw Unison / Wavetable mode, Osc1 phase inverted |
| −3 | `1004e400` | Analog, "Analog" filter, filter modes 3 / 6 |
| −2 | `1004d7a0` | Analog, "Analog" filter, filter modes 1 / 2 / 8 |
All render functions call `10049eb0` (probably the per-block control update) and `1004ab60` (112 float operations,
probably the shared filter/amp/pan stage).

## Constants and conventions
- `setSampleRate` = `10024fa0`: obj+4 sr, +8 1/sr, +0xc 32/sr, **+0x10 sr/32 (control rate, 32-sample blocks)**,
  +0x14 = ln(261.62558/sr) (log-frequency reference = middle C).
- Pitch and cutoff are natural-log frequency. One semitone = 0.057762265 (ln2/12); octave = 0.693147.
- Cutoff (elem 0x6a): `ln f = v * 10*ln2 + ln(261.626/sr)` (+ keytrack/env/mod at voice level, TBD).
  Env2 depth (0x6e): ±10 octaves, `(2v−1)*10*ln2`. Cutoff Key (0x6b): `4v − 1`. Random cutoff (0x6d): `v²/2`.
- Mod matrix (elem 0xac + 5*slot): src/mod `trunc(v*71.9)`, param `2v−1`, dest `trunc(v*15.9)`,
  depth `4·(v−½)·|v−½|`.
- `FUN_1001c440(k, v)`: applies GM-mode macro offsets (k > 0) or element quick-knob routings (k < 0) to v.
  With neither active it returns v.
- float→int is truncation (see NOTES.md).

## Sample voice start (`10014910`, element type 2)
- Start point: checkpoint `i = trunc((rand*randAmt*0.5 + startpoint − velAmt*vel + key/other + …) * 63)`, clamped
  to 0..63. Compressed samples jump to frame `(len>>12)*i*i` (or `(len*i*i)>>12` for short ones) and restore the decoder state
  from the seek table: y = ((u & 0xFFFFFF) − 0x200000) · 2⁻²¹, k = u >> 24. This matches `milan_decode.py`.
- Pitch: `ln(rate_scale · sample_rate) + ((60 − root) + tune) · ln2/12`. Zones flagged 0x200 use a root taken from the
  element's key range (centre, or ±12 from the edges for wide ranges).

## Still to read
- `10049eb0`, `1004ab60` (shared voice stages); the sample render loops (interpolation, loop handling, codec in the loop?).
- Filter implementations per mode (16 modes, Digital/Analog variants); envelopes and LFOs (`1004a340`?); the mod matrix runtime.
- Effects: insert/send FX (types listed in NOTES.md), Effect element type 0.
- Arpeggiator (`CHyperArp`), phrases (H banks), drawbar organ model, the piano "Model" type 6.
