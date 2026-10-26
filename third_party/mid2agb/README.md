# mid2agb (vendored)

pokeemerald's `tools/mid2agb` (commit 962442a51e25cb8d5c1c11460d42e0b3325059b4,
MIT, see LICENSE) as a library for `poryaaaa_render --gba-tempo-grid`, which
converts the MIDI it plays to the song bytes the ROM would play, for the
SoundMain timing model's CgbSound entry on the MIDI path
(docs/ACCURACY.md, the renderer's "as the game" mode).

Changes from upstream, each marked `poryaaaa` in the source:

- `error.cpp`: `RaiseError` throws `std::runtime_error` instead of printing
  and calling `exit(1)`.
- `midi.cpp` / `agb.cpp`: `ResetMidiState()` / `ResetAgbState()` put the
  file-scope state back to its initial values between conversions.
- `main.cpp` is replaced by `mid2agb_lib.cpp` (`mid2agb_convert`, the same
  globals and options, output to a temporary file returned as text).
