@ Voicegroup for the accuracy micro-tests: one voice per voice type.
@ Shared by the test ROM (assembled in) and poryaaaa (loaded through the
@ overlay project the harness builds, where this file is sound/voicegroups/accuracy_micro.inc).
voice_group accuracy_micro
	voice_square_1 60, 0, 0, 2, 0, 0, 15, 0                                       @ 0  sq1 50% duty, no envelope
	voice_square_1 60, 0, 0, 2, 0, 6, 8, 3                                        @ 1  sq1 with envelope
	voice_square_1 60, 0, 0x3A, 2, 0, 0, 15, 0                                    @ 2  sq1 with sweep (time 3, down, shift 2)
	voice_square_1_alt 60, 0, 0, 2, 0, 0, 15, 0                                   @ 3  sq1 fixed-frequency variant
	voice_square_2 60, 0, 1, 0, 0, 15, 0                                          @ 4  sq2 25% duty
	voice_square_2_alt 60, 0, 1, 0, 0, 15, 0                                      @ 5  sq2 fixed-frequency variant
	voice_programmable_wave 60, 0, ProgrammableWaveData_1, 0, 7, 15, 0            @ 6  wave
	voice_programmable_wave_alt 60, 0, ProgrammableWaveData_1, 0, 7, 15, 0        @ 7  wave fixed-frequency variant
	voice_noise 60, 0, 0, 0, 7, 15, 0                                             @ 8  noise, 15-bit
	voice_noise 60, 0, 1, 0, 7, 15, 0                                             @ 9  noise, 7-bit
	voice_directsound 60, 0, DirectSoundWaveData_sc88pro_flute, 255, 0, 255, 165  @ 10 looped PCM
	voice_directsound 60, 0, DirectSoundWaveData_sc88pro_pizzicato_strings, 255, 226, 0, 99 @ 11 one-shot PCM with decay
	voice_directsound_no_resample 64, 0, DirectSoundWaveData_sd90_solo_snare, 255, 0, 255, 242 @ 12 fixed-frequency drum
	voice_directsound_compressed 60, 0, Cry_Bulbasaur, 255, 0, 255, 0             @ 13 compressed sample
	voice_directsound 60, 0, DirectSoundWaveData_sc88pro_fretless_bass, 255, 253, 0, 188 @ 14 bass
	voice_directsound_no_resample 60, 0, DirectSoundWaveData_sc88pro_flute, 255, 0, 255, 165 @ 15 looped PCM, fixed-frequency (the HQ mixer's FIX loop)
	voice_directsound_reverse 60, 0, DirectSoundWaveData_sc88pro_pizzicato_strings, 255, 0, 255, 0 @ 16 one-shot PCM played backwards (uncompressed REV; no vanilla music uses one)
	voice_directsound 60, 0, DirectSoundWaveData_acc_synth_pulse, 255, 0, 255, 165     @ 17 synth: pulse, 50% duty, no LFO (accuracy_micro_synth.inc; HQ-only rows)
	voice_directsound 60, 0, DirectSoundWaveData_acc_synth_pulse_lfo, 255, 0, 255, 165 @ 18 synth: pulse with the duty LFO (base 0x20, step 0x10, mod 0x80, offset 0x40)
	voice_directsound 60, 0, DirectSoundWaveData_acc_synth_saw, 255, 0, 255, 165       @ 19 synth: pseudo-saw
	voice_directsound 60, 0, DirectSoundWaveData_acc_synth_tri, 255, 0, 255, 165       @ 20 synth: triangle
