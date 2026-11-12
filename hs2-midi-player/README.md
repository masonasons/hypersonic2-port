# HS2 MIDI Player (Schwung tool for Ableton Move)

Plays Standard MIDI Files, and MIDI from a USB device, on all 16 parts of the `core/` Hypersonic 2 engine: part N
plays MIDI channel N. Program changes and bank select load patches, through the engine's General MIDI mapping by
default. Everything is spoken by Schwung's screen reader. Nothing from Steinberg is included: it reads your own
Hypersonic 2 content folder.

It is a Schwung **tool** (Tools menu), not a sound generator: it has its own full-screen player and plays on its own,
outside the Signal Chain slots. Unlike Schwung's MIDI Player (a MIDI effect that feeds one slot's synth on one
channel), it keeps the file's 16 channels apart.

## Install

1. Build: `hs2-midi-player/build.sh` (the same toolchain as `schwung/build.sh`: `aarch64-linux-gnu-g++` on PATH, or
   Docker). Output: `dist/hs2-midi-player/` and `dist/hs2-midi-player-module.tar.gz`.
2. Install: `hs2-midi-player/install.sh` (SSH to `ableton@move.local`, override with `MOVE_HOST`), or unpack the
   tarball into `/data/UserData/schwung/modules/tools/`.
3. Content: nothing to do if the Hypersonic 2 sound generator (`schwung/`) is installed with its content; the tool
   reads `modules/sound_generators/hypersonic2/content`. Otherwise copy the content into the tool's own `content/`
   folder (same layout: `content/Patches`, `content/Samples`, ...).
4. MIDI files: anywhere under `/data/UserData/UserLibrary` (for example a `MIDI` folder). `.mid`, `.midi`, `.kar`
   and RIFF `.rmi` files are listed.

## Controls

| Where | Control | Action |
|---|---|---|
| player | Play | play / stop (Follow Move: presses Move's own Play) |
| player | Shift + Play | back to the start |
| player | Loop | loop on / off |
| player | Jog | one bar back / on (spoken: "Bar 5 of 32"); Shift = 8 bars |
| player | Click or Menu | the menu |
| player | Shift + Click | speak the status: file, position, tempo, CPU load, voices |
| everywhere | Pads, bottom two rows | parts 1-16 (bottom row 1-8): press = mute, Shift + press = solo |
| everywhere | Knobs 1-8 | level of parts 1-8; with Shift held, parts 9-16. Touch speaks part, patch and level |
| menus | Jog / Click / Menu | move / open, edit or run / finish editing or up one level |
| file browser | Jog / Click | move / open a folder or the file; Shift + Click = up one folder; Menu = back |
| anywhere | Back | hide the tool; it keeps playing (Schwung's `suspend_keeps_js`). Shift + Back exits it |

Pad lights: off = the file does not use the part, grey = used, white = playing now, red = muted, yellow = solo.

Menu: Open MIDI File (Schwung's shared file browser, starting in the last folder) · Play / Stop · Back to Start ·
Loop · Parts (for each part: its patch, Choose Patch from any bank, Lock Patch, Mute, Solo, Level, Pan, voices) ·
Tempo (from the file / Follow Move) · Program Changes (General MIDI / Hypersonic banks) · USB MIDI Input ·
Master Volume · Preload Samples · Effect Tails (ring out / cut) · File Info · Status · All Notes Off · Rescan Content.
Settings are kept in the tool's `settings.json`.

## How it plays

- **Tempo from the file** (default): the file's tempo map, with every tempo change. Time signatures give the bars.
- **Follow Move**: the file's quarter notes run on Move's beats, at Move's tempo (the file's tempo changes are
  ignored), and Move's Play and Stop start and stop it. It starts on Move's bar lines; with Loop on it starts again
  where its last bar ends. The player's Play button presses Move's Play.
- **Program changes**: with General MIDI on, the engine maps programs to banks 078 (instruments) and 079 (drums,
  channel 10, or bank select 120 elsewhere), as the original's GM mode does; a GM program the content does not have
  plays its instrument family's first patch, a missing drum kit the standard kit. With it off, bank select (CC0 alone,
  CC32 alone, or CC0 * 128 + CC32) picks the content bank and the program its patch.
- **No waiting at program changes**: when a file opens, every patch it asks for is read ahead, so a program change in
  the middle of a song switches the part inside the same audio block (the notes after it play the new patch).
  Playback starts once the file's patches are ready ("Preparing" meanwhile).
- **Seeking** stops the sounding notes and sends each channel's controllers, pitch bend, bank and program as they are
  at the new position.
- **USB MIDI Input** (on by default): MIDI from a device on Move's USB-A port plays the parts by channel, with bank
  select and program changes, alongside the file or alone. Move's own pads and knobs control the tool instead.
- **Patches chosen by hand** lock the part: the file's program changes leave it alone until Lock Patch is off.
- **Sample memory**: as in the Hypersonic 2 module, samples no part can play are freed after loads, and each part's
  samples decode on the loader thread after it loads (up to 256 MB in all), not on the audio thread.

## Not verified on hardware

Built and tested on the desktop only. To check on a Move:

- **CPU**: 16 parts at once may be more than the Move's processor manages; the player shows the audio thread's load
  (Status, Shift + Click) and the voice count, so heavy files can be judged. Reverbs and long sample patches cost
  the most. Mute parts (pads) to lighten a file.
- **USB MIDI echo**: Move's firmware may also play its own tracks from the same USB MIDI. If so, set Move's tracks to
  another MIDI input channel while using the tool.
- **Move's Play button injection** (Follow Move) uses the API Song Mode uses; untested here.
- The bottom-left pad is taken to be note 68 (Schwung's `MovePad1`).

## Tests (no Move needed)

`hs2-midi-player/tests/run_tests.sh` builds the tool for the desktop, writes synthetic content (GM banks 078 / 079)
and MIDI files (`tests/make_test_files.py`: two tempos, mid-song program changes, drums on channel 10, all 16
channels, a broken file) and runs

- `tests/host_test.cpp`: the plugin API with a fake Move clock: reading ahead, playing at the file's tempo map,
  program changes inside a block, seeking and the chase, the end and looping, Follow Move (start on Move's bar lines,
  stop with Move), mute / solo, locked parts, USB MIDI program changes, 16 parts at once, GM off, settings.
- `tests/ui_test.mjs`: the real `ui.js` with Schwung's shared UI libraries (and its shared file browser) against the
  DSP, pressing Move's controls and checking what is spoken.

`host_test` also passes under AddressSanitizer (`EXTRA_CXXFLAGS="-fsanitize=address -g" NATIVE_SUFFIX=-asan
hs2-midi-player/tests/run_tests.sh`). On macOS the UI test cannot run a sanitized build, because Node loads the
bridge too late for the sanitizer's interceptors.

## DSP keys

get: `ready` (0 scanning, 1 ready, -1 no content), `status`, `song` (file info JSON), `pos` (transport JSON: play,
prep, sec, len, bar, bars, beat, bpm, sync, wait, end, loop), `parts` (16 parts: name, bank, mute, solo, lock, used,
voices, notes, level, pan, missing), `cpu` (average %, peak %, voices), `mem` (decoded sample bytes), `cfg`
(settings JSON), `banks`, `bankp:<row>`, `module_id`.
set: `file_path` / `open` (a MIDI file), `transport` (play, stop, toggle, rewind), `seek_bar` (1-based),
`step_bar` (delta), `mute:<p>` / `solo:<p>` / `lock:<p>` (0, 1, toggle), `level:<p>` / `pan:<p>` (value, or +n / -n
in percent), `load:<p>` (`<bank>.<index>`, locks the part), `cfg:<tempo|gm|usb|loop|preload|ringout|master>`,
`lastdir`, `panic`, `rescan`.
