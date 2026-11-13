# HS2 Port plugin

A JUCE plugin around the Hypersonic 2 engine port (`core/`). Like the original it has 16 parts: part N plays MIDI channel N. A patch loads into one part; a combi loads a whole 16-part setup with its global effects. Formats: VST3 and Standalone on Windows; VST3, AU and Standalone on Mac.

The plugin is designed first for use with a screen reader: NVDA, JAWS or Narrator on Windows, and VoiceOver on Mac.

## Status

- The audio matches `hs2render` sample for sample, and `hs2render` matches the original engine. `tests/compare_with_hs2render.py` checks this.
- All 100 factory combis render the same as the original.
- Not in the plugin yet: saving combis, Combi Chain files, multiple outputs, and host parameters for the patch editor's edits (patch parameters, elements, patch send effects; they are saved with the project instead).
- The engine's open items are listed in `docs/NOTES.md`. The Status box and the patch information name any unsupported feature of a loaded patch.

## Building

Configure the top-level project with `-DHS2_BUILD_PLUGIN=ON`. CMake clones JUCE 9.0.5 into the build folder during configure (needs git): from the local checkout `HS2_JUCE_DIR` (default `C:/programs/JUCE`) when that is a git checkout, otherwise from GitHub. The local checkout is never changed.

The build's JUCE copy is patched by `plugin/juce_patch/apply.cmake`, on every platform. The patch gives the VST3 one program list per MIDI channel, so a host's MIDI program changes keep their channel; see "MIDI patch changes" below. Configure stops with an error if the JUCE code is not what the patch expects. A build folder made before the patch (with JUCE taken from `C:/programs/JUCE` directly) can fail with "does not match the source used to generate cache"; delete that build folder and configure again.

### Windows (Visual Studio 2022, from a "x64 Native Tools" prompt)

```
cmake -S . -B build_plugin -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl -DHS2_BUILD_PLUGIN=ON
cmake --build build_plugin --target HS2Port_VST3 HS2Port_Standalone hs2plughost
```

ASIO (standalone app, Windows): CMake uses Steinberg's ASIO SDK from `HS2_ASIO_SDK_DIR` (default `C:/programs/asiosdk/ASIOSDK`, the folder that contains `common/asio.h`). When that folder has no SDK, CMake downloads ASIO SDK 2.3.4 from Steinberg during configure, checked against its SHA-256, the same way it gets JUCE; so CI builds have ASIO too. `-DHS2_ASIO=OFF` builds without ASIO; `-DHS2_REQUIRE_ASIO=ON` (used by `tools/dist/dist-windows.ps1`) stops the configure if ASIO cannot be enabled. The SDK is not in this repository; Steinberg licenses it under its ASIO licence or GPLv3.

Results are in `build_plugin/plugin/HS2Port_artefacts/Release/`:
- `VST3/HS2 Port.vst3`
- `Standalone/HS2 Port.exe`

The build does not install anything. To use the VST3 in a DAW, copy `HS2 Port.vst3` to `C:\Program Files\Common Files\VST3`.

### Mac (Xcode command line tools, CMake 3.22 or later)

```
cmake -S . -B build_plugin -G Xcode -DHS2_BUILD_PLUGIN=ON
cmake --build build_plugin --config Release --target HS2Port_VST3 HS2Port_AU HS2Port_Standalone
```

Or with Ninja instead of Xcode: `cmake -S . -B build_plugin -G Ninja -DCMAKE_BUILD_TYPE=Release -DHS2_BUILD_PLUGIN=ON`, then `cmake --build build_plugin --target HS2Port_VST3 HS2Port_AU HS2Port_Standalone`.

Results are in `build_plugin/plugin/HS2Port_artefacts/Release/` (`VST3/HS2 Port.vst3`, `AU/HS2 Port.component`, `Standalone/HS2 Port.app`). To use them, copy the VST3 to `~/Library/Audio/Plug-Ins/VST3` and the AU to `~/Library/Audio/Plug-Ins/Components`. The plugin looks for the content in `~/Hypersonic 2 Content`; any other folder can be chosen in Settings.

Built on Apple Silicon with Xcode 27; the AU passes `auval`. The engine needs `-ffp-contract=off`, which `core/CMakeLists.txt` sets for Clang. It was checked under MSVC, where `long double` is 64-bit as on Apple Silicon, and matches the original there as well.

### Distribution packages

`tools/dist/` builds release zips into `dist/`:

- **Mac:** `tools/dist/dist-mac.sh` makes `dist/HS2-Port-<version>-Mac.zip` with the VST3, AU and Standalone app, universal (Apple Silicon and Intel, macOS 12 or later). It signs with the first "Developer ID Application" identity in the keychain (or `HS2_SIGN_ID`), with the hardened runtime. To notarize and staple, set `ASC_KEY_ID` and `ASC_ISSUER_ID` for an App Store Connect API key (the .p8 is read from `~/.appstoreconnect/private_keys/AuthKey_<key id>.p8`, or `ASC_KEY_PATH`), or `HS2_NOTARY_PROFILE` for a `notarytool` keychain profile; without either, other Macs show a Gatekeeper warning.
- **Windows:** `powershell -ExecutionPolicy Bypass -File tools\dist\dist-windows.ps1` makes `dist\HS2-Port-<version>-Windows.zip` with the VST3 and Standalone exe. It finds Visual Studio itself (no developer prompt needed). Set `HS2_SIGN_CERT` (a .pfx) and `HS2_SIGN_PASSWORD` to sign with signtool.
- **Windows 7, 8 and 8.1:** the same script with `-Windows7` makes `dist\HS2-Port-<version>-Windows7.zip`. JUCE 8 and 9 need Windows 10, so this build uses JUCE 7.0.12 (CMake option `HS2_JUCE7=ON`, its own build folder `build_dist_win7`), with the C++ runtime linked in so nothing else has to be installed. JUCE 7 gets the same per-channel program change patch, plus JUCE 8's fix that gives a choice box its title as its accessible name (JUCE 7 uses the shown text). The script then runs `tools\dist\check_win7_imports.py`, which fails the build if the plugin imports any Windows function newer than Windows 7. The tests run against it with `HS2_BUILD=build_win7` (`compare_with_hs2render.py`, `keyboard_walk.ps1`). Code for both builds: JUCE 8 / 9 only API behind `#if JUCE_MAJOR_VERSION >= 8`.

The version comes from `VERSION` in `plugin/CMakeLists.txt`.

**Releases by CI.** `.github/workflows/release.yml` runs on version tags or by manual dispatch, not on ordinary branch pushes. Pushing a tag `v<version>` (it must match `VERSION`) builds and publishes the notarized Mac zip, both Windows zips, and both Ableton Move modules: the Hypersonic 2 sound generator and HS2 MIDI Player. A manual run builds the same targets as artifacts without publishing a release. The Mac signing and notarization secrets are listed at the top of the workflow.

## Content

The plugin reads the user's own Hypersonic 2 Content folder; nothing from it is copied into the plugin.

- **Windows:** the folder comes from the original's registry entry, `HKLM\SOFTWARE\WOW6432Node\Steinberg\Hypersonic 2\Content Location`. If that entry is missing, it falls back to `C:\Hypersonic 2 Content`.
- **Mac:** the default is `~/Hypersonic 2 Content`. Use "Choose content folder..." in Settings to point at the real folder.

The folder choice and the piano memory mode are saved in the user settings file `HS2Port/HS2Port.settings` and shared by all instances.

## Presets (DAW preset browsers and Komplete Kontrol)

Komplete Kontrol banks carry their original numbers, with two digits below 100 (for example `00 HS1 Preview Patches` and `30 Accordions + Harmonicas`). Three-digit banks use a label such as `Bank 120 Combis` so they sort after the two-digit banks. Preset names show their one-based position within the bank, padded to two digits or three when needed. Preset filenames and DAW names are unchanged.

In Preferences > General, choose separate VST3 and Komplete Kontrol preset folders with Browse. Use Default restores the standard path for that format. The confirmation lists the exact folders before writing. Add a custom NKS location to Komplete Kontrol's User library and rescan. Changing the destination does not move or delete presets in the previous location. On Mac, Audio Unit presets keep their standard folder.

"Install presets..." in Settings writes a preset file for every patch and combi of the content folder, so they can be chosen from the DAW's preset browser and from Komplete Kontrol. The files hold only the patch reference (bank, number and name); the sound still comes from the content folder. Copy-protected patches without a capture are left out.

The 63 copy-protected factory patches load from captures in `<content>\Captures\patches` (see tools/hs2capture, --mode patch): plain "StSS" files of those patches, taken out of your licensed Hypersonic 2 by that tool. There is no setting; the patch browser marks them "(captured)" and they load like any other patch. Captures are personal-use data like the rest of your content: keep them on your machine, do not distribute them.

| Format | Windows | Mac |
|---|---|---|
| VST3 (`.vstpreset`) | `Documents\VST3 Presets\HS2 Port\HS2 Port\` | `~/Library/Audio/Presets/HS2 Port/HS2 Port/` |
| Audio Unit (`.aupreset`) | - | `~/Library/Audio/Presets/HS2 Port/HS2 Port/` |
| Komplete Kontrol (`.nksf`) | `Documents\Native Instruments\User Content\HS2 Port\` | `~/Documents/Native Instruments/User Content/HS2 Port/` |

- Komplete Kontrol presets are written only when Komplete Kontrol is installed (Windows: its registry entry or `C:\Program Files\Native Instruments\Komplete Kontrol`; Mac: the app in `/Applications`).
- Folders: `Patches/<bank>/` and `Combis/<bank>/`.
- A patch preset loads the patch into part 1 and empties the other parts; a combi preset loads the combi. The Settings choices, the selected part and the master volume stay as they are.
- On launch, the standalone offers a one-time upgrade when Komplete Kontrol and an older bank installed by HS2 Port are found. It explains the new mappings and bank order. Choose Later to skip, or run Install presets from Preferences at any time.
- Reinstalling replaces only files listed in the installer's receipts (`.hs2port-*-presets.json`, or the older `.hs2port-presets.json`). Presets saved by hand are kept. New files and previews are staged on the destination drive first. Choose Cancel preset installation in Preferences, or close the app during generation, to discard that work; the next attempt starts over. Once replacement starts, the app finishes it or restores the previous files on error. If interrupted during replacement and recovery files remain, the installer reports their location instead of overwriting them.
- Komplete Kontrol: the presets appear under User, with bank names, sound types, and patch descriptions. Single patches group the named Hyperknobs, mixer, voice, key and velocity, tuning, arpeggiator, and global FX routing. Combis include controls for populated parts. The engine renders a short, sound-appropriate Ogg preview for each preset. Komplete Kontrol 3 on Mac requires the VST3; rescan plug-ins first if HS2 Port is new to Komplete Kontrol.
- `hs2presets --content <folder> [--vst3 DIR] [--au DIR] [--nks DIR] [--defaults] [--no-previews]` (built with the plugin) writes the same files from the command line; `--no-previews` leaves out the Komplete Kontrol previews.

Host parameter ids are the parameter numbers (JUCE legacy parameter ids, since 0.3.0), which the Komplete Kontrol knob pages refer to. Automation or MIDI-learn assignments made with an earlier version may need to be assigned again; the plugin's saved setups are not affected.

## Using it with a screen reader

When the window opens, focus goes to the Part selector. Tab and Shift+Tab move through the visible controls. Ctrl+F takes you directly to search. Ctrl+I opens the larger Patch information page and focuses its read-only text; use the arrows, Home/End or Page Up/Down to review it.

The current-sound button steps through sounds with Up/Down, jumps ten with Page Up/Down and opens the browser with Enter. Choice controls use the same stepping keys; Enter opens their menus. Sliders use Enter to edit their value: engine parameters take a normalized decimal from 0 to 1, while Octave and Velocity take their displayed values. Escape cancels the edit.

The standalone Musical Keyboard toggles with Ctrl+K (Command+K on Mac). It plays the selected part using the Canvas layout through apostrophe (upper F), with Z/X for octave and C/V for velocity. Search and direct entry pause typing and release held notes. The instrument plug-in still uses the host's MIDI input.

1. **Part:**
   - Part selector. Each entry names its patch, for example "Part 3: Soft Rubber Bass" or "Part 4: empty". The patch browser, Current patch, Hyperknobs and Part mixer work on the selected part.
   - Clear part.
   - Show: what the right side shows for the selected part: Patch and mixer (Current patch, Hyperknobs, Part mixer), Part settings, Arpeggiator, Microtuning, Patch parameters, Elements, Patch send effects, Global effects, or Global settings. Ctrl+Tab and Ctrl+Shift+Tab change the page and move to its first control.
2. **Patch browser:**
   - Search box. It searches patch, combi and bank names.
   - Bank filter. The combi banks ("120 Combis", "121 User Combis") hold combis.
   - Current sound. Up and Down step through patches; Page Up and Page Down jump ten. This is the Tab stop immediately before the type-to-select patch list.
   - Patch list. Its name includes how many patches are shown, for example "Patches, 3 shown". Combis end in "(combi)".
   - Load selected patch, Previous patch, Next patch. Ctrl+Page Up and Ctrl+Page Down change patches without moving focus to the list. On Windows, Alt+W focuses Current sound. The list keeps its own type-to-select keys when the musical keyboard is on.
   - "Load patch when selected in the list". When this is on, moving through the list loads each patch into the selected part, so you can play it straight away. Moving onto a combi loads the whole combi.
3. **Current patch:** a read-only text box with the selected part's patch: name, bank, description, controller notes (what the mod wheel and aftertouch do), the Hyperknob names, the author, unsupported features, and the loaded combi.
4. **Part N Hyperknobs:** six sliders named after the patch's own knobs, for example "Hyperknob 4: Thump". Values are percentages.
5. **Part N mixer:** Mute, Level, Pan, Effect send 1 to 4. Level and sends read as a percentage and in dB.
6. **Master:** master volume in dB (all parts).
7. **Status:** a read-only text box with loading progress, errors, and features of the loaded patches that the port does not support yet. Use Alt+S on Windows or Control+S on Mac to focus it, then arrows or Page Up/Down to read it.

**Preferences** (Ctrl+comma or Command+comma) opens inside the editor. General contains content, playback options, and preset export. In the standalone, the Audio and MIDI tab contains driver, output, sample rate, buffer size, MIDI inputs, and audio status. Escape returns to the instrument. F1 opens reviewable help in the editor, and F9 sends All Notes Off.

In the Windows standalone, use the native window controls to minimize or choose **Hide to tray** (Ctrl+H) to keep playback available without a taskbar window. Use the notification icon or launch the app again to restore it. Shift+F1 checks GitHub and can open the releases page; it does not download or install an update.

The Mac standalone exposes a virtual MIDI destination called **HS2 Port** while it is running. Turn it off in Preferences > Audio and MIDI if you don't need it. Command+Shift+H hides the window to the menu bar without stopping playback. Use **Show Controls** from the HS2 Port menu-bar item or reopen the app from Applications.

Existing automation IDs and preset identities are unchanged. Detailed patch-element edits are available in the editor rather than as NKS automation.

### Pages

The right side of the window shows one page at a time (the Show box, or Ctrl+Tab / Ctrl+Shift+Tab):

- **Patch and mixer:** Current patch, Part N Hyperknobs, Part N mixer (the sections listed above).
- **Part N settings:** Output, Key Low, Key High, Key Crossfade, Velocity Low, Velocity High, Velocity Crossfade, Transpose, Fine Tune, Max Voices, Effect Sends (global or the patch's own), Pitch Bend Up / Down, Mono / Poly, Glide, Glide Time, for parts 2 to 16 Link (the part also plays the notes of the part above it), Keep Settings (the part keeps these settings when another patch loads) and Level Trim.
- **Part N arpeggiator:** Arpeggiator on / off, Latch, Mode, Octaves, Steps, Sync, Rate, Gate, Swing.
- **Part N microtuning:** two tunings (bank, scale and root note each; the bank and scale names come from the content's Tunings folder) and the depth.
- **Patch parameters** (the selected part's patch): a Group box (Patch, Piano, Patch LFOs, Knob Ranges, GM Controls, Scenes, Arp Phrase, Arp Step 1 to 16), then that group's parameters by the original's names and texts.
- **Elements** (the selected part's patch): an Element box ("Element 1: Sample, Pad Saw"; an insert effect: "Element 3: Effect, Quad Chorus"), a Group box (Main, the element type's own parameters, Key and Velocity, Voice, Pitch, Glide, LFO 3, Envelope 1, Waveform, Filter, Filter Envelope, Amp Envelope, Output, Quick Knobs, and Mod 1 to 16, each named by its source and destination: "Mod 3: LFO 1 to Pitch"; an insert effect: Effect and Quick Knobs), then that group's parameters. A sample element's bank and sample read by their names; Element Type and an insert's Effect Type are read only. A group with more than 16 parameters is split ("Analog, 1 of 2").
- **Patch send effects** (the selected part's patch): a Send effect box ("Send effect 1: Hall Reverb"), then Mute, Level, Pan, Aux Output, the effect's own knobs and the sends to effects 1 to 4.
  - The three patch pages are those of the Schwung module (`schwung/README.md`), with the same names and texts. Changes apply at once and are kept with the project; they belong to the patch they were made on, so loading another patch into the part (or a combi) starts from that patch's own values. Sliders step from one displayed value to the next; Delete returns to the patch's own value.
- **Global effects:** the effect slot ("Effect 1: Long Reverb (Hall Reverb)": the effect's name and type), the Effect box (the effect presets of the content's Effects folder, as the original's effect browser: "Delay: Mono Delay", "Chorus: Ensemble 1"..., or No effect; Up and Down load each one at once, and the choice is saved with the project; the combi's own effect shows as "..., as loaded" until you choose another), then Mute, Level, Pan, Aux Output, the effect's own knobs by the original's names and texts (a Hall Reverb's Mix, Time, Pre Del, HF Damp, Low EQ, Hi EQ; "Pre Del = 5 ms"; knobs the effect doesn't use are left out), and for effects 2 to 4 the send to effect 1. A delay's Delay knob reads in seconds, or as a note value ("1/8") when its Sync is on.
- **Global settings** (all parts, saved with the project): Hype (the original's tone enhancer drive and master level, 33 % by default), Velocity sensitivity and Aftertouch sensitivity (50 % is linear), Master tune (-12 to +12 semitones), Master fine tune (-50 to +50 cents), Preload samples (decode a patch's samples while it loads: more memory, no decoding when the first notes play).

Settings with a fixed list of values (note names, Transpose, Arp Mode...) are choice boxes: Up and Down arrows choose. The others are sliders with the slider keys. Every one is also a host parameter (below), with the same value text.

### Keys

| Key | Action |
|---|---|
| Ctrl+P | Part selector. Up and Down arrows choose the part. |
| Ctrl+Shift+Page Down / Ctrl+Shift+Page Up | Next / previous part (focus moves to the part selector, which reads the part and its patch) |
| Ctrl+F | Search box. Type words from the patch, combi or bank name; Enter moves to the list. |
| Ctrl+L | Patch list. Type letters to jump to a patch; Enter loads it into the selected part (a combi into all parts). |
| Ctrl+Page Down / Ctrl+Page Up | Next / previous patch in the list |
| Ctrl+I | Patch information of the selected part |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous page (Patch and mixer, Part settings, Arpeggiator, Microtuning, Patch parameters, Elements, Patch send effects, Global effects, Global settings), focus on its first control |
| Arrows, Page Up / Page Down, Home / End | Change the focused slider (Page Up / Page Down: a tenth of the range; Home / End: lowest / highest) |
| Delete or Backspace | Return the focused slider to the patch's own setting (the value it loaded with; Master: the default volume) |
| F1 | Keyboard help |

On a Mac, use Cmd instead of Ctrl, except for Ctrl+Tab and Ctrl+Shift+Tab, which stay Ctrl (Cmd+Tab is the system's app switcher).

In REAPER, turn on "Send all keyboard input to plug-in" in the FX window, so that these keys reach the plugin.

### Design rules

- **Stock JUCE controls only.** Each one has a title, a description, a role and a place in the focus order. Visual labels are hidden from accessibility, so names are not read twice.
- **Grouped sections.** The sections are UI Automation groups with names. The sections that work on the selected part say which ("Part 3 mixer").
- **No spoken announcements.** On Windows, JUCE's `postAnnouncement` speaks through its own SAPI voice, not through the user's screen reader, so it is not used. Information is text the screen reader reads in the normal way:
  - The part selector names each part's patch.
  - The patch list's name carries the match count.
  - Status is a text box.
  - When an action the user asked for fails, focus moves to the Status box, so the error is read.
- **Focus never sits on an unusable component.** If the window has focus but no control does, focus goes to the search box. Inside a DAW this only happens while the plugin's own window has focus, so the plugin never takes focus from the DAW.
- **Readable by host generic UIs.** The host parameters are the original Hypersonic 2's: Master Volume; per part Mute, Level, Pan, the four effect sends and the six Hyperknobs ("Part 3 Level", "Part 3 Hyperknob 4"); per part the settings, microtuning and arpeggiator ("Part 3 Transpose", "Part 3 Arp Rate"); Link for parts 2 to 16; Keep Settings and Level Trim per part; and the four global effects' Mute, knobs, Aux Output, Level, Pan and sends ("Global Effect 2 Level"; a knob's value text starts with the effect's own name for it: "Global Effect 1 Knob 4 = Pre Del, 5 ms"). 864 in all (Keep Settings and Level Trim came last in 0.5.0, so the numbers of the others did not change), each with value text from the original's own display ("+12", "C3", "Legato", "1/16"), so a host's generic UI or OSARA's parameter list can read and set them. A Hyperknob's value text starts with the patch's own knob name ("Thump, 50 %").

## How it works

- **Background loading.** Patch and combi files are read on a background thread, which also loads the samples into memory. The ready patch then goes into the running engine at the start of the next audio block (about 0.1 to 2 ms), so the other parts keep playing. A complete new engine is built only for a state restore, a new content folder or a new sample rate; the audio thread only try-locks that swap, so it never stalls. Offline renders wait for pending loads, so a render never starts silent.
- **MIDI patch changes.** As in the original: bank select (controller 0 = 0, controller 32 = bank number) followed by a program change loads that patch into the part of the MIDI channel. Banks 120 and up load combis. This can be turned off in Settings.
- **Reset part links:** Ctrl+R (Cmd+R on Mac), or Reset part links in Preferences > General, turns off every Link after using a layered combi. Each part then plays its own MIDI channel. Patches, mixer and device settings are unchanged. Loading another combi can restore its own links.
  - Senders that put the bank number on controller 0 alone (no controller 32) work too: until a MIDI channel has received a controller 32, a controller 0 value is taken as the bank number. A bank select on its own does not load anything; the program change that follows does.
  - VST3 has no MIDI program change event. A host maps a program change on MIDI channel N to the program list of channel N's unit; the plugin has one per channel (JUCE patch), so program changes on several channels at once all arrive, each in its own part (for example a MIDI file with bank and program changes on every channel at the start).
  - A host that does not use per-channel units uses the plugin's main program list (programs 0 to 127, plus "None"), without the MIDI channel. The plugin then uses the channel of the latest bank select, or part 1 if none came yet.
- **Effect tails.** The original cuts some delay tails when the last note ends (it lets a part sleep as soon as it seems silent, and checks only the left channel). "Let effect tails ring out" (on by default) lets them play out; turn it off to hear exactly the original.
- **Host transport.** Tempo, position, time signature and play state are passed to the engine every block. Tempo-synced LFOs, delays and the arpeggiator follow the host.
- **Saved state.** The state holds the loaded combi and each part's patch (bank, number and name), the host parameter values, the patch editor's edits of each part, the selected part, the MIDI / effect-tail settings and the global settings. It never contains Steinberg data; the patches are read from the content folder again.
- **New instance.** As in the original, part 1 holds "Welcome To The Matrix" (bank 084 HS2 Preview Patches, patch 0); the other parts are empty.

## Tests

- **`tests/compare_with_hs2render.py`:** the VST3, loaded by `hs2plughost` the way a DAW loads it, must render exactly what `hs2render` renders in engine mode. It covers:
  - patches loaded by MIDI program change into part 1 and into part 2
  - combis loaded by MIDI program change
  - state save and restore of each
  - 48 kHz with 512-frame blocks
  - a Hyperknob and a part level moved by the host, also after a state round trip
- **`tests/edit_checks.py`:** the patch editor's edits and the global settings through `hs2plughost` (`--state-edit`, `--state-attr`): muting every element silences the patch, an element's Octave changes it, edits and settings come back identically from the saved state, the global settings at their defaults leave the output unchanged, and Keep Settings / Level Trim are host parameters. `HS2_BUILD` names the build folder (default `build_plugin`).
- **`hs2_preset_transaction`:** pass a new, disposable folder to check cancellation, rollback, legacy receipts, and preservation of user presets. Windows also tests rollback with an open file blocking replacement.
- **`tests/preset_destinations.py`:** pass the `hs2presets` executable, a Hypersonic 2 content folder and a scratch directory to exercise a full export and reinstall across the three formats. **`tests/verify_nks_mapping.py`** checks the resulting bank order, mappings and embedded VST3 states; `--ffmpeg` also checks previews.
- **`tests/keyboard_walk.ps1`:** opens the standalone app for about half a minute and checks its keyboard behaviour through UI Automation, which is what screen readers read:
  - where focus lands at start
  - the Tab order
  - search, Enter, arrows and type-ahead
  - the shortcuts
  - the part selector, a patch loaded into part 3, and a combi
