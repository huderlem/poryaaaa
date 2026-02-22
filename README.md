# m4a_plugin

A CLAP instrument plugin that emulates the GBA m4a sound engine, allowing composers to hear GBA-accurate audio directly in their DAW when writing music for pokeemerald.

## Overview

Composing custom music for Pokemon Emerald normally requires a slow iteration loop: author MIDI in a DAW, export, build the ROM, and test in an emulator. The DAW plays back using standard MIDI voices that sound nothing like the GBA. This plugin eliminates that gap by reimplementing the m4a mixer natively and loading voicegroups and samples directly from the pokeemerald project tree.

The plugin receives MIDI events from the DAW (note on/off, program change, CC, pitch bend,) and processes them through a reasonably faithful reimplementation of the GBA's m4a sound engine, outputting stereo audio in real time.

## Features

- **12 PCM channels** with linear-interpolating mixer matching `SoundMainRAM`
- **4 CGB channels** (2 square wave, 1 programmable wave, 1 noise) with software synthesis
- **ADSR envelopes** matching the GBA's per-tick (~60 Hz) envelope processing
- **Voicegroup loader** that parses pokeemerald's `.inc` voice definitions and loads `.wav` samples at runtime
- **Keysplit and drumset support** with correct GBA offset handling
- **LFO** (vibrato, tremolo, autopan) matching `MPlayMain`
- **Reverb** (delay-based feedback matching GBA's 4-tap algorithm)
- **Pseudo-echo** matching GBA behavior
- **Frequency calculation** using the exact `gScaleTable`/`gFreqTable` lookups from `m4a_tables.c`

## Building

Requires CMake 3.16+, a C11/C++17 compiler, and the following development libraries:

- **Linux:** `libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libgl-dev`

```bash
cmake -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build
```

This produces three targets:

| Target | Description |
|---|---|
| `m4a_plugin.clap` | CLAP instrument plugin for DAWs |
| `m4a_test` | Standalone WAV export tool |
| `m4a_unit_tests` | Unit test suite |

### Cross-compiling for Windows (from WSL2/Linux)

Install the cross-compilers if not already present:

```bash
sudo apt install gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64
```

Then build using the provided toolchain file:

```bash
cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake
cmake --build build-windows
```

Copy `build-windows/m4a_plugin.clap` to your DAW's CLAP plugin directory (e.g. `%APPDATA%\CLAP` or `C:\Program Files\Common Files\CLAP`).

## Usage

### Standalone WAV export

```bash
./build/m4a_test <project_root> <voicegroup_name> [output.wav]
```

Example:

```bash
./build/m4a_test /path/to/pokeemerald petalburg output.wav
```

This loads the specified voicegroup, plays a test sequence through several programs, and writes the output to a WAV file (16-bit stereo, 44100 Hz).

### CLAP plugin in a DAW

1. Copy `m4a_plugin.clap` to your DAW's CLAP plugin directory (e.g. `%APPDATA%\CLAP`)
2. Copy `m4a_plugin.cfg.example` to the **same directory** as the `.clap` file and rename it to `m4a_plugin.cfg`
3. Edit `m4a_plugin.cfg` to set your pokeemerald project root and voicegroup name:
   ```
   project_root=C:\Users\you\pokeemerald
   voicegroup=petalburg
   ```
4. Insert the plugin as an instrument on a MIDI track in your DAW and rescan if needed
5. Open the plugin's GUI to adjust settings in real time (see below)
6. Use Program Change messages to select instruments from the voicegroup
7. Play MIDI notes to hear GBA-accurate audio

The plugin reads `m4a_plugin.cfg` on startup as a source of initial defaults. All settings can also be changed live through the GUI and are saved in the DAW's project state, so they persist across sessions.

### GUI

The plugin provides a settings panel built with [Dear ImGui](https://github.com/ocornut/imgui) and [GLFW](https://www.glfw.org/):

- **Project Root** — path to your pokeemerald repository
- **Voicegroup** — name of the voicegroup to load (e.g. `petalburg`). Press **Reload** to apply path changes and reload the instrument data.
- **Master Volume** (0–15) — m4a engine master volume, applied immediately
- **Song Volume** (0–127) — song-level volume multiplier, applied immediately
- **Reverb** (0–127) — reverb wet level, applied immediately

On Windows the GUI is embedded inside the DAW's FX window. On Linux/macOS it opens as a floating window.

### MIDI mapping

| MIDI Event | Engine Action |
|---|---|
| Note On | Allocate channel, start note with ADSR attack |
| Note Off | Transition channel to ADSR release |
| Program Change | Select voice from voicegroup |
| CC 1 (Mod Wheel) | LFO modulation depth |
| CC 7 (Volume) | Track volume (0-127) |
| CC 10 (Pan) | Track pan (0=left, 64=center, 127=right) |
| Pitch Bend | Pitch bend (scaled to bendRange semitones) |

### Running tests

```bash
./build/m4a_unit_tests
```

## Architecture

```
plugin/
  m4a_plugin.c/.h        CLAP entry point, MIDI event handling, extension dispatch
  m4a_gui.cpp/.h         Dear ImGui + GLFW settings GUI (C++ with C interface)
  m4a_engine.c/.h        Core engine: tick processing, channel allocation, MIDI routing
  m4a_channel.c/.h       PCM and CGB channel rendering, ADSR envelopes
  m4a_tables.c/.h        Frequency/scale tables (from m4a_tables.c)
  m4a_reverb.c/.h        Delay-based reverb effect
  voicegroup_loader.c/.h .inc file parser, sample loader

imgui/                   Dear ImGui v1.92.6 (submodule)
glfw/                    GLFW 3.4 (submodule)
clap-sdk/                CLAP plugin SDK (submodule)

test/
  test_engine.c          Unit tests for engine algorithms
  test_wav_export.c      Standalone WAV export test program
```

### CLAP extensions

| Extension | Purpose |
|---|---|
| `CLAP_EXT_AUDIO_PORTS` | Declares one stereo output port |
| `CLAP_EXT_NOTE_PORTS` | Declares one MIDI input port |
| `CLAP_EXT_STATE` | Saves/restores voicegroup path and audio settings in the DAW project |
| `CLAP_EXT_GUI` | Settings GUI (Win32 embedded on Windows, floating on Linux/macOS) |
| `CLAP_EXT_TIMER_SUPPORT` | 16 ms timer that drives GUI rendering and applies live parameter changes |

### How the engine works

The engine runs a **tick** at the GBA's VBlank rate (~59.7 Hz) to advance envelopes and LFO, while rendering audio sample-by-sample at the DAW's sample rate (typically 44100 or 48000 Hz).

**PCM channels** use the same 23-bit fractional sample position and linear interpolation as the GBA's `SoundMainRAM` mixer. Frequency is computed using `MidiKeyToFreq` with the exact scale/frequency table lookups, then scaled from the GBA's ~13379 Hz output rate to the DAW sample rate.

**CGB channels** are synthesized in software: square waves use an 8-step duty cycle pattern with a phase accumulator, programmable wave reads 4-bit nibbles from 16-byte waveforms, and noise uses a 15-bit LFSR.

### Voicegroup loading

The voicegroup loader parses pokeemerald's assembly source files at runtime:

1. `sound/direct_sound_data.inc` - builds a symbol-to-file mapping for PCM samples
2. `sound/programmable_wave_data.inc` - same for programmable wave samples
3. `sound/keysplit_tables.inc` - builds note-to-voice-index lookup tables
4. `sound/voicegroups/<name>.inc` - parses voice definitions (directsound, square, noise, keysplit, etc.)
5. `.wav` sample files are loaded and properly processed into their m4a ".bin" representation (e.g. how the `wav2agb` tool handles them).

Keysplit and drumset voicegroups are loaded recursively from `sound/voicegroups/keysplits/` and `sound/voicegroups/drumsets/`.

## GBA source reference

The engine reimplements algorithms from these pokeemerald source files:

- `src/m4a.c` - `MidiKeyToFreq`, `TrkVolPitSet`, `CgbSound`, `CgbModVol`
- `src/m4a_1.s` - `SoundMainRAM` (PCM mixer, reverb, envelope processing)
- `src/m4a_tables.c` - `gScaleTable`, `gFreqTable`, CGB frequency tables
- `include/gba/m4a_internal.h` - struct definitions
