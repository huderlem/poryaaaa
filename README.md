# Poryaaaa

Poryaaaa is an audio synthesizer that emulates the GBA's m4a sound engine.  It's compatible with the [pokeemerald](https://github.com/pret/pokeemerald) and [pokefirered](https://github.com/pret/pokefirered) decompilation projects.

## Overview

Composing custom music for Pokémon GBA games used to require a slow creative loop: write notes in a DAW, export midi file, build the ROM, and test in an emulator.  Without a synth like poryaaaa, the DAW's audio will sound very different from how it sounds in-game on the GBA. This project eliminates that painful gap by emulating the m4a engine and loading audio data (e.g. voicegroups, samples) directly from the decomp project's files.

Poryaaaa accurately emulates both the m4a engine and the GBA hardware it runs on (verified against [mgba](https://mgba.io)), so what you hear in poryaaaa is virtually identical to what you hear in-game.

These are the available poryaaaa tools:

- **`poryaaaa.clap`**: a CLAP instrument plugin. Insert it on a MIDI track in your DAW and hear the GBA-accurate audio in real time as you compose.
- **`poryaaaa_standalone(.exe)`**: a standalone GUI that wraps the CLAP plugin. It receives MIDI from any connected device or virtual cable and plays audio through your speakers.
- **`poryaaaa_render(.exe)`**: a standalone command-line renderer. Feed it a MIDI file, a compiled song (`mid2agb` output, or any song `.s` from the project) or a Pokémon cry; it outputs a WAV file and/or plays audio through your speakers. Supports looping with configurable repeat count and fadeout.

All tools auto-discover the project structure and work with [pokeemerald](https://github.com/pret/pokeemerald), [pokefirered](https://github.com/pret/pokefirered), and forked projects (including those with custom sound data directories).

## Usage

### `poryaaaa_render` -- Standalone command-line renderer

```
poryaaaa_render <project_root> <voicegroup> --midi <file.mid> [options]
poryaaaa_render <project_root> [<voicegroup>] --song <file.s> [options]
poryaaaa_render <project_root> --cry <id> [options]
```

Three kinds of input:

- **`--midi <file.mid>`**: a MIDI file, with the voicegroup named on the command line. This is what the DAW plugin plays.
- **`--song <file.s>`**: a compiled song (`mid2agb` output, or any song under the project's `sound/songs/`), played exactly as the game plays it. The voicegroup, volume and reverb come from the song file itself; a voicegroup on the command line overrides it.
- **`--cry <id>`**: a Pokémon cry by its index in the project's cry table, played as the game plays it. The `--cry-*` options set the pitch, pan, volume, length, release and chorus of the game's other cry modes.

Output goes to `--output <file.wav>`, `--play` (the speakers), or both. The options you will usually want:

| Option | Meaning |
|--------|---------|
| `--song-volume <0-127>`, `--reverb <0-127>` | Song master volume and reverb (`--midi` only; a `.s` song carries its own) |
| `--polyphony <1-15>` | Max simultaneous PCM channels (default 5, the games' setting) |
| `--pcm-mix-rate <hz>` | DirectSound mix rate, one of the m4a sound-mode rates (default 13379, the games' setting) |
| `--hq-mixer` | Mix with ipatix's HQ mixer instead of the stock one ([HQ mixer mode](#hq-mixer-mode-ipatix)) |
| `--sample-rate <hz>` | Output sample rate (default 44100) |
| `--loop-count <n>`, `--fadeout <s>`, `--total-duration-seconds <s>`, `--tail <s>` | Looping and length ([Loop markers](#loop-markers)) |
| `--respect-base-midi-key`, `--portamento`, `--pwm` | The [opt-in effect features](#plugin-config-reference), off by default |

`poryaaaa_render --help` lists the remaining options (cry parameters, sound-effect players, and debugging aids).

Examples:

```bash
# Render a MIDI to WAV with reverb
./build/poryaaaa_render /path/to/pokeemerald petalburg \
    --midi song.mid --output out.wav --reverb 40

# Play a MIDI through the speakers
./build/poryaaaa_render /path/to/pokeemerald petalburg \
    --midi song.mid --play --song-volume 100

# Render a song from the project exactly as the game plays it
./build/poryaaaa_render /path/to/pokeemerald \
    --song /path/to/pokeemerald/sound/songs/midi/mus_littleroot.s --output littleroot.wav

# Render Bulbasaur's cry
./build/poryaaaa_render /path/to/pokeemerald --cry 0 --output bulbasaur.wav

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

With `--song`, the loop count is the number of passes through the song's loop, and a song that ends with `FINE` ends the render after `--tail` seconds.

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

While the DAW's transport is playing, note, CC, program change and pitch bend events are held to the next GBA frame (one VBlank, about 16.7 ms), where the game's sound engine would start them; while the transport is stopped, and in the standalone (which has no transport), they apply at once so live playing stays responsive. This is the default, "Follow Transport"; **Options → Quantize to GBA Frames** (or `quantize_events` in the config) can set it to "Always" or "Never" instead. A host's note choke always cuts the note at once.

The plugin plays at the host's tempo; the game rounds a song's tempo to its own frame grid (about 0.5 % faster), so a `poryaaaa_render --song` render of the compiled song is the exact in-game timing.

The plugin reads `poryaaaa.cfg` on startup for initial defaults. All settings can be changed live through the GUI and are saved in the DAW's project state.

#### Plugin config reference

| Key | Default | Description |
|-----|---------|-------------|
| `project_root` | *(required)* | Path to project root |
| `voicegroup` | *(required)* | Voicegroup name |
| `reverb` | `0` | Reverb amount (0–127) |
| `master_volume` | `12` | M4A master volume (0–15; 12 matches Pokemon Emerald's `m4aSoundInit`) |
| `song_master_volume` | `127` | Song-level volume multiplier (0–127) |
| `max_channels` | `5` | Max simultaneous PCM (DirectSound) channels (1–15) |
| `pcm_mix_rate` | `13379` | DirectSound (PCM) mix rate in Hz: one of the m4a sound-mode rates (5734, 7884, 10512, 13379, 15768, 18157, 21024, 26758, 31536, 36314, 40137, 42048; the nearest is used). `13379` is what the Pokemon games set |
| `respect_base_midi_key` | `0` | Opt-in: treat a PCM voice's key as the sample's base MIDI note so pressed notes play at the intended pitch |
| `portamento` | `0` | Opt-in: enable the portamento glide effect (CC 5 = glide time in ticks) |
| `pwm` | `0` | Opt-in: enable pulse-width modulation on CGB square channels (CC 0x17 / 0x19) |
| `hq_mixer` | `0` | Mix DirectSound (PCM) with [ipatix's HQ mixer](https://github.com/ipatix/gba-hq-mixer) as the pret wiki installs it, instead of the stock mixer (see [HQ mixer mode](#hq-mixer-mode-ipatix)); `pcm_mix_rate` 7884 is not available in this mode and snaps to 13379 |
| `quantize_events` | `transport` | Hold MIDI events to the next GBA frame: `transport` (while the DAW transport plays; never in the standalone), `on` (always), `off` (never) |
| `sound_data_paths` | *(auto)* | Extra `.inc` files for sample symbols (semicolon-separated, relative to project root) |
| `voicegroup_paths` | *(auto)* | Extra voicegroup search directories or files |
| `sample_dirs` | *(auto)* | Extra `.wav` sample search directories |
| `log` | *(off)* | Diagnostic log file path |

The opt-in effect features (`respect_base_midi_key`, `portamento`, `pwm`) require the matching m4a engine extensions in your project. See [huderlem/pokeemerald @ m4a_extensions](https://github.com/huderlem/pokeemerald/tree/m4a_extensions).

#### GUI

The plugin opens a settings panel built with [Dear ImGui](https://github.com/ocornut/imgui) and [Pugl](https://github.com/lv2/pugl) (a lightweight embeddable windowing library):

- **General** tab — **Project Root** / **Voicegroup**: edit and press **Reload** to apply; **Song Volume** (0–127), **Reverb** (0–127), **Polyphony**, and **PCM Mix Rate** take effect immediately
- **Voices** tab — inspect and edit individual voices in the loaded voicegroup
- **Polyphony** tab — realtime monitor for debugging polyphony overflow
- **Options** menu — toggle the opt-in effect features (Respect Base MIDI Key, Portamento, Pulse-Width Modulation), **HQ Mixer (ipatix)**, and choose **Quantize to GBA Frames** (Follow Transport / Always / Never); hover an item for help text. Changes take effect immediately (the quantize choice at the next audio block) and are saved per project.

On Windows the GUI is embedded inside the DAW's FX window. On Linux/macOS it opens as a floating window.

## Building

Requires CMake 3.16+, a C11/C++17 compiler, and the following development libraries on Linux:

```bash
sudo apt-get install build-essential cmake pkg-config
sudo apt-get install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libgl-dev libasound-dev
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
| `poryaaaa_render` | `poryaaaa_render(.exe)` | Standalone renderer |
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

## Accuracy

Poryaaaa's engine is a port of pokeemerald's m4a sound driver (the sequencer, the PCM mixer and the PSG driver), running on a model of the GBA's audio hardware taken from mgba. Its output is null-tested against mgba playing the same songs, sound effects and cries on a test ROM: most of the test set is bit-identical, and the rest differs only at the level of mgba's own scheduling jitter (around −50 dB, inaudible). Details, measurements and known limitations are in [docs/ACCURACY.md](docs/ACCURACY.md) and [docs/HQ-MIXER-ACCURACY.md](docs/HQ-MIXER-ACCURACY.md); [test/accuracy/README.md](test/accuracy/README.md) explains how to run the harness.

## Golden Sun synth instruments

Poryaaaa supports the synthesized instruments from Golden Sun's custom mixer, as featured in [ipatix's improved ("HQ") mixer](https://github.com/ipatix/gba-hq-mixer): pulse wave (with duty-cycle modulation), pseudo-sawtooth, and triangle. A DirectSound voice whose sample has a length of zero is played as one of these synth tones instead of PCM data. Support is always enabled — projects without synth samples are unaffected.

Both ways of defining synth voices are recognized:

- **Binary samples**: a `.bin` sample with size 0 whose data bytes select the waveform and pulse parameters, referenced from `direct_sound_data.inc` like any other sample.
- **Assembly macros**: inline `set_synth_pulse p1, p2, p3, p4`, `set_synth_saw`, and `set_synth_triangle` definitions, auto-discovered from `sound/direct_sound_synth_data.inc`. The alias names `set_synth_custom`, `set_synth_25`, and `set_synth_50` are also accepted.

In the stock mixer the synth tones are an extension (the stock engine would play a zero-size sample as a click); in HQ mixer mode they are the HQ mixer's own synths.

## HQ mixer mode (ipatix)

Many hacks install [ipatix's High-Quality Audio Mixer](https://github.com/ipatix/gba-hq-mixer) following the [pret wiki](https://github.com/pret/pokeemerald/wiki/Implementing-ipatix's-High-Quality-Audio-Mixer): a replacement for the game's `SoundMainRAM` that mixes the DirectSound (PCM) channels into a 16-bit buffer and quantizes once, with noise shaping and saturation, so chords no longer wrap or pick up 8-bit rounding noise per channel. Poryaaaa reproduces that mixer (HQ-Mixer rev 4.0 as the wiki installs it, with reverb and the DMA copy enabled) as an opt-in mode, off by default: **Options → HQ Mixer (ipatix)** in the plugin, `hq_mixer=1` in `poryaaaa.cfg`, or `--hq-mixer` with `poryaaaa_render`. The choice is saved in the DAW project state.

Only the PCM mix changes with the mode; the sequencer, PSG channels and timing are the same. As on the GBA, the HQ mixer has no 7884 Hz mix rate, a looped compressed sample does not loop, and samples that need more than the mixer's stack budget in a frame are skipped (`--hq-stack-sp` sets the budget for a ROM other than the test ROM). Both modes are verified against mgba; see [docs/HQ-MIXER-ACCURACY.md](docs/HQ-MIXER-ACCURACY.md).

# License

MIT, except `plugin/m4a_psg.*` (MPL 2.0, derived from mGBA) and
`plugin/m4a_resampler.*` (LGPL 2.1, derived from blip_buf).  See
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for the full list,
including the vendored dependencies.

# AI Disclaimer

Much of this codebase was written with the aide of Claude Code, which allowed extremely rapid exploration, implementation, and iteration of the m4a engine code, along with consulting [mgba's audio emulation](https://github.com/mgba-emu/mgba/blob/master/src/gba/audio.c).
