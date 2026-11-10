# HS2 Port plugin

A JUCE plugin around the Hypersonic 2 engine port (`core/`). Like the original it has 16 parts: part N plays MIDI channel N. A patch loads into one part; a combi loads a whole 16-part setup with its global effects. Formats: VST3 and Standalone on Windows; VST3, AU and Standalone on Mac.

The plugin is designed first for use with a screen reader: NVDA, JAWS or Narrator on Windows, and VoiceOver on Mac.

## Status

- The audio matches `hs2render` sample for sample, and `hs2render` matches the original engine. `tests/compare_with_hs2render.py` checks this.
- All 100 factory combis render the same as the original.
- Not in the plugin yet: saving combis, Combi Chain files, multiple outputs, and host parameters for the part settings beyond the mixer and Hyperknobs (key ranges, transpose, arpeggiator, global effects).
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

**Releases by CI.** `.github/workflows/release.yml` runs both scripts on GitHub Actions. Pushing a tag `v<version>` (it must match `VERSION`) builds the notarized Mac zip and the Windows zip and publishes them as a GitHub release; running the workflow by hand builds the zips as artifacts only. The Mac signing and notarization secrets are listed at the top of the workflow.

## Content

The plugin reads the user's own Hypersonic 2 Content folder; nothing from it is copied into the plugin.

- **Windows:** the folder comes from the original's registry entry, `HKLM\SOFTWARE\WOW6432Node\Steinberg\Hypersonic 2\Content Location`. If that entry is missing, it falls back to `C:\Hypersonic 2 Content`.
- **Mac:** the default is `~/Hypersonic 2 Content`. Use "Choose content folder..." in Settings to point at the real folder.

The folder choice and the piano memory mode are saved in the user settings file `HS2Port/HS2Port.settings` and shared by all instances.

## Presets (DAW preset browsers and Komplete Kontrol)

"Install presets..." in Settings writes a preset file for every patch and combi of the content folder, so they can be chosen from the DAW's preset browser and from Komplete Kontrol. The files hold only the patch reference (bank, number and name); the sound still comes from the content folder. Copy-protected patches are left out.

| Format | Windows | Mac |
|---|---|---|
| VST3 (`.vstpreset`) | `Documents\VST3 Presets\HS2 Port\HS2 Port\` | `~/Library/Audio/Presets/HS2 Port/HS2 Port/` |
| Audio Unit (`.aupreset`) | - | `~/Library/Audio/Presets/HS2 Port/HS2 Port/` |
| Komplete Kontrol (`.nksf`) | `Documents\Native Instruments\User Content\HS2 Port\` | `~/Documents/Native Instruments/User Content/HS2 Port/` |

- Komplete Kontrol presets are written only when Komplete Kontrol is installed (Windows: its registry entry or `C:\Program Files\Native Instruments\Komplete Kontrol`; Mac: the app in `/Applications`).
- Folders: `Patches/<bank>/` and `Combis/<bank>/`.
- A patch preset loads the patch into part 1 and empties the other parts; a combi preset loads the combi. The Settings choices, the selected part and the master volume stay as they are.
- Running it again replaces only the files it wrote before (a `.hs2port-presets.json` receipt lists them); presets saved there by hand are kept.
- Komplete Kontrol: the presets appear in the browser under User, with the bank as the bank name, a sound type per bank (for example Synth Lead for Soft Leads) and the patch's author and description. Knob page 1 holds the patch's six Hyperknobs (with the patch's own names), Level and Pan; page 2 the four effect sends, Level, Pan, Mute and Master. Combis: the part levels (parts 1 to 8, then 9 to 16). Each preset has a sound preview (`.previews/<name>.nksf.ogg`, about 4 seconds, Ogg Vorbis): the engine plays a short phrase that suits the sound type (a chord phrase, a bass line, a drum groove, a percussion run, or one held key for loops and effects); a patch that stays silent with it gets a phrase on the keys that do sound. Writing the previews takes about three minutes for the factory content. Komplete Kontrol 3 on Mac hosts VST3 only: it loads the presets once it has scanned `HS2 Port.vst3`, so if HS2 Port is new to Komplete Kontrol, rescan the plug-ins first (Preferences, Plug-ins).
- `hs2presets --content <folder> [--vst3 DIR] [--au DIR] [--nks DIR] [--defaults] [--no-previews]` (built with the plugin) writes the same files from the command line; `--no-previews` leaves out the Komplete Kontrol previews.

Host parameter ids are the parameter numbers (JUCE legacy parameter ids, since 0.3.0), which the Komplete Kontrol knob pages refer to. Automation or MIDI-learn assignments made with an earlier version may need to be assigned again; the plugin's saved setups are not affected.

## Using it with a screen reader

When the window opens, focus is in the search box. Tab and Shift+Tab move through the controls in this order (Tab wraps around, so the Part section comes after Status):

1. **Part:**
   - Part selector. Each entry names its patch, for example "Part 3: Soft Rubber Bass" or "Part 4: empty". The patch browser, Current patch, Hyperknobs and Part mixer work on the selected part.
   - Clear part.
   - Show: what the right side shows for the selected part: Patch and mixer (Current patch, Hyperknobs, Part mixer), Part settings, Arpeggiator, Microtuning, or Global effects. Ctrl+Tab and Ctrl+Shift+Tab change the page and move to its first control.
2. **Patch browser:**
   - Search box. It searches patch, combi and bank names.
   - Bank filter. The combi banks ("120 Combis", "121 User Combis") hold combis.
   - Patch list. Its name includes how many patches are shown, for example "Patches, 3 shown". Combis end in "(combi)".
   - Load selected patch, Previous patch, Next patch.
   - "Load patch when selected in the list". When this is on, moving through the list loads each patch into the selected part, so you can play it straight away. Moving onto a combi loads the whole combi.
3. **Current patch:** a read-only text box with the selected part's patch: name, bank, description, controller notes (what the mod wheel and aftertouch do), the Hyperknob names, the author, unsupported features, and the loaded combi.
4. **Part N Hyperknobs:** six sliders named after the patch's own knobs, for example "Hyperknob 4: Thump". Values are percentages.
5. **Part N mixer:** Mute, Level, Pan, Effect send 1 to 4. Level and sends read as a percentage and in dB.
6. **Master:** master volume in dB (all parts).
7. **Settings:** content folder, "Choose content folder...", piano memory mode (ECO, MID or XXL), "Load patches and combis from MIDI program changes", "Let effect tails ring out", "Stop the old sound when a part's patch changes", "Install presets..." (see Presets), "Audio and MIDI settings..." (standalone app only), Keyboard help.
   - **Audio and MIDI settings** (standalone app): a window with Audio driver (ASIO on Windows when built with the ASIO SDK, Windows Audio, DirectSound; Core Audio on Mac), Output device, Sample rate, Buffer size (with its length in milliseconds), Refresh devices, Driver control panel (the ASIO driver's own settings), one "MIDI input: name" check box per MIDI input, a read-only Audio status box (device, rate, buffer, latency, errors) and Close. Changes apply at once and are saved; Escape closes the window.
8. **Status:** a read-only text box with loading progress, errors, and features of the loaded patches that the port does not support yet.

### Pages

The right side of the window shows one page at a time (the Show box, or Ctrl+Tab / Ctrl+Shift+Tab):

- **Patch and mixer:** Current patch, Part N Hyperknobs, Part N mixer (the sections listed above).
- **Part N settings:** Output, Key Low, Key High, Key Crossfade, Velocity Low, Velocity High, Velocity Crossfade, Transpose, Fine Tune, Max Voices, Effect Sends (global or the patch's own), Pitch Bend Up / Down, Mono / Poly, Glide, Glide Time, and for parts 2 to 16 Link (the part also plays the notes of the part above it).
- **Part N arpeggiator:** Arpeggiator on / off, Latch, Mode, Octaves, Steps, Sync, Rate, Gate, Swing.
- **Part N microtuning:** two tunings (bank, scale and root note each; the bank and scale names come from the content's Tunings folder) and the depth.
- **Global effects:** the effect slot ("Effect 1: Long Reverb (Hall Reverb)": the effect's name and type), the Effect box (the effect presets of the content's Effects folder, as the original's effect browser: "Delay: Mono Delay", "Chorus: Ensemble 1"..., or No effect; Up and Down load each one at once, and the choice is saved with the project; the combi's own effect shows as "..., as loaded" until you choose another), then Mute, Level, Pan, Aux Output, the effect's own knobs by the original's names and texts (a Hall Reverb's Mix, Time, Pre Del, HF Damp, Low EQ, Hi EQ; "Pre Del = 5 ms"; knobs the effect doesn't use are left out), and for effects 2 to 4 the send to effect 1. A delay's Delay knob reads in seconds, or as a note value ("1/8") when its Sync is on.

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
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous page (Patch and mixer, Part settings, Arpeggiator, Microtuning, Global effects), focus on its first control |
| Arrows, Page Up / Page Down, Home / End | Change the focused slider (Page Up / Page Down: a tenth of the range; Home / End: lowest / highest) |
| Delete or Backspace | Return the focused slider to the patch's own setting (the value it loaded with; Master: the default volume) |
| F1 | Keyboard help |

On a Mac, use Cmd instead of Ctrl.

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
- **Readable by host generic UIs.** The host parameters are the original Hypersonic 2's: Master Volume; per part Mute, Level, Pan, the four effect sends and the six Hyperknobs ("Part 3 Level", "Part 3 Hyperknob 4"); per part the settings, microtuning and arpeggiator ("Part 3 Transpose", "Part 3 Arp Rate"); Link for parts 2 to 16; and the four global effects' Mute, knobs, Aux Output, Level, Pan and sends ("Global Effect 2 Level"; a knob's value text starts with the effect's own name for it: "Global Effect 1 Knob 4 = Pre Del, 5 ms"). 832 in all, each with value text from the original's own display ("+12", "C3", "Legato", "1/16"), so a host's generic UI or OSARA's parameter list can read and set them. A Hyperknob's value text starts with the patch's own knob name ("Thump, 50 %").

## How it works

- **Background loading.** Patch and combi files are read on a background thread, which also loads the samples into memory. The ready patch then goes into the running engine at the start of the next audio block (about 0.1 to 2 ms), so the other parts keep playing. A complete new engine is built only for a state restore, a new content folder or a new sample rate; the audio thread only try-locks that swap, so it never stalls. Offline renders wait for pending loads, so a render never starts silent.
- **MIDI patch changes.** As in the original: bank select (controller 0 = 0, controller 32 = bank number) followed by a program change loads that patch into the part of the MIDI channel. Banks 120 and up load combis. This can be turned off in Settings.
  - Senders that put the bank number on controller 0 alone (no controller 32) work too: until a MIDI channel has received a controller 32, a controller 0 value is taken as the bank number. A bank select on its own does not load anything; the program change that follows does.
  - VST3 has no MIDI program change event. A host maps a program change on MIDI channel N to the program list of channel N's unit; the plugin has one per channel (JUCE patch), so program changes on several channels at once all arrive, each in its own part (for example a MIDI file with bank and program changes on every channel at the start).
  - A host that does not use per-channel units uses the plugin's main program list (programs 0 to 127, plus "None"), without the MIDI channel. The plugin then uses the channel of the latest bank select, or part 1 if none came yet.
- **Effect tails.** The original cuts some delay tails when the last note ends (it lets a part sleep as soon as it seems silent, and checks only the left channel). "Let effect tails ring out" (on by default) lets them play out; turn it off to hear exactly the original.
- **Host transport.** Tempo, position, time signature and play state are passed to the engine every block. Tempo-synced LFOs, delays and the arpeggiator follow the host.
- **Saved state.** The state holds the loaded combi and each part's patch (bank, number and name), the host parameter values, the selected part and the two MIDI / effect-tail settings. It never contains Steinberg data; the patches are read from the content folder again.
- **New instance.** As in the original, part 1 holds "Welcome To The Matrix" (bank 084 HS2 Preview Patches, patch 0); the other parts are empty.

## Tests

- **`tests/compare_with_hs2render.py`:** the VST3, loaded by `hs2plughost` the way a DAW loads it, must render exactly what `hs2render` renders in engine mode. It covers:
  - patches loaded by MIDI program change into part 1 and into part 2
  - combis loaded by MIDI program change
  - state save and restore of each
  - 48 kHz with 512-frame blocks
  - a Hyperknob and a part level moved by the host, also after a state round trip
- **`tests/keyboard_walk.ps1`:** opens the standalone app for about half a minute and checks its keyboard behaviour through UI Automation, which is what screen readers read:
  - where focus lands at start
  - the Tab order
  - search, Enter, arrows and type-ahead
  - the shortcuts
  - the part selector, a patch loaded into part 3, and a combi
