# hs2capture: snapshots, recordings and patch captures from the original Hypersonic 2

`hs2capture.exe` hosts the **original, licensed** Hypersonic 2 VST 2 plugin (not the port) on Windows. For each
patch in a list it loads the patch on one part with MIDI bank select and a program change, then

- **patch**: saves the patch itself as a plain `StSS` `.stss` file - the way the port reads every other patch. For the
  63 copy-protected factory patches (the "New" HS-2 additions, `protected_patches.tsv`) this is the point of the tool:
  they are stored encrypted, the plugin unprotects each one as it loads it, and the capture is that plain patch, read
  from the plugin's memory as the plugin frees its decrypted buffer (one pointer in its import table is redirected; no
  code is modified, and the tool decrypts nothing itself). The port reads the files from `<content>\Captures\patches`
  automatically; there is no setting. Captures are personal use under your licence, like the rest of your content: keep
  them on your machine, do not distribute them.
- **snapshot**: saves the plugin's whole state as `.fxb` (the bank chunk, what a DAW saves in a project) and the
  program chunk as `.fxp`, both named after the patch;
- **record**: plays notes over the keyboard at a few velocities and writes 32-bit float WAV files (with the root note
  in a `smpl` chunk) and an `.sfz` file that maps them.

Snapshots and recordings are what the plugin hands out, as to any VST host. The original's state is always re-encrypted
on the way out (`docs/NOTES.md`), so the snapshots load back into a licensed Hypersonic 2 only; the port cannot use
them. The recordings are ordinary audio, for your own use under your Hypersonic 2 licence.

## What you need

- Windows 10 or 11 (64-bit is fine: `hs2capture.exe` is a 32-bit program, like the plugin).
- Hypersonic 2 installed and **activated** (eLicenser), working in a host such as REAPER.
- `hs2capture.exe` and a patch list:
  `protected_patches.tsv` (the 63 copy-protected patches) or any list made by
  `tools/formats/list_protected.py "<Hypersonic 2 Content>" --tsv list.tsv [--all]`.
  A list is `bank<TAB>index<TAB>name` per line (index counts from 0 within the bank); write your own to pick patches.
- REAPER is not needed while it runs. Close it first, so that only one Hypersonic 2 uses the licence and the content.

`hs2capture.exe` is not in the repository (built binaries are git-ignored): copy a built one over, or build it on
Windows with the MinGW 32-bit compiler (the 2026 MSVC CRT is not
compatible with hosting this 2005 plugin: builds hang or corrupt the heap):

```
set PATH=%USERPROFILE%\scoop\apps\msys2\current\mingw32\bin;%PATH%
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -o hs2capture.exe hs2capture.cpp -lole32 -luser32
```

## Steps (for a person, or a Claude on that computer)

1. **Find the plugin.** `hs2capture` looks in the usual folders (`C:\Program Files (x86)\Steinberg\VstPlugins`,
   `C:\Program Files (x86)\VstPlugins`, ...). If it says it cannot find it, look up the folder in REAPER (Options >
   Preferences > Plug-ins > VST: the paths; REAPER x64 runs it through its 32-bit bridge) or search the disk for
   `Hypersonic*.dll`, and pass `--plugin "C:\...\Hypersonic2.dll"`.
2. **General MIDI mode off.** If Hypersonic 2's GM mode is on, program changes pick GM patches instead of the
   listed banks. Check its settings in REAPER first.
3. **One-patch trial, snapshot only:**
   `hs2capture.exe --list protected_patches.tsv --out C:\hs2capture --limit 1 --mode snapshot`
   A window with Hypersonic 2's editor opens: leave it open (the original loads patches from its editor / idle code)
   and do not use it. Expect `snapshot: N bytes` and `ok` in `C:\hs2capture\results.tsv`. In the editor, part 1
   should show the patch (bank 8, "Electric Piano 2").
4. **One-patch trial, patch capture:**
   `hs2capture.exe --list protected_patches.tsv --out C:\hs2capture-test --limit 1 --mode patch`
   Expect `patch: N bytes; round-trip ok` (the plugin reloaded the captured patch through its setChunk) and
   `patches\008-030 Electric Piano 2.stss` in the output folder.
5. **All 63 patches:** `.\capture_all.ps1 -List protected_patches.tsv -Out "<content>\Captures"`
   The original's loader stops loading copy-protected patches after a while (a handful per process), so the
   driver runs hs2capture again and again - it skips patches already captured, stops after two failed loads
   (`--max-fails`), a watchdog exits a hung process, and a fresh pass continues - until all files are there.
   Point `--out` at `<content>\Captures` and the port finds them automatically (also without `patches\` in
   between); no settings anywhere.
6. **One-patch trial, recording a few notes:**
   `hs2capture.exe --list protected_patches.tsv --out C:\hs2capture-test --limit 1 --notes 48-72/12 --overwrite`
   Listen to the WAVs in `C:\hs2capture-test\samples\...`: the patch, at the right pitch, no clicks or dropouts.
7. **A full run with recordings:** `hs2capture.exe --list protected_patches.tsv --out C:\hs2capture` (the
   default mode is snapshot + record). Patches already captured are skipped, so it can be stopped (Ctrl+C) and
   started again. Failed patches are retried.
8. **Check** `results.tsv`: `ok`, or `failed` with the reason; and "N silent notes skipped" for notes that made no
   sound (for example notes outside a patch's key range).

## Time

Recording runs in real time by default (`--speed 1`), as the plugin streams samples while it plays. Defaults: 25 notes
(`--notes 24-96/3`, C1 to C7 every 3 semitones) x 2 velocities (`--vels 127,64`), each held 2 s with up to 6 s of
release: about 4 to 8 minutes a patch, **5 to 8 hours for the 63**. Fewer notes or velocities, a shorter `--hold`, or
`--speed 2` (if a trial at that speed has no dropouts) shorten it. `--mode snapshot` alone takes seconds a patch.

## Output

```
C:\hs2capture\
  results.tsv                         bank, index, name, ok / failed, details
  patches\008-030 Electric Piano 2.stss   the patch itself, plain (the port reads these; capture_all.ps1 points
                                         --out at "<content>\Captures", which the port scans automatically)
  snapshots\008-030 Electric Piano 2.fxb   whole plugin state (load: the host's "import FXB" in its FX window)
  snapshots\008-030 Electric Piano 2.fxp   program chunk
  samples\008-030 Electric Piano 2\
    008-030 Electric Piano 2 n060 C4 v127.wav   ...one per note and velocity
    008-030 Electric Piano 2.sfz              key ranges halfway between notes, velocity layers
```

Notes are named with C4 = MIDI note 60. The SFZ has no loops: each sample holds the note for `--hold` seconds and then
its release, so held notes in a sampler last that long.

## Options

| Option | Default | |
|---|---|---|
| `--plugin PATH` | searched | the Hypersonic 2 DLL |
| `--list FILE` | (needed) | the patch list |
| `--out DIR` | `hs2capture-out` | output folder |
| `--mode` | `both` | `patch`, `snapshot`, `record`, `both` (snapshot+record) or `all` |
| `--part N` | 1 | part / MIDI channel the patches load on |
| `--notes` | `24-96/3` | from-to/step, from-to, or a list `36,48,60` |
| `--vels` | `127,64` | velocities |
| `--hold` / `--tail` | 2 / 6 s | note length / longest release recorded |
| `--quiet-db` | -90 | level counted as silence (ends a release, trims the file) |
| `--settle` | 20 s | longest wait for a patch to load |
| `--speed` | 1 | 1 = real time, 0 = as fast as possible |
| `--rate` / `--block` | 44100 / 512 | sample rate / block size |
| `--limit N` | | only the first N patches |
| `--max-fails N` | 3 | stop after N consecutive failed loads (the loader wedge; run again to continue) |
| `--no-gui` | | do not open the editor (patches may then not load) |
| `--overwrite` | | redo patches already captured |

## If something goes wrong

- **"the plugin did not start" / licence messages**: the eLicenser must be reachable (USB-eLicenser plugged in, or the
  Soft-eLicenser on this computer) and Hypersonic 2 must work in REAPER on the same computer.
- **"the plugin's state did not change"**: the patch did not load. Check GM mode (step 2); try `--settle 60`; try
  another part (`--part 16`: the manual mentions channel 16 for some program changes); make sure the editor window
  stayed open.
- **Windows SmartScreen** may stop an unsigned program: "More info", "Run anyway", or build it yourself (above).
- **Dropouts or clicks** in the WAVs: keep `--speed 1`, close other programs, or raise `--block` to 1024.

## How it works

`hs2capture.cpp` (one file, with `vst2abi.h`: the VST 2 binary interface written from its public description, no SDK
code). It loads the DLL, opens the plugin at the sample rate and block size, opens its editor in a window, and plays
host transport at 120 BPM. A patch is requested with CC0 = bank / 128, CC32 = bank % 128 and a program change =
index on the part's channel (as the original's `processEvents` reads them, `docs/NOTES.md`). It waits until the
plugin's state chunk has changed and stayed the same for half a second, while processing audio in real time and
pumping window messages, editor idle and plugin idle. Tests: `tests/run_test.sh` builds it for macOS / Linux and runs
it against `tests/mock_plugin.cpp`, a stand-in plugin with the same loading behaviour.
