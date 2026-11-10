# Hypersonic 2 for Schwung (Ableton Move)

The `core/` engine as a [Schwung](https://github.com/charlesvestal/schwung) sound generator for Ableton Move:
four multitimbral parts, every part / patch / element editable, fully spoken by Schwung's screen reader.
Nothing from Steinberg is included: the module reads your own Hypersonic 2 content folder.

## Install

1. Build: `schwung/build.sh` (needs `aarch64-linux-gnu-g++` on PATH, e.g. Homebrew
   `messense/macos-cross-toolchains/aarch64-unknown-linux-gnu`, or Docker). Output: `dist/hypersonic2/` and
   `dist/hypersonic2-module.tar.gz`. The C++ runtime is linked statically; the module needs glibc 2.25 or later.
2. Install: `schwung/install.sh` (SSH to `ableton@move.local`, override with `MOVE_HOST`), or unpack the tarball into
   `/data/UserData/schwung/modules/sound_generators/`.
3. Content: copy the *contents* of your Hypersonic 2 content folder into the module's `content/` folder so that
   `content/Patches` and `content/Samples` exist (also `Wavetables`, `Drawbars`, `Effects`, `Tunings`):
   `scp -r "Hypersonic 2 Content/"* ableton@move.local:/data/U	serData/schwung/modules/sound_generators/hypersonic2/content/`
   (a single subfolder holding the content is found too). Then reload the module or choose **Rescan Content**.
4. Load it into one Signal Chain slot. The module asks for passthrough forwarding (`default_forward_channel` -2); set
   the slot's **Recv Ch** to **All** so all four Move tracks' channels reach it.

## Parts and channels

Move has four tracks, so this build has four parts (the engine's parts 1-4). Each part listens on any channel
(Part Setup > MIDI Channel, 1-16 or all); defaults are part 1 = channel 1 ... part 4 = channel 4. Several parts on one
channel layer. Bank select (CC0, with CC32 when sent) + program change load patches into the parts on that channel
(Settings > MIDI Program Change). Combis (16-part setups) and Syncrosoft copy-protected patches are not offered.

## Controls

| Where | Control | Action |
|---|---|---|
| everywhere | Knobs 1-6 | the active part's Hyperknobs (spoken by the patch's own knob names) |
| everywhere | Knob 7 / 8 | part level / pan |
| everywhere | Touch a knob | speak its value; Shift = fine steps |
| home | Jog | next / previous patch (spoken; the bank too when it changes) |
| home | Shift + Jog | next / previous bank |
| home | Shift + Click | next part, 4 wraps to 1 (spoken: part, channel, patch) |
| home | Click or Menu | open the part menu; its first item, **Active Part**, also picks the part |
| menus | Jog | move (every item spoken with its value; submenus and buttons say so) |
| menus | Click | open a submenu, run a button, start / stop editing a value |
| menus | Jog while editing | change (accelerated; Shift = one step) |
| menus | Menu, Shift + Click | finish editing / up one level |

Schwung does not pass Move's arrow buttons to module screens, so no control depends on them.
| any | Back | leave the module (Schwung keeps this button) |

Values use the original's own display texts (`docs/param_display.json`, `plugin/Source/FxText.h`) and are reworded
for speech: "minus 6.0 dB", "C sharp 3", "1/8 dotted", "minus infinity". Edits apply live.

Menu: Browse Patches (banks > patches) · Part Setup (channel, mute, level, pan, effect sends, level trim, keep) ·
Hyperknobs · Elements (each element of the patch, by type: Main, the type's own page, Key and Velocity, Voice, Pitch,
Glide, LFO 3, Envelope 1, Waveform, Filter, Filter / Amp Envelope, Output, Quick Knobs, Mod 1-16; insert effects:
their own knob names) · Arpeggiator · Keyboard · Voice and Glide · Microtuning · Patch Parameters (patch type, piano,
patch LFOs, knob ranges, GM controls, scenes, arp phrase, arp steps 1-16) · Patch Send Effects · Global Effects
(effect preset from `Effects/` + every knob) · Settings (master volume, program change, effect tail ring-out,
cut notes on load, memory mode, hype, velocity / aftertouch sensitivity, master tune, preload samples) ·
All Notes Off · Rescan Content.

Element Type and an insert's Effect Type are read only (the engine builds those when a patch loads).

## Clock

The engine's VST transport comes from Move: tempo (`get_bpm`), play state (`get_clock_status`) and song position
(`get_beat_position`, 4/4 bars). Tempo-synced effects, LFOs and the arpeggiators follow Move's tempo and phase.

## State

The Move set saves, per part: channel, patch (bank, number, name), every edited part / patch / element value; the
global effect presets and edited knobs; master volume, settings and the active part (`get_param("state")`). Loading a
set reloads the patches and re-applies the edits. A new patch starts from its own values (kept: what the engine keeps
across loads, such as mute and level). Patches are not written back to the content's banks.

## Tests (no Move needed)

`schwung/tests/run_host_test.sh` builds the module for the desktop, writes a synthetic content folder
(`tests/make_test_content.py`: an Analog + insert Delay patch, a Sample patch, an effect preset) and runs
- `tests/host_test.cpp`: the plugin API with a fake Move clock: scan, missing content, loading, every menu address
  (name, text, step), element edits, channel routing, arp on the clock, effect presets, program changes, state
  round trip, rescan. Also run clean under AddressSanitizer and ThreadSanitizer (`EXTRA_CXXFLAGS=-fsanitize=...
  NATIVE_SUFFIX=-asan schwung/build.sh --native`).
- `tests/ui_test.mjs`: the real `ui.js` with Schwung's shared UI libraries against the DSP (`tests/dsp_bridge.cpp`),
  pressing Move's controls and checking what is spoken. Needs node and Schwung's `shared/` folder (`SCHWUNG_SHARED`,
  or taken from `~/schwung.tar.gz`).

## DSP keys

Addresses: `M` master · `P<p>.<j>` part VST parameter · `G<k>.<j>` global effect · `X<p>.<i>` patch parameter ·
`E<p>.<e>.<i>` element (setter index) parameter · `C<p>` channel · `B<p>` patch · `K<p>` bank · `F<k>` effect preset ·
`Z<n>` setting · `A` active part. For each: `n:<addr>` name, `t:<addr>` value text, `v:<addr>` raw value (get/set),
`r:<addr>` read only, `step:<addr>` = delta.
Lists: `pinfo:<p>`, `elems:<p>`, `elay:<p>.<e>`, `play:<p>`, `lay:<group>[.<p>]` (setup, knobs, keys, voice, arp,
tuning, sends, gfx, settings), `banks`, `bankp:<row>`. Also `ready`, `status`, `part`, `state`, `load:<p>`, `rescan`,
`panic`, and the chain knobs `hk1`-`hk6`, `level`, `pan`, `preset`, `preset_count`, `preset_name`.

## Not verified on hardware

Built and tested on the desktop only. Untested on a Move: CPU load of heavy patches (reverbs, many voices) on the
Move's processor; memory use of large sample banks; first-note sample decoding on the audio thread (enable
Settings > Preload Samples to decode while loading, at the cost of memory); the asset installer's handling of the
content's extension-less bank files (copy with `scp` if it refuses them).
