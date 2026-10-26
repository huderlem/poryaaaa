# The wiki's two-line change to src/m4a.c for the HQ mixer (docs/HQ-MIXER-ACCURACY.md, section 1).
s|^BSS_CODE ALIGNED(4) char SoundMainRAM_Buffer\[0x800\] = {0};|BSS_CODE ALIGNED(4) char SoundMainRAM_Buffer[0xB40] = {0};\nBSS_CODE ALIGNED(4) u32 hq_buffer_ptr[0xE0] = {0};|
