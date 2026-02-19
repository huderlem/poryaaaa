# m4a_plugin

A CLAP instrument plugin that emulates the GBA m4a sound engine, allowing composers to hear GBA-accurate audio directly in their DAW when writing music for pokeemerald.

## Overview

Composing custom music for Pokemon Emerald normally requires a slow iteration loop: author MIDI in a DAW, export, build the ROM, and test in an emulator. The DAW plays back using standard MIDI voices that sound nothing like the GBA. This plugin eliminates that gap by reimplementing the m4a mixer natively and loading voicegroups and samples directly from the pokeemerald project tree.

The plugin receives MIDI events from the DAW (note on/off, program change, CC, pitch bend) and processes them through a faithful reimplementation of the GBA's m4a sound engine, outputting stereo audio in real time.

## Features

- **12 PCM channels** with linear-interpolating mixer matching `SoundMainRAM`
- **4 CGB channels** (2 square wave, 1 programmable wave, 1 noise) with software synthesis
- **ADSR envelopes** matching the GBA's per-tick (~60 Hz) envelope processing
- **Voicegroup loader** that parses pokeemerald's `.inc` voice definitions and loads `.bin` samples at runtime
- **Keysplit and drumset support** with correct GBA offset handling
- **LFO** (vibrato, tremolo, autopan) matching `MPlayMain`
- **Reverb** (delay-based feedback matching GBA's 4-tap algorithm)
- **Pseudo-echo** matching GBA behavior
- **Frequency calculation** using the exact `gScaleTable`/`gFreqTable` lookups from `m4a_tables.c`

## Building

Requires CMake 3.16+ and a C11 compiler.

```bash
cmake -B build
cmake --build build
```

This produces three targets:

| Target | Description |
|---|---|
| `m4a_plugin.clap` | CLAP instrument plugin for DAWs |
| `m4a_test` | Standalone WAV export tool |
| `m4a_unit_tests` | Unit test suite |

### Cross-compiling for Windows (from WSL2)

Install the cross-compiler if not already present:

```bash
sudo apt install gcc-mingw-w64-x86-64
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
4. Insert the plugin as an instrument on a MIDI track in your DAW
5. Use Program Change messages to select instruments from the voicegroup
6. Play MIDI notes to hear GBA-accurate audio

The plugin reads `m4a_plugin.cfg` on startup (when the plugin is loaded). After editing the config file, remove and re-insert the plugin in your DAW (or save and reopen the project) for changes to take effect.

The per-project voicegroup is also saved in the DAW's project state, so once you configure it, it persists in your saved project even without the config file.

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
  m4a_plugin.c/.h       CLAP entry point, MIDI event handling
  m4a_engine.c/.h        Core engine: tick processing, channel allocation, MIDI routing
  m4a_channel.c/.h       PCM and CGB channel rendering, ADSR envelopes
  m4a_tables.c/.h        Frequency/scale tables (from m4a_tables.c)
  m4a_reverb.c/.h        Delay-based reverb effect
  voicegroup_loader.c/.h .inc file parser, sample loader

test/
  test_engine.c          Unit tests for engine algorithms
  test_wav_export.c      Standalone WAV export test program
```

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
5. `.bin` sample files are loaded with their 16-byte headers (type, status, freq, loopStart, size)

Keysplit and drumset voicegroups are loaded recursively from `sound/voicegroups/keysplits/` and `sound/voicegroups/drumsets/`.

## GBA source reference

The engine reimplements algorithms from these pokeemerald source files:

- `src/m4a.c` - `MidiKeyToFreq`, `TrkVolPitSet`, `CgbSound`, `CgbModVol`
- `src/m4a_1.s` - `SoundMainRAM` (PCM mixer, reverb, envelope processing)
- `src/m4a_tables.c` - `gScaleTable`, `gFreqTable`, CGB frequency tables
- `include/gba/m4a_internal.h` - struct definitions
