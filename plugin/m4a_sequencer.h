#ifndef M4A_SEQUENCER_H
#define M4A_SEQUENCER_H

/*
 * Song sequencer: a port of pokeemerald's MPlayMain (src/m4a_1.s) and the
 * ply_* command handlers (m4a_1.s, m4a.c), playing compiled song data parsed
 * by m4a_song.c through the engine's note / parameter entry points.
 *
 * It runs once per VBlank frame (59.7275 Hz) from the engine's frame hook,
 * before the engine's envelope tick, exactly where MPlayMain sits inside
 * SoundMain on the GBA.  Tempo is the per-frame accumulator ported verbatim:
 * tempoC += tempoI each frame, one song tick per 150 accumulated, with
 * tempoI = (TEMPO byte * 2) * tempoU / 256 and tempoU = 0x100.
 *
 * Track parameter changes (VOL, PAN, BEND, ...) are written to the engine's
 * track and flagged, then applied to the track's sounding channels at the end
 * of the frame, as MPlayMain does -- including the quirk that a note started
 * in the same frame clears those flags, so older notes on the track keep the
 * previous volume until the next change.
 *
 * Milestone 1 of docs/ACCURACY_PLAN.md: CLI and harness only.
 */

#include "m4a_engine.h"
#include "m4a_song.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MusicPlayerTrack.flags */
#define MPT_FLG_VOLSET 0x01
#define MPT_FLG_VOLCHG 0x03
#define MPT_FLG_PITSET 0x04
#define MPT_FLG_PITCHG 0x0C
#define MPT_FLG_START  0x40
#define MPT_FLG_EXIST  0x80

/* MusicPlayerInfo.status */
#define MUSICPLAYER_STATUS_TRACK 0x0000ffff
#define MUSICPLAYER_STATUS_PAUSE 0x80000000

typedef struct {
    uint8_t flags;
    uint8_t wait;
    uint8_t patternLevel;
    uint8_t repN;
    uint8_t gateTime;
    uint8_t key;
    uint8_t velocity;
    uint8_t runningStatus;
    uint8_t priority;       /* PRIO; the engine track gets player + track priority */
    uint16_t timer;         /* xwait */
    uint32_t cmdPtr;        /* offset into song data */
    uint32_t patternStack[3];
} M4ASeqTrack;

typedef struct {
    M4AEngine *engine;
    const M4ASong *song;

    uint32_t status;        /* MUSICPLAYER_STATUS_*: PAUSE when stopped/finished */
    uint32_t clock;         /* song ticks since start */
    uint8_t priority;       /* song header priority */
    uint8_t trackCount;
    uint16_t tempoD;
    uint16_t tempoU;
    uint16_t tempoI;
    uint16_t tempoC;
    uint8_t memAccArea[16];
    M4ASeqTrack tracks[M4A_SONG_MAX_TRACKS];

    /* Note start order per engine channel (ply_endtie stops the newest
     * matching channel: the GBA's chain is newest-first). */
    uint32_t chanSeq[TOTAL_PCM_CHANNELS + TOTAL_CGB_CHANNELS];
    uint32_t seqCounter;

    /* Statistics for hosts: GOTO loop passes (on the first track that
     * executes a GOTO) and notes started. */
    uint32_t loopPasses;
    int loopTrack;
    uint32_t noteCount;
} M4ASequencer;

/* Attach to an engine: installs the frame hook and takes over the per-tick
 * work the engine otherwise does itself (gate times, tempo, LFO). */
void m4a_sequencer_init(M4ASequencer *seq, M4AEngine *engine);

/* Detach from the engine, restoring its own tick behaviour. */
void m4a_sequencer_detach(M4ASequencer *seq);

/* MPlayStart: begin playing `song` (which must outlive playback).  Applies the
 * header's reverb when its SOUND_MODE_REVERB_SET bit is set.  The engine's
 * voicegroup must already be set to the song's voicegroup. */
void m4a_sequencer_start(M4ASequencer *seq, const M4ASong *song);

/* m4aMPlayStop: stop every track's channels and pause. */
void m4a_sequencer_stop(M4ASequencer *seq);

/* MPlayMain: one VBlank frame.  Called from the engine's frame hook; hosts
 * that drive frames themselves may call it directly instead. */
void m4a_sequencer_frame(M4ASequencer *seq);

/* True once every track has hit FINE (or after stop). */
static inline bool m4a_sequencer_finished(const M4ASequencer *seq)
{
    return (seq->status & MUSICPLAYER_STATUS_PAUSE) != 0;
}

#ifdef __cplusplus
}
#endif

#endif /* M4A_SEQUENCER_H */
