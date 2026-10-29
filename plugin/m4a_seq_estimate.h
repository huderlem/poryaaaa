/*
 * The MIDI-path sequencer estimate (docs/ACCURACY.md, "MIDI-path sequencer
 * estimate"): the engine's hooks.  Internal to m4a_engine.c; a host uses the
 * m4a_engine_cost_* functions in m4a_engine.h.
 */
#ifndef M4A_SEQ_ESTIMATE_H
#define M4A_SEQ_ESTIMATE_H

#include "m4a_engine.h"

/* Whether the engine runs the estimate: no M4ASequencer drives it. */
static inline bool m4a_est_active(const M4AEngine *e) { return !e->externalSequencer; }

void m4a_est_init(M4AEngine *e);
/* A new voicegroup (a new song): the paused player, no song state. */
void m4a_est_reset(M4AEngine *e);
/* m4a_engine_all_sound_off: the channels are gone.  Inferred (no declared
 * song) the estimate resets to the paused player; a declared song keeps its
 * tracks until m4a_engine_cost_song_stop. */
void m4a_est_sound_off(M4AEngine *e);
/* ply_note's cost for a note-on about to be applied (after TrkVolPitSet,
 * before the channel is taken); voice is the resolved voice or NULL. */
void m4a_est_note_on(M4AEngine *e, int track, uint8_t key, uint8_t velocity, const ToneData *voice);
/* Before the engine releases the track's channels holding key. */
void m4a_est_note_off(M4AEngine *e, int track, uint8_t key);
/* Before the engine applies the controller (the handler reads the old state). */
void m4a_est_cc(M4AEngine *e, int track, uint8_t cc, uint8_t value);
void m4a_est_program(M4AEngine *e, int track);
void m4a_est_bend(M4AEngine *e, int track);
void m4a_est_xcmd(M4AEngine *e, int track, uint8_t command);
void m4a_est_track_end(M4AEngine *e, int track);
void m4a_est_tempo(M4AEngine *e);
void m4a_est_tick_boundary(M4AEngine *e);
/* The tracks whose wait / LFO path an LFO step charges (0 when none). */
uint16_t m4a_est_lfo_mask(const M4AEngine *e);
void m4a_est_charge(M4AEngine *e, int cycles);
/* The frame's tick passes (nTicks tempo ticks) and the VOLCHG / PITCHG pass,
 * before the engine's own pass consumes the flags. */
void m4a_est_frame(M4AEngine *e, int nTicks);
/* The estimate for the SoundMain at `stamp` (cycles from the m4aSoundMain
 * entry to the CgbSound entry, FIFO stall included); resets the frame. */
int32_t m4a_est_publish(M4AEngine *e, int64_t stamp);

#endif
