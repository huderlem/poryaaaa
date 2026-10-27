@ Vanilla Emerald sound data plus the accuracy micro-test voicegroup and songs.
@ The Makefile substitutes @ROMDIR@/@BUILD@ and runs this through pokeemerald's
@ preproc + cpp + as pipeline from the pokeemerald root.
	.section .rodata

	.include "asm/macros/m4a.inc"
	.include "asm/macros/music_voice.inc"

	.include "sound/voice_groups.inc"
	.include "sound/keysplit_tables.inc"
	.include "sound/programmable_wave_data.inc"
	.include "sound/music_player_table.inc"
	.include "sound/song_table.inc"
	.include "sound/direct_sound_data.inc"

	.section .rodata
	.include "@ROMDIR@/accuracy_micro_synth.inc"
	.include "@ROMDIR@/accuracy_micro.inc"

	.section .rodata
	.align 2
	.global gAccMicroSongTable
gAccMicroSongTable:
	.include "@BUILD@/micro_song_table.inc"
gAccMicroSongTable_End:

	.align 2
	.global gAccMicroSongCount
gAccMicroSongCount:
	.4byte (gAccMicroSongTable_End - gAccMicroSongTable) / 4

	.align 2
