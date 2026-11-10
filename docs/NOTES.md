# Hypersonic 2 RE notes

Personal-use port to JUCE (Win + Mac). Content is loaded from the user's own
install at runtime, never bundled or committed.

## Files

| What | Path |
|---|---|
| Plugin | `C:\Program Files (x86)\Steinberg\Hypersonic 2\Hypersonic.dll` (also copied to `...\Steinberg\VstPlugins\`) |
| Standalone | `...\Hypersonic 2\Hypersonic.exe` |
| Content | `C:\Hypersonic 2 Content\` (registry `HKLM\SOFTWARE\WOW6432Node\Steinberg\Hypersonic 2` → `Content Location`) |

## Hypersonic.dll

- PE32 i386, image base `0x10000000`, linker 7.10 (VS .NET 2003), MSVCR71/MSVCP71.
- PE timestamp `1128419235` = 2005-10-04.
- Exports: `main` (VST 2 entry) + COM boilerplate.
- Copy protection: Syncrosoft. The strings `SYNSOACC.DLL could not be located…` and `…does not have the expected interface` are present.
- RTTI is stripped. Only the std exception classes remain.
- Sections: `.text` 0xA4000 (entropy 6.5, normal code). `.rdata` 0x31A000 with ~2.5 MB at entropy ~8.0: either encrypted code or data (protection) or compressed assets. **TBD.** `.rsrc` 0x381000, probably GUI bitmaps.
- Useful strings:
  - Parameter names: `Vel to Resonance`, `Osc3 Filter Mod`, `Velocity X-fade`…
  - Patch parser: `Warning - Patch parameter %d = %.4f`, `unexpected patch data endmark %d`.
  - Effect names (see below).
  - `CHyperArp::setText`, which leaks a class name.
- Internal engine name: **Milan** (Wizoo). Chunk tags `StSS`, `MilE`.
- `LoadLibraryA`/`GetProcAddress` are imported, so SYNSOACC.DLL is loaded dynamically.
- Resources: 82 `RT_BITMAP` (3.5 MB, GUI), `RT_VERSION`, and a **`VSTXML`** resource (120 KB).
  - VSTXML: "XML definition of VST parameters for Hypersonic 2, by Paul Kellett, Wizoo, 30 March 2005".
  - 2384 params. Param 0 is Master Volume. Then **16 channels**, each with: Mute, Level, Pan, Output,
    FX Send 1-4, FX Mode, Hyperknob 1-6, MIDI (Mono/Glide/Key+Vel range & xfade/Tune/Polyphony/Pitchbend/Level Trim),
    Arpeggiator (On/Latch/Mode/Octaves/Steps/Sync/Rate/Gate/Swing), Microtuning (2 tunings) and
    Patch Send FX 1-4 (16 generic params each).
  - After the channels: Global Send FX 1-4 (16 params + Mute/Level/Pan/Output/Sends), ids up to 2527.
  - Deep synth params (osc/filter/env/LFO) are **not** host-exposed. Extract with:
    `python -I` + pefile, resource type `VSTXML`.

### Effects named in strings

Reverb, Hall Reverb, Room Reverb, Plate Reverb, Reverse Reverb, Non-Linear Reverb, Short/Long Reverb,
Stereo/Long/Tape/Ducking/Grain/Diffuser Delay, Chorus, Quad Chorus, Space Chorus, Flanger, Step Flanger,
Phaser, Deep Phaser, Bi-Phaser, Tuned Resonators, Vintage Filter, Shelf EQ, Parametric EQ, Peak EQ,
Compressor, Multiband Comp, Compand, Distortion, Chorus + Reverb, Bit Crusher, plus GM Reverb, GM Chorus 1-4,
GM FB Chorus, GM Flanger and GM Plate Reverb.

## Content formats

### Samples (`Samples\`, 56 files): solved
See `extract script/milan_decode.py` and `extract script/readme.txt`.

### Milan Bank container: solved (all 125 banks verified)
Every extensionless content file uses this container. The parser and full layout are in
`tools/formats/milanbank.py`, taken from `FUN_10019290` (open) and `FUN_1001a700` (streaming loader).
- 0x58-byte header: signature (char 16 = S/P/H/W/D/T), `u16 kind @0x30`, `u16 version @0x32`
  (13 = sizes ^0xDEADBEEF, offsets ^0xBADEAFFE), `u32 info @0x50`, `u32 dir_size @0x54`.
- Directory block @0x58: count, name-table offset, secondary-table offset, secondary count, then 0x30-byte
  entries with name, name2 and two (flag, offset, size) slots.
- Secondary table: 0x20-byte records (sample data in S banks). Record 0 is a sentinel.
- Kind values: P=1, S=2, W=3, T=4, D=5, H=6. Slot 2 is used only by S banks (instrument/zone blocks).
- Hypersonic streams entries in and out against a memory budget. IDs: entry = i, slot 2 = i + 0x1000000,
  secondary = i + 0x2000000.

### Patch blob (slot 1 of a P-bank entry): structure solved
Parser: `tools/formats/patch.py` (`python -I tools/formats/patch.py "<bank>" <index>` dumps named JSON).
Source: `FUN_10016f70` (patch load). The 2,685 factory patches match
`size = 0x620 + (elements + send FX) * 0x420 + 12`. Open questions: 241 entries are not `StSS`, and about 50 have extra trailing data.
- Header: version @4, element count @0x14 (**inserts count as elements**), send-FX flags u8[4] @0x18.
- Strings: name @0x20, short name @0x40, description @0x80, controller notes @0x100, author @0x180,
  6 quick-control names (0x10 each) @0x1c0.
- 0x100 patch floats @0x220, then 0x420-byte records: char[0x20] name + 0x100 floats.
- Element param 1: type = `trunc(v*15.9)`: 0 Effect (inserts, drum "GROUP" buses), 1 Drawbar, 2 Sample,
  3 Wavetable, 4 FM, 5 Analog, 6 Model. The fractional part varies within a type; TBD.
- Loader fixups: version < 0xA87569 → element param 0x7d (Note-on Click) *= 0.3, or 0 if > 0.99 (the decompile shows word 0x85 of the record, which counts the 0x20-byte name: 0x85 - 8 = 0x7d); version < 0xBEC874C → patch p[0x18] = 0.5;
  type 5 (Analog) resets several params unless patch mode (in_ECX+0x4b4) is 1 or 5; LFO param 0x54 > 0.99 → default.
  The port must replicate these.

### Parameter names: `docs/params.json`
Generated by `tools/ghidra/extract_param_names.py` from `FUN_1000cee0` (getParamName, patch level) and
`FUN_1001cb60` (element; byte table @0x1001eaf0 → switch).
- Element idx 0x11..0x30 are type-specific: real idx = idx + (type+8)*0x20.
- Element idx ≥ 0xac: mod matrix, 16 slots × (Src, Mod, Param, Dest, Depth).
- Patch idx ≥ 0xac: arpeggiator, 16 steps × (Note, Vel, Gate, Seq1, Seq2).
- Value→text: `FUN_1001ecd0` (element; reads value from obj+0x18+idx*4, type int at obj+0x424) and
  `FUN_1000dac0(elem, idx, out, value)` (patch).
- **These run in an emulator** (`tools/emu/hs2emu.py`, Unicorn; DLL mapped at its native base, `sprintf` stubbed
  in Python, `floor` via a native x87 stub). `tools/emu/sweep_display.py` sweeps every param at 1025 points →
  `docs/param_display.json`. `tools/emu/params_md.py` renders that as **`docs/PARAMS.md`, the parameter reference.**
  Content-name params (sample bank/wave, wavetable, scale, arp step notes) need runtime objects. Their index is
  `trunc(v*128.9)` into the bank directory, via `FUN_1001b980(kind, bank, entry)`, where kind matches the bank kind numbers.
- **Float→int is truncation everywhere.** Ghidra prints `ROUND()`, but the code saves the FPU control word, sets RC=chop
  (`or ah,0xC`), runs `fistp`, then restores it. Stored enum values are bin centres `(n+0.5)/K`.
- The display shows Cutoff/Resonance etc. only as %, so the real Hz/Q mappings must come from the engine (setter `FUN_100121a0` and the DSP).
  - Then: name, description, comment (`MW: … | AT : …`) and author, as fixed 0x40-byte NUL-padded fields.
  - Then: Quick-control names (`Attack Shape`, `Fretnoise`, `Tone`, …).
  - Then: a large array of float32 values, mostly normalized 0..1.
- Blob size varies (0x128C, 0x16AC, 0x1EEC…), probably one repeated block per element.

### Combi / patch `.fxb` (`Patches\120 Combis\*.fxb`)
- Standard VST 2 bank: `CcnK`/`FBCh`, fxID `StSS`, opaque chunk.
- The chunk is an `StSS` bank chunk ('B', 1 at +0xc): see "Engine container, combis (2026-11-08, fourth round)".

### Effect presets (`Effects\<Type>\*.fxp`)
- VST 2 `CcnK`/`FPCh` program chunk with fxID `MilE`. The inner chunk is also tagged `MilE`, followed by the effect id (e.g. `0x3EB`),
  the name, knob labels (`KNOB 1`, …) and float params.

### TBD
Phrases (11), Tunings (4), Drawbars (1, 2.2 MB), Wavetables (1, 3.2 MB). (Combi Chains `.h2c`: see the fourth round.)

## Ghidra workflow
- Ghidra 12.1.2 is at `C:\programs\ghidra_12.1.2_PUBLIC` and needs `JAVA_HOME` = scoop `temurin21-jdk`.
- Project: `ghidra/Hypersonic2.gpr`. It analyzes a **copy** at `ghidra/bin/Hypersonic.dll`, because the batch launcher
  breaks on `(x86)` in paths.
- `tools/ghidra/DumpDecomp.java` writes `ghidra/decomp/all.c` (all 1881 functions) and `strings.tsv`
  (string → referencing functions). Use these for text searches. Re-run it after renaming things in the GUI:
  `analyzeHeadless.bat ghidra Hypersonic2 -process Hypersonic.dll -noanalysis -readOnly -scriptPath tools/ghidra -postScript DumpDecomp.java ghidra/decomp`
- Known functions: `FUN_10019290` bank open, `FUN_1001a700` bank stream load/unload,
  `FUN_10016f70` patch load, `FUN_100121a0(elem, idx, value)` set param (elem 0 = patch),
  `FUN_1000cee0`/`FUN_1001cb60` param names, `FUN_1000dac0`/`FUN_1001ecd0` param display,
  `FUN_1008d5e0` MIDI event handler, `FUN_10052d30`/`FUN_10052db0` SYNSOACC loading,
  `0x1008e09c` = MSVC `_ftol` (truncating float→int).
- Protection: Syncrosoft is a license check only. All 1881 functions decompile and the DSP is plain code. The high-entropy
  `.rdata` is lookup tables referenced directly by code.

## MIDI
- Bank select: bank = CC0*128 + CC32; request = bank*256 + program. Factory bank NNN = CC0 0, CC32 NNN.
- The patch change is queued (`+0x10000` pending flag) and loaded asynchronously, with samples streamed in real time.

## Reference renders (REAPER)
- `tools/reaper/make_job.py` → `job.tsv`. `hs2_render.lua` builds one region per patch (CC0/CC32/PC, 4 s load gap,
  fixed test notes) and renders `B###_P###.wav`. `check_renders.py` flags silent notes and duplicate renders.
- Full-speed offline render outruns sample streaming, so the first notes of a newly loaded patch can be missing. **Must render at 1x.**
- Repeat renders of the same patch are not bit-identical (free LFOs/random). Compare levels and spectra, not nulls.
- **The user's REAPER is their production DAW.** Never use `-cfgfile` and never run a REAPER installer: the silent installer ignores
  `/PORTABLE` and hijacks shortcuts and file associations.
- **Render copy:** `C:\programs\reaper-render`, REAPER 7.83 extracted from the installer with 7-Zip (application files only:
  no uninstaller, no ReWire/ReaRoute DLLs), with its own `reaper.ini` (portable marker; scans only the Steinberg
  VstPlugins folder). Nothing is registered. `run_render.ps1` launches only this copy and refuses anything else.
- Render speed is a per-project setting (`RENDER_1X` in the .rpp; the ini's `projrenderlimit` does not apply).
  `hs2_render.lua` saves the project, patches `RENDER_1X 1 0`, reopens it with `noprompt:`, then renders. About 20 s per patch.

## C++ engine core (`core/`, `tools/cli/`)
Build (MinGW GCC 16 from scoop msys2, Ninja):
```
$env:PATH = "$env:USERPROFILE\scoop\apps\msys2\current\mingw64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\tools\cli\hs2render.exe --bank 7 --patch 0 --out reference\port\B007_P000.wav
```
`hs2render` plays the same test sequence and 4 s lead-in as `tools/reaper/hs2_render.lua`. Compare against a reference render with
`python -I tools/reaper/compare.py <ref.wav> <port.wav>`.

Status (2026-11-07):
- **Done:** bank container, patch parser, sample banks (zones, codec, PCM), Sample element voice (pitch,
  start point, 3-pole/4-pole filter with all 9 modes, filter and amp envelopes, velocity curves, pan, level).
  Part output stage: patch gain `Level²·Trim²·8`, patch pan law, master `b54²·(8−2.2·drive)/4` (b54 = 0.5, drive = 0.33).
- **Since the first A/B round** (2026-11-07, later): Hyperknob macros + element quick knobs (`FUN_1000cc40`/`FUN_1001c440`),
  zone selection by transposed key and zone velocity with key/vel crossfade tables (`FUN_10021e20`, `FUN_10048b80`),
  the note-off rules ("Undamped >" key, one-shots), release-triggered elements (Note Delay "Note Off"), the full mod
  matrix (`FUN_1001c570` setup / `FUN_10049090` 29 processing modifiers), LFO1/2 (patch) + LFO3 (voice), Env1, and the
  **master tone enhancer** (`FUN_1007bcf0`): `y = x + g·(x − x[n−1] + lp2[n−1])`, with g = 2.2·drive and lp2 = two one-poles at
  a = 700/sr. It adds treble and bass, which explained all the remaining HF/low-note differences.
  Current A/B: **Natural Grand Piano: every note within 0.6 dB, spread 0.2 dB, envelope correlation 0.984.**
  **Noise Band: within 0.1 dB except C6 (−1.2 dB), correlation 0.996.** Soft Rubber Bass ±3–4 dB (Parametric EQ insert
  not implemented). True Sustain As Reverb −4 dB, decays too fast: it relies on the patch-level piano model
  (bank 082: Patch Type / Piano Model / Redamper / Piano Reso, "Res (Always Sus)" layers). Ported in the third
  round: against the batch-1 REAPER render 82/14 now +0.4 dB mean, spread 0.5 dB, correlation 0.871 (was +4.0 / 2.5 / 0.757).
- **Effects** (2026-11-07): the routing is ported (layers + inserts on one bus in patch order, Aux Out/group-bus clear,
  element sends, patch's own send FX with return `4·level`/pan and FX4→FX1 chaining; `PatchPlayer::renderChunk`).
  The effect factory `FUN_10043e20` has 53 types (table: `docs/engine/effects_table.json`; all vtable methods are decompiled
  in `ghidra/decomp/effects.c`, created by `tools/ghidra/DecompAt.java`).
  **Effect oracle:** `tools/emu/fx_oracle.py` runs the ORIGINAL effect in the emulator (CRT stubs: new/malloc/realloc,
  memmove, rand, x87 `_CIfmod`/`_CIpow`/`floor`). `tools/emu/fx_compare.py` checks a port against it sample by sample
  (`tools/cli/fxtest`). Status: see "All 52 effects" below.
- **Batch 2** (`reference/renders/batch2`, 35 sample-based patches from 18 banks; the render copy crashed on bank 32
  patch 6): `tools/reaper/compare_batch.py`. **29/35 within 1.5 dB mean / 1.5 dB spread / correlation > 0.9**, many
  within 0.1 dB at 0.999 (Clavinet 0.0 dB / 1.000). Open: the **global send FX** (most patches; defaults unknown),
  Delay (10), Chorus (20), Wah (71); mallets/bells/church organ come out **darker** in the port (centroid ratio 0.45–0.7).
- **Global send FX = factory combi 0** (2026-11-07). A fresh plugin, as REAPER loads it, holds combi 0 "Arctic Bubbles Scape":
  FX1 "Long Reverb" and FX2 "Short Reverb" (both Hall Reverb, 100 % wet, level 0.5, pan 0.5). The return is 4·level with linear pan,
  even with Aux Out "Off". With this, release tails match within 0.1–0.5 dB. (A factory reset, `FUN_1008c820`, would instead
  mute them and set type 128 = none.) The plugin state chunk in .rpp files is Syncrosoft-encrypted, so it was not used.
  **Explained in the fourth round:** these are not combi 0's: the patch loader puts a patch's send FX records with flag 2
  ("Long Reverb", "Short Reverb", ...) into the global part's empty slots. See "Engine container, combis".
- **All 52 effects ported and oracle-verified (2026-11-07)**, one file each in `core/src/fx/fx_NNN_*.cpp`
  (registry `fx/registry.h`, shared base and porting rules `fx/effect_base.h`). Ported by six parallel agents, then
  re-verified independently: **all 52 are bit-exact** (fx_compare floor, −240 dB) on fresh random sets plus all-0 /
  all-1 / all-0.5, the note-pitch effects also with `--pitch`, the tempo effects also with `--tempo 120` and sync on.
  Each file header lists the original addresses, the algorithm, quirks kept on purpose and its verification result.
  - **Precision:** the original runs the x87 at **53-bit precision** (MSVC default control word 0x027F). Mirroring the
    disassembly exactly (register values = double, float stores at the same points, `fst` keeps the unrounded register,
    exp via the fldl2e/f2xm1/fscale sequence = `x87exp`) gives bit-exact output. No `long double` needed (MSVC-safe).
    `tools/emu/disasm.py` prints a function with its float constants resolved.
  - **Note pitch (vtable +8, `Effect::setPitchSource`):** the element connects the part's note pitch (part+0x968 =
    newest held note·ln2/12 + transpose/bend, −10 when no note is held) before setSampleRate, whenever part+0x964 < 0.5
    (normally always). Used by: Tuned Resonators 7 (pitch tracking), Grain Delay 15 (grain rate = note when Length < 0.01),
    Amp Simulator 103 (feedback delay becomes a comb tuned to the note), Vintage Filter 70; Ducking Delay 14 clears it in
    setSampleRate (so normally inactive); Rotary Speakers 55/56 only write a horn-position value through it.
    `PatchPlayer` tracks held notes and connects it (transpose/bend TODO). `fx_compare --pitch` tests it with a note timeline.
  - **Tempo (vtable +0xc):** the part calls it with (part+0x958 bpm, part+0x95c beats per sample), defaults 120 bpm,
    from `FUN_1000ef10` on host tempo changes and in the element setter before +8 and setSampleRate. All 12 effects with
    a tempo method are bit-exact against the original with `fx_compare --tempo 120` and sync on. `PatchPlayer` passes
    120 bpm (TODO: host tempo). Wah Pedal, TalkBox and Step Flanger sync also read the host song position
    (`process()` args 3/4, a double); the port passes 0, as the oracle does. TODO: host transport.
  - Batch 2 after all effects: 29/35. Strat Muted Wah now runs its Wah (envelope mode; −0.9 dB, corr 0.94, about
    the same as the same guitar without the Wah). Moving (+2.5 dB) and Analogy (brightness 2.7) use only Chorus +
    Delay, both exact, so their error is in their three sample layers. Open.
  - Original quirks found: Tuned Resonators with tracking on loops forever if processed before any note (scale 0; the
    plugin avoids it because silent effects are not processed); Pitch Shift's tempo method re-applies the wrong param;
    many float-bit-mask clamps (`&0xc0000000`) and constants that differ by one ulp; all kept.
  - Emulator: `hs2emu` maps a flat GDT + FS/TEB so constructors with SEH frames run (Chorus + Reverb).
- **Trigger Condition** (element 0x37 → +0x438, `trunc(v·15.9)`), checked in `FUN_10021e20` after the key gain:
  1/2 mod wheel up/down (part+0xda0 > 0.5), 3/4 sustain up/down (part+0xd98), 5/6 odd/even patch note
  (part+0xb7c note counter parity, read before its increment), 7/8 odd/even element note (+0xb6c, counted when the
  key gain is non-zero), 9 "No Arp" (not checked here). Ported in `PatchPlayer::noteOn`. This silenced Analogy's
  "Mod Wheel Up" layer: Analogy's brightness ratio went from 2.69 (too bright) to 0.51 (too dark).
- **ECO / XXL** (element 0x03 → +0x434; patch param 1 = part quality `trunc(v·2.9)`: 0 all ECO, 1 per element,
  2 all XXL): ECO uses an instrument's second zone set (bank entry slot 2, present for 142 of 1567 instruments),
  XXL the first (`FUN_10019c20`). Ported (`Instrument::ecoZones`). No batch-2 patch uses ECO.
- **Note-on Click** (0x7d → +0x4f0): only acts when the amp attack (+0x578) < 0.0005 and click > 0.01: a matrix pass
  with amp = 1, the amp env starts in decay, the gain ramps start at (0.0005 − attack)·level·peak·amp·click·8000.
  Ported for every type (2026-11-08).
- **Resonance pattern:** every patch that comes out too dark uses filter resonance (Analogy 29 %, organ 12/19 %,
  Melody Bells 64 %). The ones that match use 0–3 %. Suspect the voice filter's damping/coefficients
  (voice.cpp `controlUpdate`). A voice/part oracle in the emulator is being built to settle it.
- **Moving** (+2.5 dB): Hyperknob 2 (source 0.5) crossfades layers A/B via Amp rows of depth ±1 (A through a Curve
  modifier). The level depends on the knob-source scaling and Amp modulation; check with the voice oracle.
- **Part oracle (2026-11-08)**: `tools/emu/part_oracle.py` runs the ORIGINAL engine end to end in the emulator:
  - **Setup:** content manager, real bank loading (host file reads behind fopen/fread stubs; the Syncrosoft entry reader
    `10019fb0` is replaced by a host read of the same bytes), synth ctor, `Engine::loadPatch`, streaming.
  - **Driving it:** MIDI goes to `10017880`, the part render `100191b0`/`100139c0` produces the audio, `--tempo` calls the
    host tempo sync `1000ef10`.
  - **Output:** the part's dry main out (no global send FX, no tone enhancer).
  - **Comparing:** `hs2render --dry --notes ... --solo ...` gives the matching port render; `part_compare.py` compares
    the two per segment.
  - **Validation:** Clavinet and Melody Bells single notes −96 to −101 dB.
  - **Fixes it found** (all applied):
    - Element quick knobs A/B take the patch Hyperknob chosen by "1./2. Source" (0x91/0x92); they also reach insert-effect
      knobs.
    - Note Delay mode 1.
    - Patch Transpose (0x37 → part+0xbb4, used for zone/key lookup; voice note pitch gets it twice, an original quirk).
    - Triangle LFO init `fmod(2p+0.5, 2)`.
    - The compressed decoder was one sample late.
    - The start index seeks only for idx > 0.
    - Pan-law level compensation at note-on.
    - The real voice-end test (amp-env flag + level < 1e-4, or a one-shot that has ended).
    - rateLn precision.
    - One extra rand draw per voice.
  - **Batch 2 result: 33/35.** Large Church Organ, Melody Bells and the basses are now at 0.0 dB / correlation 1.000.
- **Glide** (element 0x4c..0x52; note-on `10014910` @10014d6c, control `1004ab60`, note-off `10017880`):
  - Glide Mode: 0 Off, 1 Bend Only, 2 Fingered, 3 On. Glide Type Analog (Resume): note-off freezes the glide.
  - Each control block: `glide += (target − glide)·coef`, coef = min(exp(VelToGlide·vel)·timeCoef(Glide Time), 1).
  - Attack Bend (`(v−½)|v−½|·8.3178` ln pitch) is added to the start, scaled by `VelToBend·vel + 1 − |VelToBend|`.
  - Release Bend is added to the target at note-off.
  - Legato starts from the element's previous target (+0x4f8). The rule: Fingered = another note is held, On = always.
    When the element receives CC/EE (0x02), the patch Glide (patch 0x03, part+0xbc0) overrides it: Off = none,
    1 = element, 2 = Fingered, 3 = On.
  - Moving's A/B layers now match the oracle (−68 / −45 dB).
- **Tempo sync:**
  - **Sync modes:** 0 Off, 1 First Note (retrigger when < 2 notes are held), 2 Each Note, 3 Tempo + Note, 4 Tempo + Beat.
  - **Rate:** tempo modes run at `bpm · noteValue(Rate) · blockT / 30`, where noteValue = `FUN_100489b0`, the delays' table.
  - **Patch LFO1/2:** the host sync `1000ef10` applies the tempo rate. It runs only when the tempo, time signature or
    transport changes. In Tempo + Beat it also sets the phase: `2·fmod((ppq − barStart)·noteValue + Phase·1.1111, 1)`.
  - **Element LFO3:** voice rate = part bpm (part+0xd90) × elem+0x4c4.
  - **Port:** fixed at 120 bpm with the transport at a bar line, as in the REAPER renders. TODO: host transport.
- **Batch 2 after glide, tempo sync and silence gating (2026-11-08): 35/35** within tolerance; most at 0.0 dB / correlation
  1.000. Analogy, Moving and Dyno Chorus were fixed by the gating: their Chorus LFO no longer runs through the
  silence before a note. Remaining small residuals: Subsonic Church Organ (correlation 0.969) and Strat Muted Wah
  (0.981, brightness 1.26).
- **Effect silence gating** (part render protocol):
  - **The flag:** each insert/send gets a "silent" flag. The part starts it set, a sounding voice clears it, and an Aux Out
    that clears the bus sets it again. Send FX get "silent" when nothing was sent to their bus.
  - **Standard pattern:** on signal, reload a countdown (+8) from the tail length (+0xc, default 200000 samples; many
    setSampleRates use sr·0.01…0.5). While silent, skip processing once the countdown is below 0, otherwise count down,
    process and clear the flag.
  - **Detect pattern (reverbs, delays):** the flag stays set while counting down. After processing, an audible output
    (left channel 2|x| > 1e-5, `FUN_10025480`) reloads the countdown and clears the flag.
  - **Port:** `Effect::processGated` + `EffectBase::Gate`; `fx_compare --gated` tests audio and returned flags.
  - **Why it matters:** without it, a Chorus LFO kept running through silence (Moving −3.9 dB vs the oracle).
  - **Per-effect variants found with `fx_compare --gated`** (all now exact, audio and returned flags):
    - Reverbs 0–3 and 7, Delays 10–13: Detect.
    - Ducking and Grain Delay: Detect, reloading with tail + line length.
    - Reverse Reverb, Diffuser Delay, Bi-Phaser and Waveshaper: no gating at all (always process, flag untouched).
    - Scanner Vibrato: its bypass mode returns before the gate.
    - Rotary Speakers keep the rotor turning on skipped blocks.
    - Wah, TalkBox, Limiter, Compressor and Multiband clear their envelopes on skipped blocks.
    - Tube Drive snaps its gain to the target.
    - Vintage Filter adds its +0.012 resonance only when the flag is clear.
    - Tail constants are floats (sr·0.01f = 440 at 44.1 kHz).
    - Pan / Tremolo (depth 0) and Amp Simulator (mix 0) return before the gate, leaving the flag set.
    - Amp Simulator: a newly held note pitch clears the flag itself, so the comb starts ringing.
  - **Final check (2026-11-08):** all 52 effects are bit-exact (−240 dB, 0 flag mismatches; Delay −154 dB) on fresh
    random sets plus all-0 and all-1, gated and normal, with --pitch where used.
- (Resolved 2026-11-08: Large Church Organ's darkness was the element quick-knob source routing, see Part oracle.)
- Earlier A/B notes, for history (`reference/renders/batch1`, rendered 2026-11-07 with the portable render copy at 1x):
  - Natural Grand Piano: per-note level within 0.4 dB (except C2, −2.4 dB). Bands 63 Hz..2 kHz within ±0.7 dB at
    every velocity; envelope correlation 0.968. The original has ~+3 dB more at 4–8 kHz (its send reverb? interpolation?).
  - Noise Band test patch: C4/v30/v127/chord identical (0.0–0.2 dB). C2 −2.8 dB. C6 is far off: keytrack ×2 puts it
    6 octaves up, where pitch and cutoff are clamped; the original keeps much more HF.
  - True Sustain As Reverb (9 layers): +0.8 dB mean but spread 1.8 dB; layer selection/crossfades not modelled.
  - Soft Rubber Bass: −11 dB and decays where the original sustains. Most likely the Hyperknob / quick-control
    routings (`FUN_1001c440`) that offset element params; not implemented.
- (Resolved: the C2 and HF differences above came from the master tone enhancer and zone selection.)
- **Not yet:** the Model element type (zone velocity modes 1/5 and the piano model: see the third round). (Note Delay mode 2, host
  tempo, MIDI controllers and the arpeggiator: see "MIDI, transport and arpeggiator".) (Polyphony, mono modes,
  glide, patch transpose and the other element types are done: see the sections below.)

## Element types (2026-11-08)
Factory usage, out of 2747 patches:

| Element type | Elements | Patches using it |
|---|---|---|
| Sample | 7305 | 1819 |
| Analog | 1392 | 911 |
| Wavetable | 216 | 156 |
| FM | 168 | 125 |
| Drawbar | 66 | 66 |
| Model | 0 | 0 (not implemented) |

Structure:
- `Voice` (voice.h/.cpp) is the shared engine: note-on, matrix, envelopes, LFOs, glide, filter coefficients.
- The Sample oscillator provides the default hooks: `startOsc`, `oscControl`, `renderOsc`, `oscFinished`, and the
  virtual `controlUpdate`.
- Other types live in `core/src/types/<type>.cpp`. Each provides `<Type>Supported()`, `compute<Type>Params()` (a
  `TypeParams` subclass on `ElementParams::typeParams`) and `make<Type>Voice()`. The registry is
  `hs2/element_types.h`.
- `PatchPlayer` keeps one `std::unique_ptr<Voice>` per slot, re-created when the element type changes.
  TODO(realtime): preallocate.
- `EngineConsts::content` gives the type code access to the content folder (Wavetables, Drawbars banks).
- Refactor check: the batch-2 renders stayed bit-identical (`tools/reaper/regress_identical.py`).
- Single-type test patches:
  - Drawbar: 83/0..5.
  - Wavetable: 15/11, 40/14, 41/19, 0/75, 15/16, 33/30.
  - FM: 43/1, 43/5, 43/6, 43/7 (test tones), 36/36, 40/8.
  - Analog: 0/45, 15/23, 15/68, 15/70, 15/73, 15/80.

### Element type ports (2026-11-08, four parallel agents; integrated build re-verified)
Error vs the part oracle (`part_oracle.py` now opens the Wavetables and Drawbars banks too and strips the Syncrosoft
licence header from entries as the original's reader does).

**FM** (`types/fm.cpp`)
- Two 23-bit integer modulators (s − s³), a polynomial-sine carrier, Thru Mix, depth envelope, key/velocity
  scaling, LF Carrier.
- Test tones −119 to −121 dB; 30 factory FM layers −84 to −114 dB.

**Drawbar** (`types/drawbar.cpp`)
- Data format:
  - Bank kind 5, entries behind a Syncrosoft header.
  - 12 per-semitone tables, each frames of 8 int16 columns = octaves.
  - Three read pointers (key, fifth, third); weights n²·32 + 200.
- Also ported: percussion, key click and off-click, top-bar folding.
- 58 of 66 factory drawbar patches at −94 to −109 dB. The 8 others need mono, the arpeggiator or sequencer, unison,
  or patch-LFO idle behaviour (mono, Rotary knobs and the LFO sleep are now done: 83/9, 19, 27, 35 and 83/0..5 at
  −137..−146 dB; 83/37 needs the sequencer; 83/29 is fixed by the shared rand).

**Wavetable** (`types/wavetable.cpp`)
- Data format: entries of 8 mip levels × 64 waves of (256>>k)+1 int16.
- Osc 1: integer phase 2^23 with mip/wave selection on each wrap, an aliasing XOR mask, sync quirk.
- Osc 2: 8 types.
- Single notes −112 to −146 dB. 138 of 216 factory layers at ≤ −90 dB (median −136); the rest need the
  arpeggiator, patch LFOs, mono or unison.

**Analog** (`types/analog.cpp`)
- Render modes 5 / −2 / −3 / −4. Factory counts: −3: 807, −4: 344, 5: 198, −2: 43.
- BLEP table from the DLL, osc 1/2/3, 7-voice unison saw, wavetable modes, noise, integrators, osc-3 filter FM,
  drive, SVF / analog modes 3/6 / ladder filters, Note-on Click, mono unison stacks.
- Single notes −95 to −124 dB. 15/23 and 15/80 (patch-LFO sleep) now pass, see "Part-level behaviour".

### Exact shared voice engine (2026-11-08)
The shared `Voice` (voice.cpp) and the element setter values (element.cpp) now mirror the x87 at 53 bits, for every
element type, Sample included; the four type files keep only their note-on case (`startOsc`), oscillator, render loop
and (Analog) filter words. Verified against the disassembly and the part oracle:
- Note-on `FUN_10014910` in the original's order: first rand01 → LFO3 value word, velocity rounded once, glide /
  legato / attack bend, v[0x95..0x99] offsets, env peaks, the mono-unison param_7 terms, pan law, Note-on Click
  (all types; Analog seeds its filter states via `onClick`), `FUN_10049eb0` (`refresh`), glide speed.
- Note-on matrix pass `FUN_10049dd0`: **only the Sample case** (@10016298); the Wavetable case has none.
  Destination words are not reset at note-on (only Wave), so the rate words and amp keep the slot's last values.
- Rate words: LFO3 / Env1 rate words persist across notes in a slot (0 in a fresh voice). `PatchPlayer` carries them
  over when it re-creates a slot's voice object for another type (`Voice::inheritSlot`).
- `FUN_10049eb0` at render entry when the element version (+0x5ec) differs from the voice's (+0x520): drops the
  Vel→Glide factor of a fresh voice. The version is modelled per element type (`ElementParams::version`).
- Control update `FUN_1004ab60`, env stages `FUN_1004a340`, LFO `FUN_10049a60`/`FUN_10048e10` (also used by the
  patch LFOs), processed modifiers `FUN_10049090` (all cases), mod-row setup `FUN_1001c570`, the sample render loop.
- Element setter: v - 0.5 stays in the register for Cents, Attack/Release Bend and mod depth; elem+0xc / +0x14 come
  from the unrounded 1/sr; the LFO3 Rate macro needs a global flag (off by default).
- Alternate pan uses the element's note counter (+0xb6c). The voice-end loop skip applies to every type.
- Compressed samples: the start-point seek is not rounded to an even frame (only PCM is).
Result: Sample single notes went from about −100 to −120..−146 dB (masked), e.g. Clavinet −103 → −138, Soft Strings
Velo Attack −117 → −146, FM Electric Piano's Tine layer at low velocity −36 → −76 dB; the four types are unchanged or
better (Analog 15/70 −128 → −146).

### Part-level behaviour (2026-11-08, `synth.cpp`, verified against the part oracle)
Every item below is checked sample by sample with dense note sets (overlaps, repeats, chords, mono lines, 40
overlapping notes): 75 test patches at −137..−147 dB, unmasked (scratch `ab2.py`; failures listed under "Open").
- **Voice list** (part+0xe08 count, part+0xe0c 8-byte entries: slot, element, note, held note / 0 released / −1
  sustained, mute group): the 64 slots are a permutation (identity in a new part); playing entries are sorted by
  element, and the next free slot is the one at position count, i.e. the most recently freed one.
  - **Allocation `FUN_100102b0`:** a quiet voice (released or `+0x534 == 1`, amp env level < 0.03; the minimum over
    the part) is stolen (`FUN_100101a0`: Voice Mute Fadeout, `+0x534 = 2`, stamp `+0x528` = part note counter
    `+0xb7c`, which then increments) and a new slot is used. At Max Voices (element 0x3a, `1 + trunc(raw v·63.9)`):
    the element's own quiet voice, else its oldest voice of the lowest priority class. Priority class = Priority
    (0x90: 0 / 1 / 6) + 1 released, + 2 held, + 3 stolen. Polyphony = patch Max Voices (0x3a, part+0xbb0): a new
    slot below it, steal-and-new below it + 6, else the stolen entry's slot is reused.
  - **Removal `FUN_10010130`** in the two end-test loops of `100139c0` (the entry after a removed one is skipped).
  - Render order = list order, each voice after the inserts of the elements before its own (`100139c0`).
  - Slots keep their words: rate words, amp, LFO1/2 words (the note-on does not set them) and the PCM
    interpolation words y0/slope (only the compressed branch clears them: a PCM note blends in the slot's last
    values).
- **Note on/off `FUN_10017880`:** held lists part+0x20 (order) / part+0xa0 (sorted); part+0x968 = the highest held
  note. Mute Condition (`FUN_10010670`: same note, other elements, whole patch, groups) steals. Envelope Mode
  "Analog (Resume)" retriggers the same note's voice in place (`10016820`, mode 0). A note-off of a delayed voice
  silences it (`+0x534 = 2`). Gain = `float(g · 1e-8f)` (one rounding).
- **Mono** (Mono Mode 0x39: 1 lowest, 2 highest, 3 first, 4 last; `FUN_10010200` picks the held note in the key
  range). With Receive CC/EE the patch Mono/Poly (0x39, part+0x948) overrides it (setter case 0x39: Poly → 0,
  Mono → 4 if the element is poly). A mono note plays a stack of Unison Voices + 1 (every type; free unison indices
  from a bit mask). Legato Retrig (0x53) 0 "Start New Voice": all the element's voices are stolen and new ones
  start; else the held voices are retriggered by `FUN_10016820` (1 All Modulation: LFO3 if Each Note + Env1 + amp +
  filter; 2 amp + filter; 3 filter; 4 pitch only; glide per element / patch Glide; the attacks get Leg>Attack /
  Leg>F.Attack +0x870 / +0x874). Note-off back to a held note (velocity = the released voice's): Release Retrig
  (0x54) 0 starts a new voice (`FUN_10021e20` with velocity gain 10000) and steals the old one, else `10016820`.
  Sample voices of a mono element with >= 2 notes held add Leg>Start (+0x86c) to the start point and restart the
  attack with Leg>Attack.
- **Patch LFO1/2:** ticked at the start of a part render call into the array part+0xdd4, one (LFO1, LFO2) pair per
  32 frames (`i < n >> 4`, step 2; the last pair repeated; a call < 16 frames ticks nothing); each voice control
  update reads the next pair (voice+0x2e4, reset at render entry). So the LFOs depend on the host's call sizes
  (16-frame calls tick twice as often as 32-frame ones). Load: the wave setter (0x55 / 0x59, before the phase)
  inits the LFO from the part's stored phase (0 in a new part). The note-on retrigger runs per voice start
  (@10014990). Rate macros: part+0xbc4 bits 0/1 (patch 0x46 "LFO Rate Ctl") add the LFO Rate macro (Hyperknob
  slot 1) to LFO1/2; bit 2 enables it for the element LFO3 (setter case 0x2d, part+0x960).
- **Sleep:** a call with no voices, silent inserts and silent own send FX sets part+0x1140 (unless the arpeggiator
  runs: part+0xc44 is the arp object, +0x186 its running flag); later calls return at once (no LFO ticks, no effect
  calls) until any MIDI event. With the delay/reverb gate looking at the left channel only (FUN_10025480), this cuts
  delay tails: e.g. 039/2 "Soloing" (tap L much longer than R) sleeps 0.13 s after note-off with the right echo at
  -51 dB, and a short note's echo that has not come out yet is never heard. The emulated original does exactly the
  same (combi_oracle, 39/2 and 39/28 with long tails: identical). Port option `setRingOut` (not the original):
  see "Plugin fixes after the user's first REAPER trial" below.
- **Bias:** frame 0 of the part bus is 1e-6 at every call and after an Aux Out clear, +1e-6 before every insert
  (element index >= 1: element 0 is never processed as an insert), +1e-6 on an own send FX bus before its effect.
- **Muted inserts** are not processed but keep their Aux Out routing; a non-effect element with Output Level
  2v² < 0.0001 is muted (+0x41c, setter case 0x60).
- **Hyperknob assignment 13:** knobs 2..6 (0x0f, 0x67, 0x69, 0x6b, 0x6d) call `FUN_10010d20(0x37, 0x38, k, value)`
  with k = 0, 2, 3, 4, 5 (knob 1 has no call): knob k of the first unmuted Rotary Speaker insert, through the
  element setter (its quick-knob routes apply). Other paths to effect knobs: the element quick knobs (done); the
  pitch-bend rotary switch for patch type 3 (`10017880`, MIDI, not ported). The macros (`FUN_1000ca70`) reach no
  effect knob. (83/1's 0.766487 is the stored value.)
- **Drawbar percussion** looks at the other held drawbar voices of the list (`+0x164`), not a global voice list.
- **Compressed samples:** the render loop adds per-frame deltas (`SampleData::delta`); a loop wrap
  (`FUN_1004aad0`) restores the decoder value of seek checkpoint 0 (block +0x10 / +0x14), and a start seek uses
  checkpoint idx. This was the Large Church Organ "error at 3.5 s" (its loop) and the 26/0, 21/1, 32/14 long-note
  errors.
- **Sample Force Mono** (0x14 → +0x858): a stereo sample plays its left channel in a mono render mode.
- **Note Delay "Note Off"** gain: `exp(float((-1 - held) · (float)(1/sr) · 3.45) / t(Fade))` in x87 precision.

- **Shared rand()**: Tape Delay (13), Grain Delay (15) and Vintage Filter (70) draw from the CRT `rand()`, which
  the original shares with the voice engine. `Effect::setRandom` hands them the part's `MsvcRand` (standalone
  fxtest keeps a private one, so fx_compare is unchanged at −240 dB). 83/29, 0/73, 36/66: −138..−147 dB.

**Open:** see "MIDI, transport and arpeggiator" below (this list moved there).

## MIDI, transport and arpeggiator (2026-11-08, second round)
Verified against the part oracle (`part_oracle.py --events / --host`, scratch `ab3.py`): every case below at
-144..-147 dB unless listed under Open.

### PatchPlayer API (core/include/hs2/synth.h)
```cpp
PatchPlayer p(content, sr);
p.load(patch);            // Engine::loadPatch: new part, synced to the last host tempo, patch params, arp, trailer
p.resume();               // plugin resume(): the next block's host tempo counts as a change (sync flags 0x20)
// per host block (events sorted by offset, 0 <= offset < frames; channel nibble ignored):
p.processBlock(outLR, frames, events, numEvents, &transport);   // adds into interleaved stereo
```
- `MidiEvent { int offset; uint8_t data[4]; }` (data[3] = detune in cents for note-ons, 0 from a host).
- `HostTransport { bpm, tempoValid, ppq, ppqValid, barStart, barValid, sigNum, sigDen, sigValid, playing }`, as
  VstTimeInfo. nullptr = no time info (no sync; the song position keeps counting).
- `processBlock` = plugin `FUN_100884c0` (host sync) + `FUN_1007bcf0` for one part: part render calls split at the
  events, song position advanced by n * beats per sample per split, global send FX once per block, master enhancer,
  then the plugin position += frames * beats per sample. Each part render call is `FUN_100191b0`, which the arp
  splits further. The output depends on the host's block sizes exactly as the original's does.
- Also: `midi(status, d1, d2, d3)` (immediate, = FUN_10017880 flag 0), `noteOn/noteOff`, `setParam(elem, idx, v)`
  (FUN_100121a0; elem 0 patch, elem >= 1 element record elem-1), `syncTempo(bpm, bps, bpb, ppq, bar, flags)`
  (FUN_1000ef10 directly), `render(out, n)` = processBlock without events / transport.
- hs2render: `--events "t:cc:n:v,t:pb:v14,t:at:v,t:on:n:v,t:off:n,t:raw:s:d1:d2[:d3]"`,
  `--host "t:bpm:play[:ppq],..."` (start stopped at 120, ppq 0, 4/4; resume() first), `--block N`. Without --host it
  syncs once after loading (120 bpm, flags 0) like `part_oracle --tempo 120`. part_oracle has the same options
  (HostSync mirrors FUN_100884c0; the legacy song position now accumulates like the plugin, which changed the
  cached oracle renders of the arp patches only).

### MIDI handler FUN_10017880 (every case)
- Host note-on with part key x velocity range weight (part+0x97c / +0xa7c, patch 0x31..0x36, FUN_10048b80) 0 is
  dropped (also a 0x90 velocity 0). Then the arp filter `FUN_10046980`; consumed events stop here.
- Note-on: byte 3 -> part+0x120+note (cents; arp notes: byte 3 * 100, not stored), part gain weight
  (velTab * keyTab, 1e8 for arp notes), gain = float(g * float(weight * 1e-8f) * 1e-8f), part+0x112c max velocity,
  part+0x968 = (top held + transpose) * ln2/12 + part+0xbd4 even when the note then plays nothing; patch Mute
  (part+0xba4) stops here. Voice pitch v[0xbe] includes the cents; |cents| > 99 is added again to the glide target
  when the element has Patch Pitch Bend (0x4b -> +0x454). Fingered glide checks the part key table.
- Legato pedal CC68 (part+0xd9c): with >= 2 held notes, a poly element's held voices get FUN_10016820 (element
  Legato Retrig mode, the new note's part+0x120 detune), then the new voice starts as usual.
- Note-off: note bytes 0x80..0xfe = note -byte without a held-list update; sustain (part+0xd98) marks entries -1
  unless the arp is on and not Ctrl Seq; the mono return to a held note is skipped while the arp is on; the release
  tests "Undamped >" against the note byte (-1 for the pedal's release, so it always releases).
- CC value float(v * 0.007874016f + 1e-8f). First the CC map part+0x1a0 (128 shorts, elem << 8 | param), which
  sends the value to the setter instead: defaults (FUN_1000c830) 5 Glide Time (0x94), 7 Level, 9 Mute (!), 10 Pan,
  16-19 / 80-81 Hyperknobs, 65 Glide (0x03), 69 Arp On, 70-79 the macro weights 0x7e..0x87, 91-94 FX sends; patch
  type 1: 20-30 -> element 1 params, 2: 70 -> Piano Character, 3: 12-20 / 31 / 66 / 70-72 -> drawbar element
  params, 4: 77/78 -> element 1 mod row 0. The patch blob's trailer (dword after the records) then overrides it:
  0 = the Hyperknob CCs stored in patch params 0x04, 0x05, 0x68, 0x6a, 0x6c, 0x6e (trunc(v*128.9) = CC + 1; e.g. 0/4
  has CC1 -> Hyperknob 4) plus setter 0x95 = 0, 0x96 = 0, 0x97 = 1, 0x1e = 1; 2 = a stored map + embedded phrases.
- Unmapped CCs: 1 mod wheel part+0xda0 (trigger 1/2; copied to +0xda4 = source 6 per render call; +0xdc8 copy),
  2 / 3 / 4 sources 9 / 10 / 11, 11 expression part+0x950 = v^2 + 1e-8 and every element level +0x5d4 =
  +0x5b8 * that (version++), 64 sustain (voice +0x304 = v for listed voices; up: note-off of note 0xff through the
  handler (and the arp); down with Redamper (patch 0x45 -> +0xbf0): released entries held again, amp coef *=
  exp(-0.03 r)), 66 sostenuto (held -> -1, not with arp on), 67 soft (+0xdbc, voice +0x308 at note-on), 68 legato,
  84 portamento control (every element's last glide = (v-60)*100 cents), 120/123 steal all voices, 121 reset
  controllers (element levels keep the old expression), 126/127 patch Mono/Poly, 0x80/0x81 (arp only) Seq1/Seq2
  +0xdc0/+0xdc4 = (v - 63) * 0.015873 (source 21 is part+0x970 in patch type 2).
- Channel pressure: +0xda8 = v * 0.007874016 + 1e-8, smoothed per render call into +0xdac (source 7):
  += (da8 - dac) * min(n * 10/sr, 1).
- Pitch bend: +0xbcc = ((msb-64)*128 + lsb) / 8192 (source 8); range +0xbb8 up / +0xbbc down (patch 0x49 / 0x4a,
  trunc(v*15.9)): 13 = floor(12x + 0.5), 14 = 0.45x plus a 7-step table (-11.55, -6.72, -3, 0, 4.87, 8.84, 11.55),
  15 = 24, else range * x; +0xbd4 = semis * ln2/12 + fine tune (+0xbc8, patch 0x38); the voice pitch pointer
  v[0x93] is +0xbd4 for Patch Pitch Bend elements, else +0xbc8. Patch type 3 with both ranges 0: bend > 0.25 /
  < -0.25 sets Rotary knob 2 to 1 / 0.
- Patch setter cases ported for runtime changes: 0x00, 0x03, Hyperknobs (quick-knob elements, macro slot via
  FUN_1000ca70, Rotary assignment 13, send FX 3/4 quirk), 0x11..0x21 arp, 0x1b phrase, 0x31..0x39, 0x3a..0x43,
  0x45, 0x47, 0x49/0x4a, 0x70.. (FUN_1000cc40), 0x7e..0x87 weights, 0x94, 0x95, steps. Element records:
  recompute + version++ (voices refresh at render entry); mod-matrix rows set +0x5f0 and the part render end loop
  copies that row into playing voices (FUN_10010c50); a source/mod/dest change sets part+0x978 (Envelope Resume
  threshold +0xb80, mono Start New Voice).
- Macro weights (part+0x8d0, slots 0..9 = 2 p[0x7e+k] - 1, slot 11 = 2 p[0x94] - 1): v' = (1 - w^2) *
  clamp(v + knob) + (w > 0 ? w^2 : 0) (FUN_1001c440), also for the patch LFO rates.
- Part output: the main out goes through `FUN_1000fa30`: a ramp from the last call's gains (part+0xb84/0xb88) by
  (target - last) * 0.001 per frame, snapping within 0.003 (x87 register order of the unrolled loop mirrored), so
  CC7 / CC10 changes glide in; +0xb8c = float(level^2 trim^2 8), L/R = float(((1-p)+x) G) / float((x+p) G).

### Host sync and tempo
- Plugin FUN_100884c0 per block: resume() clears the bpm word; tempo change -> flags 0x20 and bps = bpm / (sr*60);
  play state change +2 stop / +4 start; signature change (num*4/den) +0x48 (+0x50 and position = bar start when
  stopped); while playing a ppq jump > 0.2 adds 8, the position becomes the host ppq and the bar start only moves
  down; +1 when playing. Any flags -> FUN_1007baf0 -> FUN_1000ef10 for every part; the values are kept for parts
  loaded later. Returns the position (+0xb20), which advances by frames * bps after the block.
- FUN_1000ef10: +0xd90/+0xd88 (double), +0x954 = 120/bpm (Tempo to Env Rates: FUN_1004a340 stage times and
  FUN_1001c570 mod-row times scale by (120/bpm - 1) * elem+0x544 + 1), +0x958/+0x95c floats, every insert and own send
  FX's tempo method, the arp sync FUN_10045710, patch LFOs in Tempo sync (rate = bpm * +0xdec; Tempo + Beat phase
  2 fmod((ppq - bar) noteValue + Phase 1.1111, 1)), playing voices' tempo-synced LFO3 rate. The patch load itself
  leaves tempo LFOs at their free rate until the next sync (as the original).
- Effects get the song position (`Effect::setSongPos`, process() args 3/4): Step Flanger, Wah Pedal, TalkBox sync
  (fx_compare still -240 dB; with a song position verified in the part: 3/51, 0/74, 0/39 under --host).
- Note Delay mode 2: delay = trunc(1 / (noteValue(1 - Delay Time) * bps)) (0/24, 0/30, 12/31, 18/10 now exact).

### Arpeggiator / phrase sequencer (`core/src/arp.cpp`, `hs2::Arp`)
- The CHyperArp object (part+0xc44, 0x468 bytes) ported field for field: init FUN_100467b0, setSampleRate
  FUN_10046340, params FUN_100460b0 (patch 0x11..0x21), steps (0xac..), phrases (bank kind 6 'H' from the user's
  Phrases folder, FUN_1001afb0; patch-embedded slots), sync FUN_10045710, MIDI filter FUN_10046980, next event
  FUN_100485d0 / step engine FUN_10046da0. Unit oracle `tools/emu/arp_oracle.py` + `tools/cli/arptest.cpp` +
  `tools/emu/arp_compare.py`: 76 scenarios (all 10 modes, phrases from all 11 banks, transport, random seeds)
  identical events and object state, rand() included.
- Part wiring: load = init, the new part's default params (0x17 0.8, 0x18 0.5, 0x1c 0.5, 0x1d, 0x1e 1, steps 0.5),
  setSampleRate, sync, the patch's in-range values in index order (0x18 = 0.5 for patches < 0xBEC874C), trailer.
  FUN_100191b0 loop: ev zeroed once per call, a next() that pops nothing resends it (quirk), events go to
  FUN_10017880 with the arp flag; the part may sleep only when the arp is not running.
- Result: 0/3, 37/91, 84/78, 83/37 (and 84/1, 84/49, 86/10, 86/33 embedded phrases, 3/64) at -144..-147 dB with all
  note sets, tempo / transport variations, sustain and CC 123.

### Fixed this round (other)
- Zone lookup = content manager vtable +4 `FUN_1001b2a0` (sorted records by keyHi<<24|velHi<<16|keyLo<<8|velLo,
  signed); zone velocity modes 3/7 play only when part+0x970 >= 0.285.
- PCM start index: the PCM branch seeks for any idx != 0 (negative squared, @100164c5); this was the drum
  "high velocity zone" bug (0/8, 1/0, 1/17 now -138..-140).
- Compressed seek / loop wrap load +0x28 and +0x34 for mono too (3/32 note set p: -94 -> -140 dB).
- Live element updates: voices of types that keep a pointer to their element data re-read it after a refresh.

### Open
- Moved to "Piano model, element versions and the rest (2026-11-08, third round)" below.

## Piano model, element versions and the rest (2026-11-08, third round)
Verified against the part oracle (scratch `agent_piano/ab3`, `ab2`, `sweep`): every case below at -145..-147 dB.

### Piano model (patch type 2, bank 082; also any element with a Sample "piano layer")
There is no separate piano engine: the "model" is instrument selection plus a velocity map on ordinary Sample
elements, all in the element setter and the zone lookup.
- **Part word +0x974** (patch setter cases 0x48 / 0x4b / 0x54, @100122f4): `floor(float(Piano Mode * 2.9)) * 6 +
  Piano Reso * 2.9 + (Piano Model > 0.5 ? 3 : 0)` (x87). The loop after it re-sets element param 0x13 (Wave) of
  element records 0..7 only.
- **Piano Mode default:** the patch loader (FUN_10016f70) replaces a stored 0x54 above 0.99 by part+0x14, which
  Engine::loadPatch (FUN_1007bbb0) sets to its third argument (the plugin's memory mode; the oracle passes 0.5 = MID).
  Port: `PatchPlayer::setMemoryMode(v)` (default 0.5), applied at the next load.
- **Layer selection** (element setter case 0x7f, @10023ae6): Sample param 0x11 = layer (+0x854, `trunc(v*15.9)`;
  the word the zone lookup reads as the velocity mode). For layer 1..8 and a wave below 0x80: k = trunc(+0x974),
  mode = k / 6 (0 ECO, 1 MID, 2 XXL), r = k % 6 (Reso 0 off / 1 sustain / 2 always, +3 for Model 2). Instruments of
  bank 009 per mode (ECO / MID / XXL): Model 1 main 8 / 7 / 0, Model 1 resonance 12 / 11 / 2, Model 2 main
  10 / 9 / 1, Model 2 resonance 14 / 13 / 3. Layer 1: M1 main if r < 3; 2: M1 res if r == 1; 3: the stored wave if
  r <= 2; 4: M1 res if r == 2; 5: M2 main if r > 2; 6: M2 res if r == 4; 7: stored wave if r >= 3; 8: M2 res if
  r == 5; otherwise 0x80 (no instrument: the element still counts the note and draws its two rands, then no voice).
  Port: `PatchPlayer::bindSample` (load, element setParam, patch 0x48/0x4b/0x54).
- **Velocity map** of layers 1 / 5 (FUN_1001c0f0, replaces the zone-velocity rand draw): c = clamp(0x100 -
  trunc(part+0x970 * -1024), 0, 0x300) (part+0x970 = Piano Character - 0.375); rows u = c >> 8 (+4 when the
  instrument name has '2' at index 6, i.e. only "Model 2"), interpolated with row u + 1 by c & 0xff; the zone
  velocity is 1 below the first threshold, else the representative velocity of the band (`pianoZoneVel`).
  Layers 3 / 7 play only when part+0x970 >= 0.285 (ported earlier). The "Res" layers come in through their
  sustain-pedal mod rows (source 12 -> Lin Lag -> Peak Hold -> Add -> Amp).
- **Render mode 0x14** (FUN_1004bb10: compressed stereo, filter modes other than 1 / 3) skips the filter and the
  output while gainL + gainR == 0 and both ramps are 0 (only the sample position runs). The resonance layers sit at
  amp 0 until the pedal goes down, so their filter starts from rest there (`Voice::quietSkip_`).
- Result: all 40 bank-082 patches with notes a, d, d + sustain pedal (`sus`), d + `pno` (sustain, soft pedal,
  CC70 Piano Character): -144..-147 dB. Before: 82/0 about 0 dB (wrong
  instruments), `pno` +0.9 dB, `sus` +2.4 dB.

### Element versions (+0x5ec) (`PatchPlayer::verCall`)
- The voice copies its element's version (+0x520) at render entry and re-runs FUN_10049eb0 when it differs. A slot
  that moves to another element of the same type refreshes when their versions differ, which drops the Vel->Glide
  factor of the new note's glide speed (0/91, 0/31: -6 / -3 dB before).
- Versions are per element, not per type: element setter calls of certain cases add 1. Measured in the emulator for
  every type and parameter (all unconditional): common 0x46 0x47 0x48 (FUN_1001c410) 0x4e 0x57 0x62 0x66 0x68 0x6a
  0x6b 0x6e 0x8b 0x8c; Drawbar 0x19..0x21; Wavetable 0x15..0x18, 0x1d; Analog 0x15..0x17, 0x19, 0x1a,
  0x1d..0x21, 0x23, 0x24. Nested calls: case 1 (type change: 0x11..0x30 again), case 2 (Receive CC change: 0x62
  0x6a 0x68 0x7e 0x7f 0x82 0x6e 0x6f 0x70 0x39), case 6 (quick-knob destination 0x06/0x08/0x0a/0x0c: the knob
  before and after, dest = 0x10 + trunc(v*235.9)), cases 7..10 (0x07/0x09/0x0e and 0x0b/0x0d/0x0f: the knob's two
  destinations). A new element: type 0, Receive CC 1, destinations OFF, version 43 after the part's setup.
- At the end of FUN_10016f70 the six Hyperknob setters run again with the elements present: quick-knob elements set
  0x0e / 0x0f, then the knob's macro slot (FUN_1000ca70: 0x62 (0x16 for Sample), 0x57, 0x6a, 0x68, 0x6e, 0x6f,
  0x70 (+0x73), 0x7e, 0x7f, 0x82, 0x43, 0x4e, default 0x62, 0x80, 0x71) re-sets one param of every Receive CC element.
- This reproduces the loaded versions of 99 elements in 20 patches exactly. Runtime calls (setParam, macros, knobs,
  0x39, 0x94, CC11) add the same counts.

### Fixed this round (other)
- **Analog drum kits 3/65, 3/66, 3/67:** the kits route a Hyperknob to their group bus's Output Level through the
  element quick knob (TOM GROUP: knob A = Hyperknob 6 -> 0x8b). An insert's Aux Out / level / pan / sends now come
  from the knob-routed params (`effectRouting`, also on knob changes). -10 dB -> -146.
- **Live type parameters:** FUN_10049eb0 copies the Analog voice words (+0x110 ratio 1, +0x11c osc 3 mix, +0x100
  XOR key, +0x154 osc 1 mix, +0x15c sub, +0x160 noise, +0x178 osc-3 FM, +0x1c8 ratio 2, +0x1d8 osc 2 mix, +0x17c
  drive (0 when the tap is the zero word), +0x240 osc 2 PW base) and the Wavetable osc 2 mix (+0x11c) again; the
  drawbar render (FUN_10051470) calls FUN_1004a030 (weights + fold-back) at its entry, before the note-delay test,
  whenever the versions differ (`Voice::renderEntry`). 83/0, 83/1, 83/4 `org` -0.6 / -3.5 / -12.8 -> -146;
  13/64, 8/64 `drum` -13 / -32 -> -143 / -147.
- **Muted records are built** (the original builds every record; FUN_10021e20 tests +0x41c first), so elements can
  be unmuted at runtime (setParam(elem, 0, 0)); verified with `par` events (82/0 solo, -146).
- **0/65:** a new part's setSampleRate applies the ctor's default patch params, so the patch LFOs start as LFO1
  wave 2 (sine) and LFO2 wave 4 (saw, value 1) with phase 0; a patch wave that differs re-inits from there. 0/65's
  LFO2 Slow Drift walked from 1, not 0. +3 dB -> -146.
- **Sample root following the key range** (zone flag 0x200, @10016586): x87 order with the stored float ln, and the
  Formant +12 case (root 60 - transpose, ln - Keytrack * note pitch). 3/14 Finger Snap -99 -> -146.
- **Macro results in the register:** element setter cases 0x40 (Cutoff), 0x44 (Filter Env amount) and 0x81
  (Formant) use FUN_1001c440's unrounded result. 13/64 (Receive CC Analog) -130 -> -146.
- **Quick-knob routes in the register:** FUN_1001c440 (k < 0) sums v + (2 depth - 1) * knob for route bits 1, 2, 4,
  8 at x87 precision and clamps; the setter's prologue stores the result as float once (`applyElementKnobs`).
  82/15 Gramophone Piano's Crackles level was one ulp off: -134.6 -> -146.5; 31/36, 31/38, 31/50 -106 / -87 / -86
  -> -146. Effect inserts first get their knobs routed with their own knob values, then the routed knobs again
  with the Hyperknob values at the end of the load (the original's order).
- **Setter order (Analog Osc1 Mix sign):** case 0xa9 multiplies Osc1 Mix by the Osc1 Phase sign word as it is when
  0x15 is set, i.e. in the element's load pass with the element's own knob values 0x0e / 0x0f; the loader's final
  Hyperknob calls re-route 0x12 (a knob destination) without setting 0x15 again. The port takes the sign from the
  own-knob routing (`elementOwnKnobParams`). 37/15 -128 -> -146. (General caveat: the port recomputes every derived
  word from the final params; a setter case that reads another word keeps that word's value from its own call.)
- **Microtuning** (FUN_10010870, FUN_10010770; patch 0x4c..0x53): tuning 1 = part+0xbf4 (root +0xbe0), tuning 2 =
  part+0xbf8 (root +0xbec, depth +0xbfc = |v - 0.5|(v - 0.5)16). An entry of the user's Tunings bank (kind 4, read
  at runtime): int32 count + count floats in cents. Counts 0..100 wrap (C remainder of note - root), others clamp.
  Note pitch v[0xbe] = ((int)(cents + (N - 60) * 100) + tuning) * ln2/1200, looked up with the untransposed note;
  the legato retrigger too. Mod source 20 is per voice (voice+0x310 = 0.01 * tuning 2, set at note-on only). A new
  part loads entry 0.0 for both (ctor bank 0 / entry 0x80; the defaults change the entry). No factory patch uses a
  real tuning (entries 0.0, 0.128 or missing banks). Verified with `par` events (9/0: Just Intonation + Piano
  stretch, root and depth changes, source 20 -> pitch): -146 dB.
- **Phrase scenes** (FUN_10016e20, `selectScene`): all trailer slots are kept; patch 0x95 selects slot i; i != 0
  applies its flags (+0x28 bit 0: Hyperknobs from +0x2c; bit 1: arp params 0x11..0x21 except 0x12/0x1a/0x1b from
  +0x5c, steps from +0xac) through the setter, then hands its phrase to the arp only if its event count (+0x1f4) is
  non-zero. No factory patch has more than one slot or a non-zero scene, so only slot 0 is verified.
- **Receive AT / Receive MW** (+0x45c / +0x458, element setter cases 0x68 / 0x6a): written only; no code reads them
  (every other +0x458 / +0x45c access in the DLL is the arp object's own field). Nothing to port.
- Zone velocity "modes 1/5" were the piano layers' velocity map above, not a cycling mode.

### Oracle / tool additions
- `part_oracle.py`: `--events` kind `par:elem:idx:value` (the part setter FUN_100121a0 between render calls; content
  it requests is streamed in at once), and the Tunings banks (kind 4) are opened with the other content banks.
  Existing renders are unchanged: 16 cached renders of all kinds (notes, CCs, sustain, host transport, arp)
  re-rendered bit-identical.
- `hs2render --events` has the same `par` kind (`PatchPlayer::setParam` between blocks; the block is split there).

### Sweep (every factory patch, notes a and d, --tail 0.5)
- Script: scratch `agent_piano/sweep/sweep.py` (oracle renders cached, `--lead 0.1 --tail 0.5`, notes `a` = C4 1.5 s,
  `d` = the dense overlap/chord set). All 2747 parseable factory patches of banks 000..086 (241 entries are empty or
  Syncrosoft-encrypted, not loadable by the port or the oracle, skipped): 5494 renders, 15 silent in both.
- Result with the final build: 5461 renders at -140 dB or better (median -146.3), 14 at -140..-130, 4 above -130:

| Patch | Notes | Error | Suspected cause |
|---|---|---|---|
| 34/68 Compu Pad (= 80/76) | d | -125.9 dB | Analog osc 1 wavetable (wave 6) with Osc1 Mix 0 (mix = 1e-8): voice words identical to the oracle, output differs by single ulps from the first note and the relative error grows in the release; output-stage rounding of the -3 render path |
| 80/76 Compu Pad | d | -125.9 dB | same patch as 34/68 |
| 79/0 GM Drums | a / d | -112.9 / -126.9 dB | only the part's first 32-frame call: the group-bus Shelf / Parametric EQ inserts answer the part's 1e-6 bias differently (absolute 6e-7, exact afterwards; their element params match the oracle); suspected effect state at the first process call |

- The 14 at -140..-130: 1/12, 1/21, 3/1, 3/12, 3/22, 3/26, 3/46, 3/47 (note a), 5/0, 5/7 (a, d), 34/68 and 80/76
  (note a); not investigated.

### Open
- 34/68 = 80/76 (Analog wavetable osc, see the sweep table) and 79/0 (first-call EQ insert response).
- Analog / other setter cases that read a word set by another case keep that word's value from their own call
  (the port recomputes everything from the final params); found and fixed once (Osc1 Mix sign).
- Phrase scenes other than 0 are ported but untested (no factory patch has them).
- MIDI learn (part+0x2a0) and the Model element type: not ported (no factory patch uses Model).

## Plan
1. Formats → Python parsers in `tools/formats/`, each with a JSON dump.
2. Engine RE in Ghidra (project kept in `ghidra/`, not committed).
3. Reference renders from the original plugin, saved in `reference/renders/` (not committed), for A/B tests.
4. JUCE 8 implementation (CMake; VST3/AU/Standalone).

## Plugin UI requirement: accessible to a blind user
The JUCE plugin must be fully usable with a screen reader (JAWS, NVDA or Narrator on Windows, VoiceOver on Mac).
This is a core requirement, not polish; the original's custom-painted GUI is not accessible.
- Use standard JUCE components with JUCE's accessibility support (UIA / NSAccessibility). Any custom-drawn
  control needs an `AccessibilityHandler` with a name, role, value text and keyboard actions.
- Every parameter has a real name and value text with units or enum names (from `docs/param_display.json`), both in
  the editor and as host parameters, so host generic UIs (REAPER's included) are usable too.
- Full keyboard operation and a logical focus order, grouped by section: patch, elements, effects, Hyperknobs,
  global FX.
- Patch and bank browser as lists with type-ahead search; patch changes and errors as readable text.
- **Never `AccessibilityHandler::postAnnouncement`** (the user, 2026-11-08): on Windows JUCE speaks it with its own
  SAPI voice, not through the screen reader. Put information in control names, a Status text box, and move focus to
  Status when an action the user asked for fails.
- Nothing conveyed only by colour or graphics.
- Test with NVDA or JAWS before calling a UI step done.

## JUCE plugin, first version (2026-11-08, `plugin/`)
Details, build steps, keys and design rules: `plugin/README.md`. JUCE 9.0.5 at `C:/programs/JUCE`; configure with
`-DHS2_BUILD_PLUGIN=ON` (MSVC on Windows; the build adds `enable_language(C)` for JUCE).
- One part: one patch on all MIDI channels, VST3 + Standalone (+ AU on Mac, untested). Superseded by the 16-part
  version below ("JUCE plugin, 16 parts").
- Loading on a background thread with a lock-free swap (the audio thread only try-locks); offline renders wait for
  pending loads. MIDI CC0 + program change load patches. Host transport goes to `PatchPlayer::processBlock` per block.
- No `ScopedNoDenormals`: flushing changed the output (~1e-36 differences); the original never flushed.
- Piano memory mode defaults to MID (0.5), the engine's / original's part+0x14 default.
- `Content::sampleBank` is now thread-safe (shared between instances; files read outside the lock).
- Patch directory names start with a one-character marker in many banks (`[` `]` `{` `}` `_` or a space; meaning not
  known, probably browser grouping). `displayName()` strips it; open question for the multitimbral browser.
- **Engine portability:** the MSVC build of `hs2render` matches the oracle exactly as the MinGW build does (−143..−147 dB
  on the 25-test MIDI/host/arp set), so `long double` = 64-bit (MSVC, Apple Silicon) is fine.
- **Tests:** `plugin/tests/compare_with_hs2render.py`: the VST3 in `hs2plughost` (a console host) is bit-identical to
  `hs2render` for program-change loading, state restore (also saved before the host parameters caught up), 48 kHz /
  512-frame blocks, piano and drum-kit patches; a host Hyperknob change takes effect. `plugin/tests/keyboard_walk.ps1`:
  UI Automation check of the standalone app (initial focus in Search, 20 named Tab stops, Enter / arrows /
  type-ahead / Ctrl shortcuts with real scan codes). Not yet tried with NVDA/JAWS or inside REAPER.

## Engine container, combis (2026-11-08, fourth round)
The 16-part instrument: `hs2::Engine` (`core/include/hs2/engine.h`, `core/src/engine.cpp`), combis (`hs2::Combi`,
`core/include/hs2/combi.h`, `tools/formats/combi.py`), the engine oracle `tools/emu/combi_oracle.py`, and
`hs2render` engine mode. Result: **every factory combi renders bit-identical to the emulated original** (100 of 100, two
note / controller / transport sets), and so do patch changes and combi switches while notes sound, mixer parameters,
RPN / NRPN, block sizes 17 / 32 / 512, 44.1 / 48 / 96 kHz, and the setup values. `PatchPlayer` alone is unchanged
(429 / 429 renders bit-identical, batch 2 35 / 35, the plugin test passes).

### The original's structure
- **Synth** (ctor `FUN_1007c690`, 0x370 bytes): +0 content manager, +4 the **global part** (a part that never gets a
  patch; its send FX slots are the four global send FX), +8 the 16 channel parts (the latest loaded per channel).
  Per channel a list: +0x88 pending head, +0x48 pending tail, +0xc8 current node; a node = (part, next); the current
  node's chain holds the parts still sounding after a patch change. +0x108/+0x10c parts to delete (idle `FUN_1007b960`).
  +0x110 sample rate, +0x118 output pairs, +0x11c output channels, +0x124 per-channel "output used" flags
  (content+0x7c), +0x128..+0x148 the stored host sync (bpm, beats per sample, beats per bar, bar, flags & 1 | 0x60;
  the ctor sets 120 bpm / the 44.1 kHz value 0x3f07c6f8c751f177 whatever the rate), +0x14c enhancer mode, +0x150 g,
  +0x154 a, +0x158 the enhancer states (4 floats per output channel), +0x358 overflow logged.
  The ctor's part 1 holds an init "Sawtooth" (one Analog element, part level 0x43 = 0, so silent).
- `FUN_1007ba30` engine setSampleRate (from the plugin's resume): content buses `FUN_1001b040`, every part's
  `FUN_100137f0`. `FUN_1007baf0` host sync: the global part's `FUN_1000ef10`, stores +0x128.., then each channel's
  latest part. `FUN_1007bbb0` Engine::loadPatch(ch, blob, memory): new part (`FUN_10018b80`), setSampleRate, sync to
  the stored values, part+0x14 = memory, `FUN_10016f70`, setter (0, 0x12, 0); the part becomes synth+8[ch] at once and
  the channel's pending tail (played from the next render).
- **Main render `FUN_1007bcf0(lists, outs, n, pos, clear)`**, per channel 0..15: the oldest pending part becomes current
  (the previous current is the first "old" part); old parts render the whole block first (offset 0), the last one is
  removed when it returned silent (or when it is not the only old one); then the current part renders up to each event
  of the channel's list (`FUN_100191b0`), the event's channel nibble is cleared, `FUN_10017880` on the current part and,
  for everything but note-ons, on the old parts (after they rendered: their note-offs act next block); the song
  position advances by frames * synth+0x130 per split. Then the global part's `FUN_1000f130(outs, n, pos, -1)`, the
  overflow guard (|x| >= 100 in the first 4 frames of output channels 1/2: outputs and enhancer states cleared, the
  enhancer skipped), and the **tone enhancer** on every output channel whose used flag is set. Mode 1 (< 64 kHz) and
  mode 2 (two-frame lag) are mirrored with the x87 register / float pattern of their 4-frame unrolled loops
  (1007c254.., 1007c410..); this was needed for bit-exactness.
- **Part buses** (part+0x1134 = the content's bus table): 0 the part's dry bus, 1..4 the global send FX buses (one block
  long, shared by all parts), 5..8 the part's own send FX buses (used within a render call); used flags part+0x1138.
  A slot k's sends (inserts' sends and the remaining bus) go to the own bus 5+k when FX mode (patch 0x3e -> part+0x2b0)
  is on and the own slot has an effect object, else to the global bus 1+k (per slot, not per part). The first writer
  stores, later writers add (`FUN_1000f660`, x87: `float(g * x + dst)` in 53 bits; the port's standalone float add
  differs by an ulp when two parts add into one buffer, so engine mode uses the x87 form).
- Main out `FUN_1000fa30` to output part+0xba8 (patch 0x3d: `min(trunc(v*32.9), outputs)`; 0 = no main output, the ramp
  then stays); insert Aux Out to the part's output, or Output - 2 + Aux Out for Aux Out > 2 (clamped).
- **`FUN_1000f130` (send FX)**, slots FX4..FX1: an unmuted slot with an effect gets +1e-6 at frame 0 and the gated
  process ("silent" = nothing was sent to its bus); when audible, FX2..4 with Send 1 (+0x844 = 4 v^2) > 1e-8 feed
  **content bus 1 = the global FX1 bus** (also for a part's own send FX, at the call's frame offset), and the return goes
  to output +0x83c (element setter case 0x6c: `min(trunc(Aux Out * 32.9), outputs)`; **0 = no return**) with level
  4 * raw 0x8b and linear pan (raw 0x8c). Every bus is cleared after. A part runs it at the end of each render call
  (its own slots, even with FX mode off); the global part once per block.
- **Patch send FX records (`FUN_10016f70`)**: flag 1 = the part's own slot. Flag 2 (a patch with elements): when the
  global part's slot k is empty (raw type >= 0.999) the record goes **into the global part's slot** and the own slot
  is muted / emptied (setter 0, 0x12, 0x13 = 1); when the global slot holds an effect, a record named "Long Reverb" /
  "GM Reverb" (slot 1), "Short Reverb" (slot 2) or "GM Chorus" (slot 3) is skipped, any other goes to the own slot.
  Each record goes through the setter (param 1 forced to 0). So loading Clavinet 9/0 into a fresh plugin makes its
  Long / Short Reverb the global FX: this is the "global FX = combi 0" seen in the REAPER renders (combi 0's global FX
  have Aux Out 0, i.e. no return).
- Patch parameters outside 0..1 are not set (the part keeps its ctor default; the port does this in engine mode); the
  old-patch Gate fixup (0x18 = 0.5 below version 0xBEC874C) is written into the stored value (the port now too).
- New element (`FUN_10025050`): raw param 0 = 1 (muted), 0x13 = 1 (type 128), knobs 0.5 0.5 1 0 1 0 0 0 0.5 1 0 0,
  level / pan 0.5, +0xba4 = 128; setter case 0x6b creates an effect only when the type changes (so a combi's global FX
  of the same type keep their effect object), with tempo, pitch source, setSampleRate and the 12 current knobs.

### The plugin layer (ported in `hs2::Engine`)
Plugin object: vtable 0x103b8f38 (+4 setParameter `FUN_1008a950`, +0x14 process `FUN_1008d400`, +0x44 getChunk
`FUN_10089820`, +0x48 setChunk `FUN_1008c270`, +0x58 resume `FUN_1008af40`, +0xc8 processEvents `FUN_1008d5e0`), ctor
`FUN_1008bad0`, init `FUN_100887f0` (creates the synth), registry defaults `FUN_10088ab0`. Fields: +0xa78 memory mode,
+0xa7c / +0xa80 vel / touch sens, +0xa8c MIDI combi select (0 off, 1 on, 2 chain), +0xae0 / +0xae4 master fine /
semitones, +0xae8 hype (0.33), +0xaec GM, +0xaf0 synth, +0xaf4 parts array, +0xaf8 global part, +0xafc content,
+0xb54 the 0x9e0 VST parameters, +0xb3f4 / +0xb474 velocity / touch tables, +0xb588 / +0xb5c8 per-channel event
lists (8-byte entries: offset, 4 bytes; 99999999 ends a list), +0xb608 + 4k link flag "part k + 1 plays part k's
events", +0xb6cc first block, +0xb8 combi chain[128], +0x32e0 per-channel pending load (bank * 256 + program | 0x10000).
- **VST parameters** (VSTXML): 0 Master Volume; part p, j = 1..0x97 at p * 0x98 + j; j = 0 of parts 2..16 = Link;
  global FX k at 0x980 + 0x18 k. `FUN_1008a950` maps part j: 1 Mute (+ a queued all-sound-off when muting a part whose
  quality param 1 is not 0), 2 Level (0x43), 3 Pan (0x3c), 4 Output (0x3d), 5..8 Sends (0x3f..), 9..14 Hyperknobs,
  15..20 key / vel ranges (0x31..), 21 Transpose (0x37), 22 Fine (0x38), 23 Polyphony (0x3a), 24 FX Mode (0x3e),
  25 Keep (stored only), 26 Level Trim (0x3b), 27 / 28 PB up / down, 29 Mono (0x39), 30 Glide (3), 31 Glide Time
  (0x94), 32..38 microtuning, 39..47 arp 0x11..0x19 (45 = Rate 0x17 shown as v * 1.25 - 0.25, set as v' * 0.8 + 0.2),
  56 + 24 s + j' the part's send FX s (0 mute, 1..16 knobs 0x19.., 17 aux, 18 level, 19 pan, 20..23 sends); global FX
  the same block layout. Master volume: `FUN_10089380`: content+0x12dc = float((8 - g) / 4 * v * v), g = hype * 2.2.
  Unlinking a linked part queues an all-sound-off (CC 120) on it (`FUN_100886a0`; `FUN_1008cc20` / `FUN_1008b110` add
  it to the channel's list at the last event's offset at the next process). No solo parameter exists.
- **Patch loader `FUN_10089ef0(blob, part, restore, bank)`**: `FUN_1007bbb0`; default scene root (patch 0x97 = 1 and
  bank 0..99: 0x97 = 24 / 128, 0x96 = 0); Link re-set; setter (0, 1, memory mode); restore (combis): every part setting
  read back from the patch through setParameter; a normal load keeps the plugin's Mute, Level (unless < 0.01) and
  Output, and with Keep (j 25 > 0.5) also pan, sends, ranges, transpose, fine, polyphony, FX mode; then the Hyperknobs,
  trim, PB, mono, glide, tunings and arp 0x11..0x19 re-set through setParameter (each a setter call: version bumps,
  macro re-application, as the original).
- `FUN_10089c90` clear part (only when the part's patch name is not empty): trim 0, all-sound-off, an empty part
  (`FUN_1007bbb0` with a null blob: the ctor defaults), default part settings unless Keep.
- **processEvents `FUN_1008d5e0`** -> `FUN_100886c0(ch, linked, ofs, status, d1, d2, d3)` (appends, then to the next
  channel while its link flag is set): note-on velocity through the table, velocity 0 = note-off, note-off d2 = 0,
  poly AT (d1 = touch[d2], d2 = note), channel AT through the touch table, pitch bend and other CCs as they are. Kept
  in the plugin: CC0 / CC32 bank select, CC 6 data entry with CC 98..101 (RPN 0 PB range (* 8.2, > 0.8 -> 1), 1 fine,
  2 coarse ((v - 0.5) * 1.78 + 0.5), 3 / 4 tuning; NRPN = element << 8 | parameter of the part's setter, or the mapped
  VST parameters in `FUN_10087fb0`), CC 121 also resets the RPN, CC 126 / 127 -> part Mono (j 29), program change
  (below). Combi chain prev / next CCs (registry CombiNextCC / CombiPrevCC, `FUN_1008d4f0`) not ported.
- **process `FUN_1008d400`**: queued events, host sync `FUN_100884c0`, list terminators, `FUN_1007bcf0`, position +=
  frames * bps.
- **Program changes**: request = (CC0 * 128 + CC32) * 256 + program per channel, queued when it differs from the
  channel's last; GM mode maps to banks 78 / 79 (channel 10: drums unless CC0 = 121); chain mode (+0xa8c = 2): any
  channel, the chain entry, always. The loader thread (`FUN_1008c100` loadPatchFromLibrary) loads a 'P' entry with
  `FUN_10089ef0(blob, ch, 0, bank)`, anything else through setChunk (a combi). Banks 120..127 are listed for MIDI only
  with MIDI combi select on (`FUN_10086830`); the manual's "channel 16 only" rule is not in the code ported here (the
  loader runs in the GUI / idle code). The port reports requests (`Engine::takeProgramRequests`).

### Combi format (`Combi::parse`, `tools/formats/combi.py`)
VST 2 bank file: 'CcnK', 'FBCh', fxID 'StSS', chunk size (big-endian) at 0x9c, chunk at 0xa0. Chunk (setChunk
`FUN_1008c270`, little-endian; 1000..2000000 bytes):
- 0x00 'StSS', 0x04 version (0x990EDF..0xD1CEF00; factory 0x990F46, two 0x990F3E), 0x0c 'B', 1.
- 0x10 u32 offsets[18]: [0..15] part patches (0 = empty part: `FUN_10089c90`), [16] global FX, [17] settings.
- Part k: a patch blob **without its 16-byte header** (setChunk writes 'StSS', the chunk version, 0, 'P', 1, 0, 0 in
  front, in place; a blob with 'S' at +3 is used as is). It runs to the next block and includes its trailer marker.
  Its version is the chunk's (0x990F46 < 0xA87569: every factory combi patch gets the old-patch fixups). The patch
  params carry the part settings. Loaded with `FUN_10089ef0(blob, k, 1, -1)`.
- Global FX: 4 x (char[0x20] name + f32[0x100] element params); each through the global part's setter (0x41 + k,
  idx 0..255, idx 1 forced 0), then `FUN_10086cf0` sets the 24 VST parameters from the element (each a setter call).
- Settings: u32 version v (factory 0x34; two combis 4), f32 master volume (setParameter 0), u8 link[16] at +8
  (setParameter(k * 0x98, link ? 1 : 0) for k = 1..15, and k * 0x98 + 0x19 (Keep) = 0); v > 0x33: char[0x20] name;
  v > 0x3f: vel sens, touch sens, memory mode (floats); v > 0x4f: s32 tune semitones, f32 fine, f32 hype, u32 GM; v >
  0x5f: s32 chain[128] (taken only when the current chain is empty), preload mode; v > 600: next / prev CC.
- Factory combis: `Patches/120 Combis/NNN name.fxb` (100), all settings version <= 0x34 (no setup values stored);
  `121 User Combis` is empty here. Outputs 0.076 (output 2) appear in the surround combis 91..98 (clamped with one
  output). Their active global FX return to an output (Aux Out 1) in 48 combis, have Aux Out 0 (no return) in 48, and
  4 have none.
- **Combi Chain (.h2c)**: text "HS2//COMBICHAIN" + 128 lines of bank * 256 + program (-1 = empty)
  (`parseCombiChain`, `combi.py`, `hs2render --chain`). Factory Chain: combis 120/0..23 and 25.

### Engine API (`core/include/hs2/engine.h`)
```cpp
hs2::Engine e(content, sr, outputs = 1);   // plugin ctor + synth ctor + resume: 16 empty parts, no global FX
e.loadCombi(hs2::Combi::load(path));       // setChunk; or e.loadPatch(part, patch, bank = -1) / e.clearPart(part)
e.processBlock(outLR, frames, events, n, &transport);  // events carry their channel; adds into interleaved stereo
e.processBlockMulti(outs, ...);            // outs[p] = output pair p
e.setParameter(index, v);                  // the VST parameter set (above); setPartParameter(part, j, v)
e.setPartSetter(part, elem, idx, v);       // FUN_100121a0 on a part (part -1 = the global part)
e.setMemoryMode(v); e.setHype(v); e.setVelSens(v); e.setTouchSens(v); e.setMasterTune(semis, fine);
e.setMidiCombiSelect(mode); e.setCombiChain(chain); e.takeProgramRequests();
e.syncTempo(...); e.resume(); e.part(p); e.globalPart();
```
- Parts are `PatchPlayer`s in engine mode (`PartBus`, private hooks; `friend class Engine`): one shared `MsvcRand` for
  all parts and effects (the CRT rand() is global in the original), master gain / master tune from the engine,
  per-slot send routing, outputs, x87 mixing, `sendFxRender` (= `FUN_1000f130`), send FX setter cases
  (`setSendFxParam`), `getParam` (`FUN_1000cea0`), patch setter cases 0x01 (part quality: Receive CC elements re-run
  the ECO/XXL case, others keep theirs), 0x3d, 0x3e. The standalone `PatchPlayer` path is untouched (its global FX stay
  the hard-coded two Hall Reverbs).
- Loads create new parts (as the original); call them between blocks on the audio thread or under the processor's lock
  (they read sample banks through `Content`). The previous part keeps sounding until silent.

### Oracle and tools
- `tools/emu/combi_oracle.py`: original code for the content manager, synth ctor, engine setSampleRate, and on a **fake
  plugin object** (the plugin's vtable and the fields above, set as the plugin ctor / init leave them; the licence gate
  byte 0x103c5538 is set, nothing is decrypted): master / hype `FUN_10089380`, velocity / touch tables, setChunk
  `FUN_1008c270`, the patch loader `FUN_10089ef0`, setParameter `FUN_1008a950`, processEvents `FUN_1008d5e0` (with real
  VstEvents), the queued events (`FUN_1008b110`), and the main render `FUN_1007bcf0`. Mirrored in Python: process
  `FUN_1008d400` (terminators, position) and the host sync `FUN_100884c0` (calling `FUN_1007baf0`). Options: `--combi`,
  `--part ch:bank:patch`, notes with a channel (`t:len:pitch:vel:ch`), events with a channel (`t:cc/3:7:100`), control
  events between blocks (`load:ch:bank:patch`, `vst:index:value`, `combi:index`), `--host`, `--block`, `--sr`,
  `--vel-sens`, `--touch-sens`, `--hype`, `--memory`, `--outputs`. Output pair 1.
- `hs2render` engine mode: the same options (`--combi`, `--part`, `--engine` with `--bank/--patch`, `--outputs`,
  `--memory`, `--vel-sens`, `--touch-sens`, `--hype`, `--chain`); without `--host` one `FUN_1007baf0` sync after
  loading (as the oracle).
- A/B script: scratch `agent_combi/cab.py` (oracle renders cached in `agent_combi/coracle`).

### Results (all bit-identical, -600 dB = zero difference)
- **Sweep, all 100 factory combis**, notes on channels 1..12 (set `s`): 100 / 100. **Second sweep**, notes on channels
  1..4 with CC 7 / 10 / 1 / 64 and pitch bend on three channels, host transport with tempo changes and a jump (`m|cc|h2`):
  100 / 100.
- Patch changes while notes sound (old parts), a combi switch while playing (43 then 17), VST parameters (level, pan,
  mute with the queued all-sound-off, unlink, sends, master volume, global FX aux, transpose, fine), Keep + patch load,
  RPN / NRPN / CC 126 / CC 121, program-change requests, block sizes 17 and 512, 48 kHz, 96 kHz (enhancer mode 2),
  vel sens 0.1 / 0.85, touch sens 0.2 / 0.9, hype 0 / 0.8, memory mode ECO / XXL; single patches in engine parts (9/0,
  31/0, 13/4, 82/0, 3/65, 6/16, 37/11, 35/26). The MSVC build gives the same bits (12 tests).
- Fixed on the way (PatchPlayer, engine mode only unless noted): the stored Gate fixup (both modes; it only changes
  what is read back), out-of-range patch parameters keep the part defaults, per-slot send routing, flag-2 records to the
  global part, x87 output mixing.

### Open
- The ctor's part 1 "Sawtooth" init element is not modelled (the port's fresh parts are empty): its silent voices draw
  from the shared rand, so a test that plays channel 1 before loading part 1 differs at -163 dB.
- getChunk (`FUN_10089820`, saving a combi / the plugin state) not ported; combi chain prev / next CCs, the GUI loader's
  rules for program-change requests (only reported), GM reset `FUN_1008c820`, SysEx `FUN_1008cc90`, the GM bank-79
  "missing program" fallback, meters (`FUN_1000ffe0`), the overflow guard (mirrored, not triggered in a test).
- Realtime: processBlock allocates (event lists, buffers on first use) and loads allocate parts.
- Multiple outputs: `processBlockMulti` exists; tested with one output only (part / FX outputs clamp to it).

## JUCE plugin, 16 parts (2026-11-08, second version)
The plugin now runs one `hs2::Engine`: part N plays MIDI channel N, patches load into single parts, combis load all
parts and the global send FX. Details for users: `plugin/README.md`.
- **Loading.** The loader thread reads and parses the patch / combi and loads it once into a throwaway `PatchPlayer`
  (fills the shared sample-bank / wavetable / drawbar caches, collects the unsupported-feature warnings), then queues
  a command; the audio thread applies it at the next block start (`Engine::loadPatch` / `loadCombi` / `clearPart` /
  `setMemoryMode`), so the other parts keep playing. Measured after warm-up (MSVC, 44.1 kHz): `loadPatch` 0.1..1.0 ms
  (drum kit 3/65 the slowest), `loadCombi` 0.4..1.9 ms; the warm-ups (file reads) take up to 0.9 s and stay on the
  loader thread. A whole new engine is built (loader thread, swapped under the try-locked SpinLock) only for a state
  restore, a new content folder or a new sample rate.
- **Thread safety for that:** the wavetable caches (`analog.cpp`, `wavetable.cpp`) now lock like `Content::sampleBank`
  (lookup under a mutex, file read outside it, first entry kept); the drawbar cache already locked.
- **MIDI program changes** are the engine's own (`FUN_1008d5e0`): bank = CC0 * 128 + CC32 (so CC32 = bank number with
  CC0 = 0), a request only when it differs from the channel's last one; the plugin loads banks 120.. as combis
  (`combiPath`), others into the channel's part. Settings: "Load patches and combis from MIDI program changes" (per
  instance, saved). The first version's own CC0 = bank rule is gone.
- **Host parameters** (mirroring engine VST parameters, raw 0..1 values): Master Volume (0), and per part Mute (j 1),
  Level (2), Pan (3), Effect Send 1-4 (5-8), Hyperknob 1-6 (9-14): 209. Both ways: host changes are applied by the audio
  thread when the host value moved; engine changes (loads, combis, MIDI) are found by comparing the engine's values
  every block and pushed to the host on the message thread (20 Hz timer), with an echo check so the push is not
  applied back. The value text: Master `20 log10((8 - 2.2 * 0.33) / 4 * v^2)` dB; Level and sends `40 log10(v)` dB
  (gain v^2; Level also times the patch trim); pan left / centre / right; mute muted / playing.
- **State (version 2):** the combi (bank, index, name) if one was loaded, then per part what was loaded after it (a
  patch: bank, index, name; or cleared), the 209 values as the engine holds them, the selected part, the MIDI setting.
  Restore = new engine, combi, part loads in part order, then the values that differ from what the loads set. Version 1
  states load as part 1. Names find a patch again if its index moved.
- **New instance:** Analogy (31/0) in part 1, other parts empty.
- `hs2render` additions for the plugin test: `--load-bank` (loads pass the real bank to `Engine::loadPatch`, as the
  plugin does: the default scene root rule), `--skip N` (the test starts N frames later and those frames are left out),
  and event times `@F` (absolute frames).
- **Tests:** `plugin/tests/compare_with_hs2render.py` (38 checks, all bit-identical / OK): 11 patches into part 1 by
  program change (reference: Analogy in part 1, then `load` at frame 32) and by state restore; a patch into part 2 on
  channel 2 next to part 1; combis 120/3, 22, 44 by program change and state restore; 48 kHz / 512 blocks; a state
  saved without a message loop; a Hyperknob and a part Level set by the host, also after a state round trip.
  `plugin/tests/keyboard_walk.ps1`: 29 named Tab stops in order, the part selector (Ctrl+P, arrows,
  Ctrl+Shift+Page Down), a patch loaded into part 3, a combi loaded from the list.
- **Layout bug found by the UIA walk:** with all sections in one column the window was taller than the screen and the
  patch list got zero height, which removed it from the Tab order and from UI Automation. Now two columns.
- **Open:** combi saving and Combi Chain files in the plugin; multiple outputs; the rest of the part settings (Keep,
  ranges, transpose, polyphony, arp...) and the global FX as host parameters; a part list with all mixer values at a
  glance; NVDA / JAWS / VoiceOver and REAPER trials by the user; a combi load at very small buffers (64 frames =
  1.45 ms) can take longer than one block.

## Plugin fixes after the user's first REAPER trial (2026-11-08)

- **Delay tails cut off (user report, Soft Leads).** Not a porting error: the original does the same (see Sleep
  above). New `PatchPlayer::setRingOut` / `Engine::setRingOut` (default off = the original, bit-exact): after each
  insert / send effect, the effect counts as audible for 4.5 s after its input was used or its output was audible on
  either channel (2|x| > 1e-5), so the part stays awake and the effect output keeps reaching the outputs. 39/2 and
  39/28 now decay smoothly to about -100 dB. `hs2render --ring-out`. The plugin turns it on by default (Settings,
  "Let effect tails ring out", saved in the state as `ringOut`).
- **Program changes in the VST3.** VST3 has no MIDI program change event: a host maps it to the plugin's program
  list parameter (kIsProgramChange), and JUCE calls `setCurrentProgram` only from the edit controller's side
  (the processor-side parameter change is stored but not passed on), with no MIDI channel. The plugin now has 129
  programs: 0..127 = program numbers, 128 = "None", which `getCurrentProgram` always reports so a repeated number
  still arrives (JUCE skips a value equal to the current program). `setCurrentProgram(n)` queues a MIDI program
  change n that the audio thread adds at the end of the next block, on the channel of the latest bank select (CC0 /
  CC32 do arrive as MIDI through JUCE's MIDI CC parameters), and the engine handles it as any program change.
  `hs2plughost --pc-param` tests this path the way a host drives it (JUCE's VST3 host: CCs through IMidiMapping, the
  program through the program parameter). Whether REAPER sends MIDI program changes this way is NOT verified yet.
- **REAPER render copy not isolated.** A run of `C:\programs\reaper-render` used the user's `%APPDATA%\REAPER`
  configuration (plugin list, REAPER.ini writes), so its result came from the user's older `C:\vst` copy. Do not
  launch it again until that is solved (see the memory note on REAPER).
- **Tests:** `plugin/tests/compare_with_hs2render.py` now 45 checks, all bit-identical / OK: the hs2render references
  use `--ring-out` (the plugin default); new: VST3 program parameter (15/23 and 39/2 into part 1, 9/0 into part 2,
  combi 120/22), and ring-out off / on through the state (`hs2plughost --state-attr ringOut=0`) on 39/2.
- **Patch change stops the old sound (user, second trial).** The user remembers the original stopping what was playing
  when a program change switched the patch. The original's program change path (idle function 1008d2b0 ->
  1008c100: the bank lookup, 10089ef0, then 1000c710 = copy the patch name into the part) has no explicit sound-off;
  in the engine the old part sounds on (Engine::render's old list), as ported. The audible stop in the real plugin may
  come from the load running in the idle/UI thread or from its sample streaming; not verified. New
  `Engine::setCutOnLoad` (default off = as ported): a load marks the slot, and once the new part plays the old parts
  fade out over 10 ms (rendered into scratch buses, faded into the outputs and global send buses) and get no more MIDI.
  `hs2render --cut-on-load`. Plugin: on by default, Settings "Stop the old sound when a part's patch changes", state
  attribute `cutOnLoad`.
- **Program change lag (user: "sometimes laggy").** Load times for bank 039: under 1 ms for most patches; the first
  load of a sample-heavy patch reads its samples from disk (39/14 Capsular Lead 664 ms, 39/18 157 ms; again: < 1 ms).
  The VST3 path also waits for the host to hand the program change to the edit controller (REAPER's timing unknown).
- **Tests:** 46 checks (the references also use `--cut-on-load`; new: a load while a note sounds, with and without).
- **Default patch (user: "the default patch is wrong").** The original's plugin ctor (1008bad0) sets +0xb6d0 =
  0x15400 = bank 0x54 (84, HS2 Preview Patches), program 0 ("Welcome ToThe Matrix"); the idle function 1008d2b0
  ("Loading default patch:") moves it into channel 1's program request, loaded through 1008c100 -> 10089ef0 (not a
  restore). setChunk clears it (1008c2c6), so a restored project never loads it. The plugin's new instance now loads
  084/0 into part 1 (was 031/0 Analogy); the plugin test references start from 84/0.
- **Loading speed.** The slow first loads (39/14 Capsular Lead 623 ms even with the OS file cache warm) were
  `MilanBank::open` reading the whole bank file through `istreambuf_iterator` (sample banks up to 179 MB). Bank files
  are now memory-mapped (read-only; Windows MapViewOfFile, POSIX mmap): bank 039 first loads mean 0.5 ms, worst 2.5 ms.
  Output unchanged (39/28 still identical to the original; plugin test 46/46).
- **Sample decoding** still happens at a note-on that first needs a sample (`SampleBank::sample`, on the audio
  thread), now thread-safe (mutex, decode outside the lock). `PatchPlayer::preloadSamples()` decodes everything a
  patch's sample elements can play, but the decoded form is large (data + delta floats: 8 bytes per compressed byte):
  82/0 Natural Grand Piano 889 MB in 525 ms, 21/79 583 MB; so the plugin does not call it yet. Open: a smaller decoded
  form (e.g. deltas only, or int16) and/or background preloading with a memory budget.
- **Program changes on several channels at once (user: a MIDI file in REAPER, 13 tracks to one HS2 Port; not all
  parts loaded, wrong program in part 1).** The file sends CC32, CC0, program change on every channel at tick 0. With
  JUCE's single root program list the host maps every channel to one parameter and the plugin kept one pending
  program, so all but one were lost. Fix: `plugin/juce_patch/apply.cmake` patches the build's JUCE 9.0.5 (FetchContent
  clone, local checkout or GitHub; never the checkout itself): `VST3ClientExtensions` gets
  `getPluginHasProgramChangePerMidiChannel`, `setProgramForMidiChannel`, `getProgramNameForMidiChannel`; the VST3
  wrapper then has 16 more units (ids 'hs2'+1..16) with a program list each ('hsl'+n, 128 programs), a program change
  parameter per unit ('hsp'+n, kIsProgramChange | kIsList, set on the controller -> setProgramForMidiChannel), and
  `getUnitByBus(kEvent, kInput, 0, ch)` returns channel ch's unit. The plugin queues one program per channel and adds
  them at the end of the next block. Test: `hs2plughost --pc-channels` (5 channels at once, bit-identical). Verified
  that a fresh GitHub clone of JUCE 9.0.5 patches and configures (the Mac path). Still to check in REAPER with the
  user's file: whether REAPER uses getUnitByBus (VST3's documented way; Kontakt relies on it).
- **Controllers sent with a program change were lost (user: the same MIDI file, "the controllers might be the
  issue").** The engine handles CC7 / CC10 / CC91 exactly as the original (combi_oracle vs hs2render, identical): the
  part's CC map (FUN_1000c830) sends CC7 -> patch 0x43 Level, CC10 -> 0x3c Pan, CC91..94 -> 0x3f..0x42 sends, all in
  the part. A program change replaces the part a block or more later (the original's loader is even later: the
  request is taken by the plugin's idle function 1008d290, a vtable entry, also the content streaming driver), so the
  volume / pan / sends a MIDI file sends with its program changes went to the old part. The plugin now holds the
  controllers a channel receives from the block of its program change request until the new patch is in place (at
  most 2 s; not CC 0/32, 6/38, 96..101, 120..127) and sends them again to the new patch (a combi: all channels).
  Test 7c (CC7/10/91 with the program change, identical to hs2render applying them after the load).
  Second version (user: volume still not set, and the delay the file switches off with CC93 = 0 still there): in
  REAPER the program change comes through the VST3 program list, after the block with the controllers, so the hold
  window opened too late. Now every new patch put in place gets its channel's controllers of the last 3 s again
  (ccLast_ / ccTime_, independent of when the program change arrived). The file sends PC, CC7, CC91 (and CC93 = 0 on
  channels 5 and 9: send 3 off) at tick 0 on every channel. Test: --pc-param with --init-cc 7=40,10=20,93=0.
- **Portamento Time (CC 5) had no effect (user: a lead's portamento time wrong in the MIDI file).** CC 5 -> patch
  0x94 (CC map); the patch setter case 0x94 (@10012ea5) stores the slot 11 (Glide Time) macro weight part+0x8fc =
  2v - 1 (v*0.5 - 1 with part+0x114c) and re-sets the Receive CC elements' Glide Time. The port re-set the glide but
  `computeMacroWeights` only had slots 0..9 (0x7e..0x87), so slot 11 stayed 0. Fixed (element.cpp); no factory patch
  stores 0x94 != 0.5, so only CC 5 changes anything. combi_oracle vs hs2render on 35/10 with CC5 alone, CC65 + CC5,
  CC126 + CC65 + CC5 and the file's full lead set-up (CC65, CC126, CC5 = 86 / 0, RPN 0 = 2, CC84): all identical
  (were 2.5..3 dB apart). Slots 10 and 12..15 are only ever 0 in the original (reset writes only).
- **NVDA and the patch list; ASIO and accessible audio settings (2026-11-08, after looking at OnjLouis/
  sound-canvas-bridge, MIT).** Patch list: JUCE focuses the ListBox container, so NVDA sometimes reads only "list";
  `PatchListBox` now hands the accessibility focus to the selected row's handler (table interface getRowHandler ->
  grabFocus) after focusGained (callAsync) and on every selection change, Windows only. ASIO: `HS2_ASIO_SDK_DIR`
  (default C:/programs/asiosdk/ASIOSDK, ASIO SDK 2.3.4 downloaded from steinberg.net, not in git) turns on
  JUCE_ASIO=1 for the plugin's shared code. `AudioSettings.cpp`: "Audio and MIDI settings" window for the standalone
  app (Settings button, shown when `StandalonePluginHolder::getInstance()` exists): driver, output device, sample rate,
  buffer, refresh, driver control panel, MIDI input check boxes, status text; saves through
  `StandalonePluginHolder::saveAudioDeviceState`. Built; the window and the list change still need a screen-reader
  check by the user (the UIA test cannot take the foreground while the user works).
- **"not a StSS patch" (user report: 017 "Pick Bass New", 025 "Miami Brass").** 63 of the 2810 factory patches are
  stored Syncrosoft copy-protected (slot 1 starts with "SyncrosoftContent"): the "New" HS-2 additions after the GM
  patches in banks 008, 015, 017, 018, 024, 025, 031, 033, 034, 035. The original unlocks them with its licence. The
  port does not decrypt them (copy protection, like the plugin state chunk). The plugin now marks them in the patch
  list ("(copy-protected, cannot load)") and a load says why; the part keeps its patch. No factory combi is protected.

- **Presets for DAWs and Komplete Kontrol (2026-11-08).** `plugin/Source/PresetExport.cpp`, the Settings button
  "Install presets..." and the tool `hs2presets`. Formats: .vstpreset (VST3 SDK preset file: "VST3", version 1, the
  class ID ABCDEF019182FAEB4873327048733250, list offset, a "Comp" chunk = the plugin state, chunk list), .aupreset
  (plist with type/subtype/manufacturer/version/name and the state under "jucePluginState", JUCE's AU key) and .nksf.
  NKS layout, learned from a VST3 NKS preset of OnjLouis/sound-canvas-bridge on this machine (decoded only): RIFF form
  "NIKS"; chunks NISI, NICA, PLID, PCHK, each starting with the 32-bit LE version 1; NISI/NICA/PLID are MessagePack
  maps; NISI {author, bankchain [product, bank, ""], characters, comment, deviceType "INST", name, types [[type, sub]],
  uuid, vendor}; NICA {"ni8": pages of 8 {autoname, id, name, vflag[, section]}}; PLID {"VST.magic": 0, "VST3.uid":
  the class ID as four 32-bit numbers, pluginName, pluginVendor}; PCHK = version + a complete .vstpreset. Knob ids are
  host parameter ids; the plugin now uses JUCE legacy parameter ids (id = index) so that both readings agree.
  The preset state carries a "preset" attribute: setStateInformation then keeps the settings, the selected part and
  the master volume. Re-runs remove only the files of the previous run's receipt. Test 10: two patches by .vstpreset,
  two by .nksf and a combi by .nksf, loaded through JUCE's VST3 host (Steinberg's PresetFile::loadPreset):
  identical to hs2render. 2747 patches + 100 combis in about 20 s; 63 copy-protected patches skipped. Not yet tried
  in a DAW's preset browser, in Komplete Kontrol or with AU (Mac).
- **Preset switching lag and Komplete Kontrol detection (user, 0.3.0 trial).** Measured with `hs2plughost
  --switch-preset`: setPreset returns in 0.6 ms, the rebuild takes about 4 ms, the parameter push 1.4 ms. The lag was
  the host: each patch change renamed the Hyperknob parameters and called updateHostDisplay(parameterInfoChanged), so
  REAPER re-read about 2300 parameters (209 + JUCE's 16 x 130 MIDI CC parameters). Hyperknob parameter names are now
  fixed ("Part 1 Hyperknob 4"); the patch's knob name leads the value text ("Thump, 50 %"); no parameter info update
  on patch changes (user's choice). NKS presets are written only when Komplete Kontrol is installed
  (`kompleteKontrolInstalled`: Windows HKLM\SOFTWARE\Native Instruments\Komplete Kontrol InstallDir with
  Komplete Kontrol.exe, or Program Files\Native Instruments\Komplete Kontrol; Mac the app in /Applications).
- **The original's other parameters (user: "expose more parameters the original had"; 2026-11-09).** Host parameters
  832 = the 209 before (unchanged numbers) + per part 32 settings (ParamDefs.h kExtDefs: VST j 4, 15..24, 27..47: the
  part settings, microtuning, arpeggiator; Keep (25) and Level Trim (26) are not exposed, the original's VSTXML does
  not name them either) + Link for parts 2..16 + 4 global effects x 24. Engine indices from the original's layout
  (Engine::setParameter); defaults from `Engine::defaultParameters()` (the plugin ctor's). Value texts: the original's
  display code via param_display.json (`tools/emu/gen_param_text.py` -> `plugin/Source/ParamText.h`); "<C>" (pan
  centre) reads "centre"; microtuning bank / scale names from Tunings/ (PatchLibrary::tunings). Global effect knob
  names are still "Knob N" (each effect's paramName / paramDisplay in the DLL is not captured yet). Editor: a Show box
  in the Part section and Ctrl+Tab / Ctrl+Shift+Tab switch the right side between Patch and mixer, Part settings,
  Arpeggiator, Microtuning and Global effects (ParamRow: a choice box for fixed lists, else a slider). UIA walk: the
  page controls read with names and values ("Output = 1", "Arp Mode = Up", "Tuning 1 Scale" by name).
- **Global effect knob names and texts (2026-11-09).** `tools/emu/sweep_fx_display.py` runs each of the 52 effects'
  own paramName / paramDisplay (docs/engine/effects_table.json) in the emulator: per knob a fresh effect object (other
  knobs at their constructor defaults), setParam(knob, k/1024) and paramDisplay for k = 0..1024, stored as the exact
  texts run-length coded (docs/effect_display.json; no unit conversion, so "2 ms" stays "2 ms"). Knob numbers can skip
  (most effects leave knob 2 unused unless it is Sync). A switch knob (2..8 texts) is tried at each text against every
  other knob: 11 knobs follow Sync (the delays' Delay, Reverse Reverb Time, Step Flanger Step Rate, Wah / TalkBox Rate:
  seconds or Hz when off, note values when on), recorded as cases; nothing follows a Mode switch.
  `tools/emu/gen_fx_text.py` writes plugin/Source/FxText.h (UTF-8: the DLL's texts are Windows-1252, degree and cent
  signs; "Cho<>Rev" is written "Cho/Rev" so a screen reader doesn't say "less than greater than"). Global effect VST
  knob j (1..16) is the effect's knob j - 1 (sendFxParamIndex: element 0x18 + j, which the element passes to the
  effect as knob j - 1); no effect has more than 12. The host parameter names stay "Global Effect k Knob n" (as the
  Hyperknobs: names fixed so hosts don't re-read the list); the value text leads with the knob's name. All 100 factory
  combis use only Long Reverb / Short Reverb (both Hall Reverb, type 1) as global effects, so the Sync cases can't be
  shown through a factory combi.
- **Choosing a global effect (2026-11-09).** The original's FUN_10085160(part, element, path): reads
  `<content>/Effects/<path>` (accepted only at 0x4e4 = 1252 bytes), names the element after the file (without
  ".fxp", 31 characters), then setter 0x91 = 0, 0x92 = 0 (Hyperknob routes off), 0x13 = file float +0x130 (the type,
  e.g. Mono Delay 0.0781 -> 10 Delay), 0x19..0x24 = file floats +0x148.. (the 12 knobs), 0 = 0 (unmuted); a file it
  can't read empties the slot (0x13 = 1.0). Port: `Engine::loadGlobalFx` (then the slot's VST parameters re-read as
  after a combi), `EffectPreset` / `PatchLibrary::effects` (all 108 factory presets, 14 folders), the processor's
  `Setup::gfx` (saved as SETUP/GFX slot / effect; restored after the parts, before the saved parameter values) and
  the editor's Effect box. hs2plughost `--state-gfx "1=Delay/Mono Delay.fxp"`: Mono Delay's knobs read as the file
  (Delay 0.506 s, Feedback 50 %), and with a part sending to effect 1 the return starts 0.5 s after the dry sound,
  repeating at half level, as the original's own Delay does on an impulse (fx_oracle: echoes at 0.505 s, x 0.4975).
  Sync on reads "Delay, 2/4". A delay at Mix 0 % passes its input through (the original's too), so a send effect
  with Mix 0 adds the dry sound.
