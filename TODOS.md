This file tracks known issues and to-do items for the plugin.

- Reverb continues ringing after playback is stopped in the DAW.
- Clicking on the left-side keyboard in Reaper doesn't produce noise (most of the time?), which is super annoying
    - Only produces noise when drawing an actual note?
- GUI implemented via Dear ImGui + GLFW (floating window). Known gaps:
    - High-DPI / scale-aware window sizing not yet implemented.
    - File browser dialog for Project Root not yet implemented.
- Full midi -> .wav output regression tests
- Properly simulate PCM channel limits.
- Architecture document
- Identify any performance issues/improvements
- Read the .wav instrument sample files instead of .bin files
- Support more flexible loading the various sound data from disk
    - Currently it's very rigid to pokeemerald's directory structure.
