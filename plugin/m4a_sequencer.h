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

/* Emerald's music players (sound/music_player_table.inc, gMPlayTable order;
 * the `ms` field of a song_table.inc entry): the BGM player has 10 tracks,
 * SE1 3, SE2 9, SE3 1.  A song with more tracks than its player plays only
 * the first ones (MPlayStart), in the game and in the accuracy ROM.  The
 * two cry players (gPokemonCryMusicPlayers, m4aSoundInit, two tracks
 * each) are opened after SE3; SetPokemonCryTone picks the first one whose
 * track is free, cry 0 from silence (m4a_cry.h). */
enum {
    M4A_PLAYER_BGM = 0,
    M4A_PLAYER_SE1 = 1,
    M4A_PLAYER_SE2 = 2,
    M4A_PLAYER_SE3 = 3,
    M4A_PLAYER_CRY0 = 4,
    M4A_PLAYER_CRY1 = 5,
    M4A_PLAYER_COUNT
};
#define M4A_SEQ_DEFAULT_MAX_TRACKS 10

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

typedef struct M4ASequencer M4ASequencer;
struct M4ASequencer {
    M4AEngine *engine;
    const M4ASong *song;
    /* The engine track this player's track 0 is (its tracks are the range
     * [trackBase, trackBase + maxTracks)); 0 for a lone player.  A second
     * player attached with m4a_sequencer_init_chained takes a disjoint
     * range above the first's, as the GBA's MusicPlayerTrack arrays lie
     * in memory (gPokemonCryTracks[2..3] above [0..1]): ply_note's channel
     * scan and the steal tests order tracks by that index as the GBA
     * orders them by address. */
    uint8_t trackBase;
    /* The voicegroup VOICE selects from (NULL: the engine's).  The second
     * cry player's song is the template copy whose VOICE 0 is the second
     * cry's ToneData, so each player names its own. */
    const ToneData *voiceGroup;
    /* SoundMain's MPlayMain chain: each player calls the previously opened
     * one before its own body, so the bodies run in MPlayOpen order (BGM,
     * SE1, SE2, SE3, cry 0, cry 1).  `outer` is the next player whose body
     * runs after this one's, continuing from this one's _081DDA6C cycle
     * count; `inner` the one before.  NULL for a lone player: then mpEntry
     * folds the deeper players' paused costs in and mpExit the outer ones'. */
    M4ASequencer *outer;
    M4ASequencer *inner;

    uint32_t status;        /* MUSICPLAYER_STATUS_*: PAUSE when stopped/finished */
    uint32_t clock;         /* song ticks since start */
    uint8_t priority;       /* song header priority */
    uint8_t trackCount;     /* min(song tracks, maxTracks) */
    uint8_t maxTracks;      /* the player's track count (MusicPlayerInfo.trackCount);
                             * tracks beyond it are not played.  Set by
                             * m4a_sequencer_set_player before start. */
    uint8_t player;         /* M4A_PLAYER_*: which music player this is */
    /* The player's position in SoundMain's MPlayMain chain (M4A_CYC_MP_* in
     * m4a_engine.h), set with the player: cycles from VBlank line 160 to the
     * start of its body (mpEntry), its body head to _081DD9BC (mpHead), its
     * paused path to _081DDA6C (mpPaused), and _081DDA6C to CgbSound (mpExit). */
    int16_t mpEntry;
    int16_t mpHead;
    int16_t mpPaused;
    int16_t mpExit;
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
    /* Track (+1) whose chain a channel was started in; 0 once the tick's
     * chain walk has dropped it (ClearChain cost in the timing model). */
    uint8_t chainOwner[TOTAL_PCM_CHANNELS + TOTAL_CGB_CHANNELS];

    /* Statistics for hosts: GOTO loop passes (on the first track that
     * executes a GOTO) and notes started. */
    uint32_t loopPasses;
    int loopTrack;
    uint32_t noteCount;

    /* SoundMain timing model: CPU cycles of MPlayMain work this frame
     * (M4A_CYC_* in m4a_engine.h), published to engine->cgbEntryCycles. */
    int32_t frameCycles;
    uint32_t frameCount;        /* MPlayMain calls since start (the ROM's engine frame - 2) */
    /* Optional trace: one line per frame, "<frame> <CgbSound entry cycles from
     * VBlank line 160>", comparable to mgba_probe's .frames column.  NULL = off. */
    void *traceFile;            /* FILE * */
};

/* Attach to an engine: installs the frame hook and takes over the per-tick
 * work the engine otherwise does itself (gate times, tempo, LFO). */
void m4a_sequencer_init(M4ASequencer *seq, M4AEngine *engine);

/* A second player on the same engine, run in SoundMain's chain after
 * `inner` (its body after inner's, from inner's _081DDA6C), on engine
 * tracks [trackBase, trackBase + its track count).  Installs no hook:
 * inner's hook runs the chain.  Selects the next player up the chain
 * (inner's + 1: cry 1 after cry 0); its voicegroup with
 * m4a_sequencer_set_voicegroup.  Returns 0, or -1 when inner is cry
 * player 1 (nothing is opened after it), already has an outer player, or
 * trackBase overlaps inner's tracks or its player's tracks do not fit,
 * which changes nothing. */
int m4a_sequencer_init_chained(M4ASequencer *seq, M4ASequencer *inner, int trackBase);

/* The same with the player named: a chain may skip players (BGM -> cry 0
 * with the three SE players paused between them, SE1 -> cry 0), their idle
 * costs charged by the chain's frame code.  `player` must be a higher
 * M4A_PLAYER_* than inner's and its tracks must fit.  Returns 0, or -1 as
 * above (and for a player out of order or range), which changes nothing. */
int m4a_sequencer_init_chained_player(M4ASequencer *seq, M4ASequencer *inner, int trackBase, int player);

/* The 128-entry voicegroup this player's VOICE commands select from (NULL:
 * the engine's, the default). */
void m4a_sequencer_set_voicegroup(M4ASequencer *seq, const ToneData *voiceGroup);

/* SetPokemonCryTone's test of a cry player (m4a.c): its track 0 has no
 * flags and no channel of its own chained (track->chan == 0 or
 * chan->track != track).  True before anything ever played on it and
 * after its song's FINE. */
bool m4a_sequencer_player_free(const M4ASequencer *seq);

/* Detach from the engine, restoring its own tick behaviour (a chained
 * player: unlinks it from the chain). */
void m4a_sequencer_detach(M4ASequencer *seq);

/* Select the music player (M4A_PLAYER_*; the song table's `ms` field):
 * sets the track count and the MPlayMain chain position the timing model
 * charges.  Call before m4a_sequencer_start; init selects the BGM player.
 * Returns 0, or -1 for an out-of-range value, a chained player's out of
 * chain order (not above its inner's, not below its outer's) or a track
 * count past the engine's tracks, which changes nothing. */
int m4a_sequencer_set_player(M4ASequencer *seq, int player);

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
