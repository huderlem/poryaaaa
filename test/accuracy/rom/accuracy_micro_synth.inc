@ Golden Sun synth descriptors for the accuracy micro-tests (the HQ mixer's
@ synth paths: docs/HQ-MIXER-ACCURACY.md, step 9).  A synth is a zero-size
@ DirectSound sample whose data bytes select ipatix's oscillators: data[1]
@ the waveform (0 pulse, 1 pseudo-saw, 2 triangle), data[2..5] the pulse's
@ base duty, LFO step per frame, modulation amount and LFO offset.
@
@ Assembled into both test ROMs (sound_data.s) and read by poryaaaa's
@ loader as the overlay project's sound/direct_sound_synth_data.inc (the
@ pokeemerald-expansion layout: a label followed by one set_synth_* macro
@ line, which the loader parses itself).  The macros below assemble the
@ loader's in-memory WaveData byte for byte: the 16-byte header (type 0,
@ status 0x4000, frequency 0x01058920 so the 64-sample period lands on
@ middle C, loop start 0, size 0), then 0x80, the waveform and the four
@ pulse parameters, padded to a word.
@
@ Only the vanilla mixer's extension and the HQ mixer play these; the
@ vanilla ROM's SoundMainRAM plays a zero-size sample as a click, so the
@ harness skips the synth_* rows unless --mixer hq.

	.macro set_synth_pulse base_duty:req, lfo_step:req, mod_amount:req, lfo_offset:req
	.byte 0, 0
	.2byte 0x4000
	.4byte 0x01058920
	.4byte 0
	.4byte 0
	.byte 0x80, 0, \base_duty, \lfo_step, \mod_amount, \lfo_offset
	.align 2
	.endm

	.macro set_synth_saw
	.byte 0, 0
	.2byte 0x4000
	.4byte 0x01058920
	.4byte 0
	.4byte 0
	.byte 0x80, 1, 0, 0, 0, 0
	.align 2
	.endm

	.macro set_synth_triangle
	.byte 0, 0
	.2byte 0x4000
	.4byte 0x01058920
	.4byte 0
	.4byte 0
	.byte 0x80, 2, 0, 0, 0, 0
	.align 2
	.endm

	.align 2
DirectSoundWaveData_acc_synth_pulse::
	set_synth_pulse 0x80, 0, 0, 0

	.align 2
DirectSoundWaveData_acc_synth_pulse_lfo::
	set_synth_pulse 0x20, 0x10, 0x80, 0x40

	.align 2
DirectSoundWaveData_acc_synth_saw::
	set_synth_saw

	.align 2
DirectSoundWaveData_acc_synth_tri::
	set_synth_triangle
