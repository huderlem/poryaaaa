# Poryaaaa

Poryaaaa is an audio synthesizer that emulates the GBA's m4a sound engine.  It's compatible with the [pokeemerald](https://github.com/pret/pokeemerald) and [pokefirered](https://github.com/pret/pokefirered) decompilation projects.

## Overview

Composing custom music for Pokémon GBA games normally requires a slow iteration loop: write notes in a DAW, export midi file, build the ROM, and test in an emulator.  Without a synth like poryaaaa, the DAW's audio will sound very different from how it sounds in-game on the GBA. This project eliminates that painful gap by emulating the m4a mixer and loading audio data (e.g. voicegroups, samples) directly from the decomp project's files.

These are the available poryaaaa tools:

- **`poryaaaa.clap`**: a CLAP instrument plugin. Insert it on a MIDI track in your DAW and hear the GBA-accurate audio in real time as you compose.
- **`poryaaaa_standalone(.exe)`**: a standalone GUI that wraps the CLAP plugin. It receives MIDI from any connected device or virtual cable and plays audio through your speakers.
- **`poryaaaa_render(.exe)`**: a standalone command-line renderer. Feed it a MIDI file and a voicegroup name; it outputs a WAV file and/or plays audio through your speakers. Supports looping with configurable repeat count and fadeout.

All tools auto-discover the project structure and work with [pokeemerald](https://github.com/pret/pokeemerald), [pokefirered](https://github.com/pret/pokefirered), and forked projects (including those with custom sound data directories).

## Usage

### `poryaaaa_render` -- Standalone command-line MIDI renderer

```
Usage: poryaaaa_render <project_root> <voicegroup> --midi <file.mid> [options]

Required:
  <project_root>              Path to pokeemerald/pokefirered project root
  <voicegroup>                Voicegroup name (e.g. petalburg)
  --midi <file.mid>           MIDI input file

Output (at least one required):
  --output <file.wav>         Write rendered audio to WAV file
  --play                      Play audio through computer speakers

Audio options:
  --song-volume <0-127>       Song master volume (default: 127)
  --reverb <0-127>            Reverb amount (default: 0)
  --analog-filter             Enable GBA analog low-pass filter (default: off)
  --polyphony <1-12>          Max simultaneous PCM channels (default: 5)
  --sample-rate <hz>          Sample rate in Hz (default: 44100)
  --tail <seconds>            Silence after last event, no loop markers (default: 3.0)

Loop options (when MIDI contains '[' / ']' text events):
  --loop-count <n>            Number of loop body repetitions (default: 2)
  --fadeout <seconds>         Fadeout duration after final loop (default: 5.0)
  --total-duration-seconds <s>  Override loop-count; set exact total duration
                                (fadeout occupies the final --fadeout seconds)
```

Examples:

```bash
# Render to WAV with reverb and analog filter
./build/poryaaaa_render /path/to/pokeemerald petalburg \
    --midi song.mid --output out.wav --reverb 40 --analog-filter

# Play through speakers
./build/poryaaaa_render /path/to/pokeemerald petalburg \
    --midi song.mid --play --song-volume 100

# Render and play simultaneously
./build/poryaaaa_render /path/to/pokeemerald petalburg \
    --midi song.mid --output out.wav --play

# Loop twice with 5-second fadeout (default when '['/']' markers found)
./build/poryaaaa_render /path/to/pokeemerald petalburg \
    --midi song.mid --output out.wav

# Custom total duration of 90 seconds with 5-second fadeout
./build/poryaaaa_render /path/to/pokeemerald petalburg \
    --midi song.mid --output out.wav --total-duration-seconds 90
```

#### Loop markers

When the MIDI file contains text events (Meta type 0x01) or marker events (Meta type 0x06) with the content `[` and `]`, `poryaaaa_render` treats those as loop boundaries:

- Everything before `[` is the intro, played once.
- The region from `[` to `]` is the loop body, repeated `--loop-count` times.
- After the final repetition, audio continues playing from `[` while fading to silence over `--fadeout` seconds.

`--total-duration-seconds` overrides `--loop-count` and sets the exact total render length. The fadeout still occupies the last `--fadeout` seconds of that duration.

### Standalone executable

`poryaaaa_standalone(.exe)` wraps the CLAP plugin as a self-contained application with its own audio output (via RtAudio) and MIDI input (via RtMidi). It presents the same ImGui settings GUI as the DAW plugin.  No DAW or config file is needed to run it.

#### Running

Simply launch the executable. The GUI window opens immediately:

```bash
# Linux
./poryaaaa_standalone

# Windows (or double click on it)
poryaaaa_standalone.exe
```

- Edit **Project Root** and **Voicegroup** and press **Reload** to load a voicegroup.
- Adjust **Song Volume** and **Reverb** live.
- Close the window to exit.

The app reads `poryaaaa.cfg` (located next to the executable) on startup as initial defaults, using the same format as the plugin. See the [Plugin config reference](#plugin-config-reference) below.

#### Sending MIDI on Windows

RtMidi uses the WinMM MIDI backend. Any device or virtual MIDI port visible in Windows will be detected on startup.

To route MIDI from software (e.g. [Sekaiju](https://openmidiproject.osdn.jp/Sekaiju_en.html), a DAW, or other sequencer):

1. Install [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html) and create a virtual port (e.g. `loopMIDI Port`).
2. In your sequencer, set the MIDI output to the loopMIDI port.
3. Launch `poryaaaa_standalone.exe` — it will automatically open all available MIDI inputs (e.g. loopMIDI ports).
4. Play notes or send program changes from your sequencer.

#### Sending MIDI on Linux

RtMidi uses the ALSA sequencer backend. Create a virtual MIDI port with any tool and connect it to `poryaaaa_standalone`'s input:

```bash
# Example using aconnect (part of alsa-utils)
aconnect <source_port> <poryaaaa_port>
```

Caveat: I haven't actually tested this on Linux.  It theoretically works...

### CLAP plugin in a DAW

1. Copy `poryaaaa.clap` to your DAW's CLAP plugin directory.
   - e.g. On Windows, `%APPDATA%\CLAP` or `C:\Program Files\Common Files\CLAP`
2. Copy `poryaaaa.cfg.example` to the **same directory** as the `.clap` file and rename it `poryaaaa.cfg`.
3. Edit the config to point at your project:
   ```ini
   project_root=C:\Users\you\pokeemerald
   voicegroup=petalburg
   ```
4. Insert the plugin as an instrument on a MIDI track and rescan plugins or relaunch your DAW so it sees the new plugin.
5. Use Program Change (PC) midi messages to select instruments from the voicegroup.
6. Play MIDI notes. You should hear GBA-accurate audio.

The plugin reads `poryaaaa.cfg` on startup for initial defaults. All settings can be changed live through the GUI and are saved in the DAW's project state.

#### Plugin config reference

| Key | Default | Description |
|-----|---------|-------------|
| `project_root` | *(required)* | Path to project root |
| `voicegroup` | *(required)* | Voicegroup name |
| `reverb` | `0` | Reverb amount (0–127) |
| `master_volume` | `15` | M4A master volume (0–15) |
| `song_master_volume` | `127` | Song-level volume multiplier (0–127) |
| `sound_data_paths` | *(auto)* | Extra `.inc` files for sample symbols (semicolon-separated, relative to project root) |
| `voicegroup_paths` | *(auto)* | Extra voicegroup search directories or files |
| `sample_dirs` | *(auto)* | Extra `.wav` sample search directories |
| `log` | *(off)* | Diagnostic log file path |

#### GUI

The plugin opens a settings panel built with [Dear ImGui](https://github.com/ocornut/imgui) and [GLFW](https://www.glfw.org/):

- **Project Root** / **Voicegroup**: edit and press **Reload** to apply
- **Song Volume** (0–127), **Reverb** (0–127): take effect immediately

On Windows the GUI is embedded inside the DAW's FX window. On Linux/macOS it opens as a floating window.

## Building

Requires CMake 3.16+, a C11/C++17 compiler, and the following development libraries on Linux:

```
libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libgl-dev
```

```bash
cmake -B build
cmake --build build
```

This produces the following targets:

| Target | Output | Description |
|--------|--------|-------------|
| `poryaaaa` | `poryaaaa.clap` | CLAP instrument plugin |
| `poryaaaa-standalone` | `poryaaaa_standalone(.exe)` | Standalone GUI |
| `poryaaaa_render` | `poryaaaa_render(.exe)` | Standalone MIDI renderer |
| `poryaaaa_test` | `poryaaaa` | Quick WAV export test (hardcoded sequence) |
| `poryaaaa_unit_tests` | `poryaaaa_unit_tests` | Engine unit test suite |

To build a single target:

```bash
cmake --build build --target poryaaaa-standalone
cmake --build build --target poryaaaa_render
```

### Cross-compiling for Windows (from WSL2/Linux)

```bash
sudo apt install gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64
cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake
cmake --build build-windows
```

Copy `build-windows/poryaaaa.clap` to your DAW's CLAP plugin directory (e.g. `%APPDATA%\CLAP` or `C:\Program Files\Common Files\CLAP`).

### Running tests

```bash
./build/poryaaaa_unit_tests
```

## Architecture

```
cmd/
  poryaaaa_render.c           Standalone MIDI renderer (CLI tool)

plugin/
  m4a_plugin.c/.h             CLAP entry point, MIDI event handling, extension dispatch
  m4a_gui.cpp/.h              Dear ImGui + GLFW settings GUI (C++ with C interface)
  m4a_engine.c/.h             Core engine: tick processing, channel allocation, MIDI routing
  m4a_channel.c/.h            PCM and CGB channel rendering, ADSR envelopes
  m4a_tables.c/.h             Frequency/scale tables (from m4a_tables.c)
  m4a_reverb.c/.h             Delay-based reverb effect
  voicegroup_loader.c/.h      Project discovery, .inc/.s parser, sample loader
  standalone_main_win32.cpp   Custom Win32 entry point for the standalone executable

test/
  test_engine.c          Unit tests for engine algorithms
  test_wav_export.c      Hardcoded-sequence WAV export test (poryaaaa_test)

third_party/
  miniaudio.h            Single-header audio I/O library (used by poryaaaa_render)

clap-sdk/                CLAP plugin SDK (submodule)
clap-wrapper/            Wraps the CLAP plugin as a standalone app (submodule)
glfw/                    GLFW 3.4 (submodule)
imgui/                   Dear ImGui (submodule)
```

### CLAP extensions

| Extension | Purpose |
|-----------|---------|
| `CLAP_EXT_AUDIO_PORTS` | Declares one stereo output port |
| `CLAP_EXT_NOTE_PORTS` | Declares one MIDI input port |
| `CLAP_EXT_STATE` | Saves/restores voicegroup path and audio settings in the DAW project |
| `CLAP_EXT_GUI` | Settings GUI (Win32 embedded on Windows, floating on Linux/macOS) |
| `CLAP_EXT_TIMER_SUPPORT` | 16 ms timer that drives GUI rendering and applies live parameter changes |

### How the engine works

The engine runs a **tick** at the GBA's VBlank rate (~59.7 Hz) to advance envelopes and LFO, while rendering audio sample-by-sample at the configured sample rate (typically 44100 or 48000 Hz).

**PCM channels** use the same 23-bit fractional sample position and linear interpolation as the GBA's `SoundMainRAM` mixer. Frequency is computed using `MidiKeyToFreq` with the exact scale/frequency table lookups, then scaled from the GBA's ~13379 Hz output rate to the target sample rate.

**CGB channels** are synthesized in software: square waves use an 8-step duty cycle pattern with a phase accumulator, programmable wave reads 4-bit nibbles from 16-byte waveforms, and noise uses a 15-bit LFSR.

**`poryaaaa_render`** pre-renders the entire song to float stereo buffers before writing or playing back. For looping songs it builds an extended event timeline by repeating the loop body events with increasing sample offsets, then applies a linear fadeout envelope in place over the final window.

### Voicegroup loading

The loader auto-discovers and parses project assembly source files at runtime:

1. **Project discovery**: scans the `sound/` directory tree to locate symbol definition files, voicegroup directories, monolithic voicegroup files, and `.wav` sample directories. Works with pokeemerald's individual-file layout (`sound/voicegroups/<name>.inc`) and pokefirered's monolithic layout (`sound/voice_groups.inc` with labeled sections).
2. **Symbol maps**: parses `direct_sound_data.inc` and `programmable_wave_data.inc` to build symbol-to-file mappings for PCM and programmable wave samples.
3. **Keysplit tables**: parses `keysplit_tables.inc`, supporting both pokeemerald's macro format (`keysplit name, startNote`) and pokefirered's raw format (`.set`/`.byte` directives).
4. **Voice definitions**: parses voice macros (`directsound`, `square`, `noise`, `keysplit`, etc.) from the discovered voicegroup file.
5. **Sample loading**: loads `.wav` samples with a deduplication cache. When a sample symbol isn't found in the symbol map, the loader falls back to searching discovered `.wav` directories.

Keysplit and drumset sub-voicegroups are resolved recursively across all discovered voicegroup directories. Additional search paths can be configured for projects with non-standard layouts.

## GBA source reference

The engine reimplements algorithms from these pokeemerald source files:

| File | Content |
|------|---------|
| `src/m4a.c` | `MidiKeyToFreq`, `TrkVolPitSet`, `CgbSound`, `CgbModVol` |
| `src/m4a_1.s` | `SoundMainRAM` (PCM mixer, reverb, envelope processing) |
| `src/m4a_tables.c` | `gScaleTable`, `gFreqTable`, CGB frequency tables |
| `include/gba/m4a_internal.h` | Struct definitions |

# AI Disclaimer

Much of this codebase was written with the aide of Claude Code, which allowed extremely rapid exploration, implementation, and iteration of the m4a engine code, along with consulting [mgba's audio emulation](https://github.com/mgba-emu/mgba/blob/master/src/gba/audio.c).
