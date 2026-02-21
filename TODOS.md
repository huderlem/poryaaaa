This file tracks known issues and to-do items for the plugin.

- Reverb continues ringing after playback is stopped in the DAW.
- Clicking on the left-side keyboard in Reaper doesn't produce noise (most of the time?), which is super annoying
    - Only produces noise when drawing an actual note?
- Needs a GUI frontend so that the user can change settings on the fly in the DAW.
    - Dear Imgui seems fine for now?
- Full midi -> .wav output regression tests
- Architecture document
- Identify any performance issues/improvements
- Read the .wav instrument sample files instead of .bin files
- Support more flexible loading the various sound data from disk
    - Currently it's very rigid to pokeemerald's directory structure.
