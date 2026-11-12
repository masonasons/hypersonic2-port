# Hypersonic 2 port

A reverse-engineered port of Steinberg / Wizoo Hypersonic 2 (2005, 32-bit Windows VST) to a JUCE plugin for
Windows and Mac. Personal use. The plugin is built to be fully usable with a screen reader.

Nothing from Steinberg is in this repository: the plugin reads an existing Hypersonic 2 content folder
(`Patches`, `Samples`, `Wavetables`, ...) at run time.

- `core/`: the engine (patches, voices, all element types, the 52 effects, arpeggiator, 16-part engine, combis).
  Checked sample for sample against the original DLL running in an emulator (`tools/emu/`).
- `plugin/`: the JUCE plugin (VST3, AU, Standalone). Building, keyboard use and tests: `plugin/README.md`.
- `schwung/`: the engine as a Schwung module for Ableton Move (four parts, screen reader). See `schwung/README.md`.
- `hs2-midi-player/`: HS2 MIDI Player, a Schwung tool that plays MIDI files and USB MIDI on all 16 parts.
  See `hs2-midi-player/README.md`.
- `tools/cli/`: `hs2render` (offline renders with the engine) and test tools.
- `tools/emu/`, `tools/formats/`: the emulator oracles and file format readers (Python; need the original DLL).
- `docs/NOTES.md`: everything found so far, open items included.

## Quick start (Mac)

Needs the Xcode command line tools, CMake 3.22 or later and git. JUCE 9.0.5 is downloaded during configure.

```
cmake -S . -B build_plugin -G Xcode -DHS2_BUILD_PLUGIN=ON
cmake --build build_plugin --config Release --target HS2Port_VST3 HS2Port_AU HS2Port_Standalone
```

Copy the Hypersonic 2 content folder to `~/Hypersonic 2 Content`, or choose its location in the plugin's Settings.

## Quick start (Windows)

From a Visual Studio 2022 "x64 Native Tools" prompt:

```
cmake -S . -B build_plugin -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl -DHS2_BUILD_PLUGIN=ON
cmake --build build_plugin --target HS2Port_VST3 HS2Port_Standalone
```
