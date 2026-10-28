# Licenses

poryaaaa is MIT licensed (see `LICENSE`), **except** for the files listed
below, which are derived from other projects and keep those projects'
licenses.  Full license texts are in `licenses/`.

## Files in this repository under a different license

| Files | License | Derived from |
|---|---|---|
| `plugin/m4a_psg.c`, `plugin/m4a_psg.h` | [MPL 2.0](licenses/MPL-2.0.txt) | [mGBA](https://mgba.io) 0.10.5, `src/gb/audio.c`, `src/gba/audio.c`, `src/gba/io.c`. Copyright (c) 2013-2016 Jeffrey Pfau. |
| `plugin/m4a_resampler.c`, `plugin/m4a_resampler.h` | [LGPL 2.1 or later](licenses/LGPL-2.1.txt) | [blip_buf](http://www.slack.net/~ant/) 1.1.0, the fast linear delta path (the copy vendored in mGBA as `src/third-party/blip_buf`). Copyright (C) 2003-2009 Shay Green. |

The MPL 2.0 is file-scoped: modifications to the MPL files are themselves
MPL, and their source must stay available to anyone who receives a binary
built from them.  This repository is that source.  The LGPL files are built
into the plugin and the command-line tools; their source is likewise this
repository, and they may be replaced and relinked by rebuilding from it.
Both licenses permit combining these files with the MIT-licensed rest of
the project ("Larger Work" in MPL terms).

The mGBA and blip_buf derivations are transcriptions, with the structural
changes noted in each file's header, made so that the engine's output nulls
against mGBA's.  The test harness under `test/accuracy` additionally links
against `libmgba-static.a` from a user-supplied mGBA tree; nothing from
that tree is vendored here.

## Vendored and submodule dependencies

| Path | Project | License |
|---|---|---|
| `third_party/pugl/` | [Pugl](https://github.com/lv2/pugl) | ISC |
| `third_party/miniaudio.h` | [miniaudio](https://miniaud.io) | Public domain or MIT-0 (at your option; statement at the end of the file) |
| `third_party/mid2agb/` | mid2agb from [pokeemerald](https://github.com/pret/pokeemerald) tools | MIT (`third_party/mid2agb/LICENSE`) |
| `imgui/` (submodule) | [Dear ImGui](https://github.com/ocornut/imgui) | MIT (`imgui/LICENSE.txt`) |
| `clap-sdk/` (submodule) | [CLAP](https://github.com/free-audio/clap) | MIT |
| `clap-wrapper/` (submodule) | [clap-wrapper](https://github.com/free-audio/clap-wrapper) | MIT |

## Reference material

The sequencer, mixers, PSG driver and voice tables are ports of the m4a
sound engine as decompiled in [pokeemerald](https://github.com/pret/pokeemerald)
and documented on its wiki.  That decompilation carries no license grant
from the original rights holders; see the "GBA source reference" section of
`README.md` for the files consulted.
