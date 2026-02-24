This file tracks known issues and to-do items for the plugin.

- GUI implemented via Dear ImGui + GLFW (floating window). Known gaps:
    - High-DPI / scale-aware window sizing not yet implemented.
    - File browser dialog for Project Root not yet implemented.
- Full midi -> .wav output regression tests
- Architecture document
- Identify any performance issues/improvements
- Rustboro City's track #3 (programmable wave 01) sounds different than in-game. This appears to be due to mgba's hardware emulation causing the resulting wave to be quite different from the raw pcm values (this plugin outputs the raw pcm values and doesn't do any hardware emulation). I think the GBA has kind of a built-in low pass filter, for example, but not really sure.
    - We'll have to take a look at mgba's source to see what kind of hardware emulation we might want to do.
