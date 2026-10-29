/*
 * The MIDI-path sequencer estimate (docs/ACCURACY.md, "MIDI-path sequencer
 * estimate").
 *
 * Without an M4ASequencer the engine never runs MPlayMain: the host's MIDI
 * events stand in for the song's commands.  This file charges, frame by
 * frame, what MPlayMain would have cost for them on the GBA, with the
 * measured M4A_CYC_* segments m4a_sequencer.c sums along MPlayMain's control
 * flow -- the same constants, the same branch rules, decided from what the
 * engine knows instead of from the song data:
 *
 *  - the frame skeleton (the player's entry, head and exit, or the paused
 *    player), the tick passes (one per tempo tick of the engine's own
 *    tempoC / 150 accumulator) over the player's trackCount slots and the
 *    VOLCHG / PITCHG pass, from the tracks that exist (declared by
 *    m4a_engine_cost_song_start, or a track's first note);
 *  - each track's chain walk from the engine's channels (dead, released,
 *    the tick its N## gate expires, a TIE's gate 0, else a running gate),
 *    the START init, the wait / LFO path from the track's lfoSpeed / mod /
 *    lfoDelayC (charged in the engine's LFO step);
 *  - per event, the command mid2agb would have written for it: the command
 *    byte (explicit, or running status by mid2agb's compression rule), the
 *    handler, and for a note ply_note's bytes (key / velocity / gate byte by
 *    the same rule, the note length predicted from the track's last note),
 *    voice resolution, the CGB test or the DirectSound scan over the
 *    engine's channels as they are before the note, the unlink by the
 *    channel's chain links, TrkVolPitSet, ChnVolSetAsm and the frequency
 *    routine; a note-off is no command (mid2agb writes N## with a gate)
 *    unless the note was longer than 96 ticks (TIE, then EOT);
 *  - the W## each track runs on a tick with an event, on mid2agb's bar
 *    marks and 96 ticks after its last one;
 *  - the FIFO DMA stall over the span.
 *
 * Events applied in a frame without a tempo tick are held for the next tick
 * (where MPlayMain would read them).  Integer adds and small loops over
 * the real channels; nothing allocated.
 */

#include "m4a_seq_estimate.h"
#include "m4a_sequencer.h"

#include <string.h>

/* gClockTable (m4a_tables.c): the N## / W## lengths. */
static const uint8_t kClock[49] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68,
    72, 76, 78, 80, 84, 88, 90, 92, 96
};

#define EST_NONE UINT32_MAX
#define EST_NOTE_LEN_MAX 96  /* mid2agb: longer notes are TIE + EOT (CreateTies) */
#define EST_SPLIT 96         /* mid2agb's TimeSplit: no wait longer than 96 */

/* A command byte through _081DD8E0..: explicit (BYTE + LOW below 0xBD, + RS
 * at or above, which also stores the running status) or a data byte under
 * running status. */
#define CYC_CMD_EXPLICIT_LOW (M4A_CYC_CMD_FETCH + M4A_CYC_CMD_BYTE + M4A_CYC_CMD_BYTE_LOW)
#define CYC_CMD_EXPLICIT_RS  (M4A_CYC_CMD_FETCH + M4A_CYC_CMD_BYTE + M4A_CYC_CMD_BYTE_RS)
#define CYC_CMD_RUNNING      (M4A_CYC_CMD_FETCH + M4A_CYC_CMD_RUNNING)
/* A W## command: 93 (docs/ACCURACY.md, appendix A). */
#define CYC_W                (CYC_CMD_EXPLICIT_LOW + M4A_CYC_CMD_WAIT)

/* Handlers, entry -> _081DD938, ROM song (m4a_sequencer.c cmd_cycles). */
#define CYC_H_VOL    89
#define CYC_H_PAN    91
#define CYC_H_BEND   91
#define CYC_H_BENDR  89
#define CYC_H_LFOS   69      /* 109 for 0 (clear_modM) */
#define CYC_H_LFODL  81
#define CYC_H_MOD    69      /* 109 for 0 */
#define CYC_H_MOD0   109
#define CYC_H_MODT   91      /* 95 when it changes */
#define CYC_H_MODT_CHG 95
#define CYC_H_TUNE   91
#define CYC_H_VOICE  184
#define CYC_H_KEYSH  89
#define CYC_H_TEMPO  91
#define CYC_H_GOTO   110

static inline int chan_count(void) { return M4A_EST_CHANNELS; }
static inline uint8_t ch_status(const M4AEngine *e, int h)
{
    return h < TOTAL_PCM_CHANNELS ? e->pcmChannels[h].status : e->cgbChannels[h - TOTAL_PCM_CHANNELS].status;
}
static inline int ch_track(const M4AEngine *e, int h)
{
    return h < TOTAL_PCM_CHANNELS ? e->pcmChannels[h].trackIndex : e->cgbChannels[h - TOTAL_PCM_CHANNELS].trackIndex;
}
static inline uint8_t ch_midi_key(const M4AEngine *e, int h)
{
    return h < TOTAL_PCM_CHANNELS ? e->pcmChannels[h].midiKey : e->cgbChannels[h - TOTAL_PCM_CHANNELS].midiKey;
}

/* mid2agb's g_noteVelocityLUT (4-step velocities). */
static inline uint8_t vel_lut(uint8_t v)
{
    if (v == 0)
        return 0;
    int q = ((v + 3) >> 2) << 2;
    return (uint8_t)(q > 127 ? 127 : q);
}

/* gClockTable's index of mid2agb's g_noteDurationLUT[d]: the largest table
 * length at most d (1 <= d <= 96). */
static int clock_index(int d)
{
    int i = 48;
    while (i > 0 && kClock[i] > d)
        i--;
    return i;
}

static inline uint32_t ev_clock(const M4ASeqEstimate *s) { return s->clock + s->tickInFrame; }
static inline int tick_index(const M4ASeqEstimate *s)
{
    return s->tickInFrame < M4A_EST_TICKS ? s->tickInFrame : M4A_EST_TICKS - 1;
}

void m4a_est_charge(M4AEngine *e, int cycles) { e->seqEst.cycles += cycles; }

/* The player's skeleton (m4a_sequencer_set_player's rule). */
static void set_player(M4ASeqEstimate *s, int player)
{
    static const uint8_t trackCounts[M4A_PLAYER_COUNT] = { 10, 3, 9, 1, 2, 2 };
    if (player < 0 || player >= M4A_PLAYER_COUNT)
        player = M4A_PLAYER_BGM;
    s->slots = trackCounts[player];
    if (player == M4A_PLAYER_BGM) {
        s->mpEntry = M4A_CYC_MP_BGM_ENTRY;
        s->mpHead = M4A_CYC_MP_HEAD;
        s->mpPaused = M4A_CYC_MP_PAUSED;
    } else {
        s->mpEntry = (int16_t)(M4A_CYC_MP_BGM_ENTRY + M4A_CYC_MP_PAUSED + (player - 1) * M4A_CYC_MP_IDLE);
        s->mpHead = M4A_CYC_MP_SE_HEAD;
        s->mpPaused = M4A_CYC_MP_IDLE;
    }
    s->mpExit = (int16_t)((M4A_CYC_MP_CHAIN - 1 - player) * M4A_CYC_MP_IDLE + M4A_CYC_MP_TAIL);
}

/* mid2agb's ResetTrackVars (a track's start, a label). */
static void reset_track_vars(M4ASeqEstimate *s, int i)
{
    s->lastOp[i] = 0;
    s->lastOpKey[i] = 0;
    s->lastKey[i] = 0xFF;
    s->lastVel[i] = 0xFF;
}

/* Forget the chains and the held notes (a panic or a stop: the channels
 * are gone, no note-off will come). */
static void drop_chains(M4ASeqEstimate *s)
{
    memset(s->owner, 0, sizeof(s->owner));
    memset(s->tie, 0, sizeof(s->tie));
    memset(s->chExpire, 0xFF, sizeof(s->chExpire));
    memset(s->ghostWalked, 0, sizeof(s->ghostWalked));
    memset(s->ghostCycles, 0, sizeof(s->ghostCycles));
    memset(s->keyOn, 0xFF, sizeof(s->keyOn));
}

/* Everything but the clock (the engine's accumulator keeps running): the
 * paused BGM player, no song, no track state. */
static void est_reset(M4ASeqEstimate *s)
{
    uint32_t clock = s->clock;
    uint8_t tif = s->tickInFrame;
    memset(s, 0, sizeof(*s));
    s->clock = clock;
    s->tickInFrame = tif;
    set_player(s, M4A_PLAYER_BGM);
    s->tempoClock = EST_NONE;
    for (int i = 0; i < MAX_TRACKS; i++) {
        reset_track_vars(s, i);
        s->lastEvClock[i] = EST_NONE;
    }
    drop_chains(s);
}

void m4a_est_init(M4AEngine *e)
{
    memset(&e->seqEst, 0, sizeof(e->seqEst));
    est_reset(&e->seqEst);
}

void m4a_est_reset(M4AEngine *e)
{
    est_reset(&e->seqEst);
}

void m4a_est_sound_off(M4AEngine *e)
{
    M4ASeqEstimate *s = &e->seqEst;
    if (!s->songDeclared) {
        est_reset(s);                 /* inferred: the song is over until the next note */
        return;
    }
    drop_chains(s);                   /* declared: the host says when the song stops */
}

static int popcount16(uint16_t v)
{
    int n = 0;
    for (; v; v &= (uint16_t)(v - 1))
        n++;
    return n;
}

/* A track the song starts with: MPT_FLG_EXIST | START; its first tick runs
 * the START init, then (mid2agb's track head) the implicit VOL when no VOL
 * comes before its first note, the first wait, KEYSH. */
static void start_track(M4ASeqEstimate *s, int i)
{
    s->declared |= (uint16_t)(1u << i);
    s->exist |= (uint16_t)(1u << i);
    s->startPending |= (uint16_t)(1u << i);
    s->firstNote &= (uint16_t)~(1u << i);
    s->volSeen &= (uint16_t)~(1u << i);
    reset_track_vars(s, i);
    s->lastEvClock[i] = EST_NONE;
    s->lastWClock[i] = ev_clock(s);
    s->lastDur[i] = 0;
    s->keep[i] = true;                 /* KEYSH is a PrintByte */
    /* KEYSH (0xBC, below 0xBD: explicit, no running status), PITCHG. */
    s->cycles += CYC_CMD_EXPLICIT_LOW + M4A_CYC_CMD_TABLE + CYC_H_KEYSH;
    s->extraChg[i] |= MPT_FLG_PITCHG;
    s->playing = true;
}

void m4a_engine_cost_song_start(M4AEngine *e, uint32_t trackMask, int player)
{
    M4ASeqEstimate *s = &e->seqEst;
    est_reset(s);
    set_player(s, player);
    s->songDeclared = true;
    s->barLen = 96;
    s->barStart = ev_clock(s);
    for (int i = 0; i < MAX_TRACKS; i++)
        if ((trackMask & (1u << i)) && popcount16(s->exist) < s->slots)
            start_track(s, i);
}

void m4a_engine_cost_song_stop(M4AEngine *e)
{
    est_reset(&e->seqEst);
}

void m4a_engine_cost_time_signature(M4AEngine *e, uint32_t clocksPerBar)
{
    M4ASeqEstimate *s = &e->seqEst;
    s->barLen = (uint16_t)(clocksPerBar > 0xFFFF ? 0xFFFF : clocksPerBar);
    s->barStart = ev_clock(s);
}

/* Every event of a track (a command or not) ends the wait before it: the
 * track runs a W## on this tick, and mid2agb's PrintWait arms the next
 * note's running status (s_keepLastOpName). */
static void touch(M4ASeqEstimate *s, int i)
{
    uint32_t c = ev_clock(s);
    if (s->lastEvClock[i] != c) {
        s->lastEvClock[i] = c;
        s->keep[i] = true;
    }
    s->cmdTicks[i] |= (uint8_t)(1u << tick_index(s));
}

/* Every existing track: a label (PrintSeqLoopLabel: an event, then
 * ResetTrackVars), and at the loop end first the GOTO (0xB2, a PrintByte,
 * with its address word: 110). */
void m4a_engine_cost_loop_point(M4AEngine *e, bool loopEnd)
{
    M4ASeqEstimate *s = &e->seqEst;
    if (!m4a_est_active(e) || !s->playing)
        return;
    /* The bar marks follow the song position: the GOTO takes them back to
     * the phase they had at the loop start. */
    uint32_t c = ev_clock(s);
    if (!loopEnd) {
        s->loopSaved = true;
        s->loopBarLen = s->barLen;
        s->loopBarPhase = (uint16_t)(s->barLen && c >= s->barStart ? (c - s->barStart) % s->barLen : 0);
    } else if (s->loopSaved) {
        s->barLen = s->loopBarLen;
        s->barStart = c - s->loopBarPhase;
    }
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (!(s->exist & (1u << i)))
            continue;
        touch(s, i);
        if (loopEnd)
            s->cycles += CYC_CMD_EXPLICIT_LOW + M4A_CYC_CMD_TABLE + CYC_H_GOTO;
        reset_track_vars(s, i);
        s->keep[i] = false;
    }
}

void m4a_engine_set_cost_estimate_forced(M4AEngine *e, bool forced)
{
    e->costEstimateForced = forced;
}

/* Whether an event of track i is a song command: in a declared song only
 * the song's tracks (mid2agb drops a channel without notes); inferred,
 * every track's. */
static inline bool song_track(const M4ASeqEstimate *s, int i)
{
    return !s->songDeclared || (s->declared & (1u << i));
}

/* A PrintOp command byte: running status when mid2agb's s_lastOpName is the
 * same command, then it becomes the last op. */
static int op_cost(M4ASeqEstimate *s, int i, uint8_t op)
{
    int c = s->lastOp[i] == op ? CYC_CMD_RUNNING : CYC_CMD_EXPLICIT_RS;
    s->lastOp[i] = op;
    s->lastOpKey[i] = 0;
    return c;
}

/* The track's chain, newest first.  Returns the count. */
static int chain_list(const M4ASeqEstimate *s, int i, int out[M4A_EST_CHANNELS])
{
    int n = 0;
    for (int h = 0; h < chan_count(); h++)
        if (s->owner[h] == i + 1)
            out[n++] = h;
    for (int a = 1; a < n; a++) {
        int h = out[a], b = a;
        while (b > 0 && s->chSeq[out[b - 1]] < s->chSeq[h]) {
            out[b] = out[b - 1];
            b--;
        }
        out[b] = h;
    }
    return n;
}

static void unchain(M4ASeqEstimate *s, int h)
{
    s->owner[h] = 0;
    s->tie[h] = false;
    s->chExpire[h] = EST_NONE;
}

/* RealClearChain by the channel's links (m4a_sequencer.c unlink_cost);
 * drops it from its chain. */
static int unlink_cost(M4ASeqEstimate *s, int h, int c00, int c01, int c10, int c11)
{
    int owner = s->owner[h];
    bool prev = false, next = false;
    for (int k = 0; k < chan_count(); k++) {
        if (k == h || s->owner[k] != owner)
            continue;
        if (s->chSeq[k] > s->chSeq[h]) prev = true;
        else next = true;
    }
    unchain(s, h);
    return prev ? (next ? c11 : c10) : (next ? c01 : c00);
}

static bool chain_live(const M4AEngine *e, int h, int i)
{
    return (ch_status(e, h) & CHN_ON) && ch_track(e, h) == i;
}

/* A live chained channel's step in its track's walk on tick `clock`
 * (_081DD892 .. _081DD8B4): its N## gate expiring on this tick, gate 0
 * after that (or released some other way, or a TIE: predicted, ended by
 * EOT, or older than 96 ticks), else a running N## gate. */
static int gate_cost(const M4AEngine *e, int h, uint32_t clock)
{
    const M4ASeqEstimate *s = &e->seqEst;
    uint32_t x = s->chExpire[h];
    if (x != EST_NONE)
        return clock == x ? M4A_CYC_WALK_GATE_EXPIRE
             : clock > x  ? M4A_CYC_WALK_GATE0
             : s->tie[h]  ? M4A_CYC_WALK_GATE0 : M4A_CYC_WALK_GATE_DEC;
    if ((ch_status(e, h) & CHN_STOP) || s->tie[h] || clock - s->chStart[h] > EST_NOTE_LEN_MAX)
        return M4A_CYC_WALK_GATE0;
    return M4A_CYC_WALK_GATE_DEC;
}

/* A command of track `track` on this tick is about to take channel h from
 * its chain (a note stealing it, FINE).  MPlayMain walks and runs the
 * tracks in order, so the owner's walks of this frame's earlier ticks met
 * the channel, and this tick's when the owner is this track or an earlier
 * one: those steps are charged to those walks now (a dead one the first
 * walk unlinked, so the command finds it unchained).  A channel started on
 * this tick was not walked yet. */
static void ghost_walk(M4AEngine *e, int h, int track)
{
    M4ASeqEstimate *s = &e->seqEst;
    uint32_t now = ev_clock(s);
    if (!s->owner[h] || s->chStart[h] >= now)
        return;
    int o = s->owner[h] - 1;
    int last = tick_index(s) - (o > track ? 1 : 0);
    for (int j = 0; j <= last && s->owner[h]; j++) {
        uint32_t c = s->clock + (uint32_t)j;
        if (s->chStart[h] >= c)
            continue;
        int g;
        if (chain_live(e, h, o))
            g = gate_cost(e, h, c);
        else
            g = M4A_CYC_WALK_DEAD + unlink_cost(s, h, M4A_CYC_WALK_UNLINK_00, M4A_CYC_WALK_UNLINK_01,
                                                M4A_CYC_WALK_UNLINK_10, M4A_CYC_WALK_UNLINK_11);
        s->ghostWalked[o][j]++;
        s->ghostCycles[o][j] = (int16_t)(s->ghostCycles[o][j] + g);
    }
}

static int tvps_cost(const M4ATrack *t, uint8_t flags, bool inNote)
{
    int c = 0;
    if (flags & MPT_FLG_VOLSET)
        c += M4A_CYC_TVPS_VOLSET + (t->modT == 1 ? M4A_CYC_TVPS_VOLSET_MODT1 : 0);
    if (flags & MPT_FLG_PITSET)
        c += M4A_CYC_TVPS_PITSET + (inNote && t->modT != 0 ? M4A_CYC_TVPS_PITSET_MODT : 0);
    return c;
}

static int cgb_key_cost(const M4ATrack *t, int key, bool noise)
{
    int k = key + t->keyM;
    if (k < 0)
        k = 0;
    if (noise)
        return k <= 20 ? M4A_CYC_NOISE_FREQ_LOW_KEY : k > 80 ? M4A_CYC_NOISE_FREQ_CLAMP : 0;
    return k <= 35 ? M4A_CYC_CGB_FREQ_LOW_KEY : 0;
}

/* ply_note's DirectSound scan over the engine's channels before the note
 * (m4a_sequencer.c scan_pcm_cost, the walk allocate_pcm_channel takes).
 * Returns the channel the GBA takes or -1; adds the cost. */
static int scan_pcm(const M4AEngine *e, int track, uint8_t priority, int *cyc)
{
    int n = e->maxPcmChannels;
    bool stopping = false;
    uint8_t bestPrio = priority;
    int bestTrack = track, chosen = -1;
    int c = M4A_CYC_NOTE_SCAN_ENTER;
    for (int k = 0; k < n; k++) {
        const M4APCMChannel *ch = &e->pcmChannels[k];
        if (!(ch->status & CHN_ON)) {
            *cyc += c + M4A_CYC_SCAN_FREE;
            return k;
        }
        bool compare;
        if (ch->status & CHN_STOP) {
            if (!stopping) {
                c += M4A_CYC_SCAN_FIRST_STOP;
                stopping = true;
                bestPrio = ch->priority;
                bestTrack = ch->trackIndex;
                chosen = k;
                compare = false;
            } else {
                c += M4A_CYC_SCAN_STOP;
                compare = true;
            }
        } else {
            c += M4A_CYC_SCAN_ON;
            if (stopping) {
                c += M4A_CYC_SCAN_ON_SKIP;
                compare = false;
            } else {
                c += M4A_CYC_SCAN_ON_CMP;
                compare = true;
            }
        }
        if (compare) {
            if (ch->priority < bestPrio) {
                c += M4A_CYC_SCAN_LOWER;
                bestPrio = ch->priority;
                bestTrack = ch->trackIndex;
                chosen = k;
            } else {
                c += M4A_CYC_SCAN_CMP;
                if (ch->priority > bestPrio) {
                    c += M4A_CYC_SCAN_HIGHER;
                } else if (ch->trackIndex > bestTrack) {
                    c += M4A_CYC_SCAN_LATER_TRACK;
                    bestTrack = ch->trackIndex;
                    chosen = k;
                } else if (ch->trackIndex == bestTrack) {
                    c += M4A_CYC_SCAN_SAME_TRACK;
                    chosen = k;
                } else {
                    c += M4A_CYC_SCAN_EARLIER_TRACK;
                }
            }
        }
        c += k + 1 < n ? M4A_CYC_SCAN_NEXT : (chosen >= 0 ? M4A_CYC_SCAN_DONE : M4A_CYC_SCAN_NONE);
    }
    *cyc += c;
    return chosen;
}


void m4a_est_note_on(M4AEngine *e, int i, uint8_t key, uint8_t velocity, const ToneData *voice)
{
    M4ASeqEstimate *s = &e->seqEst;
    if (e->auditionNote)
        return;                       /* a host preview, not a song command */
    M4ATrack *t = &e->tracks[i];
    uint16_t bit = (uint16_t)(1u << i);
    /* A track exists from its first note when nothing declared it (or the
     * host's song gained a track). */
    if (!(s->declared & bit) && popcount16(s->exist) < s->slots)
        start_track(s, i);
    touch(s, i);
    int cyc = 0;
    /* mid2agb's track head: VOL 127 when no VOL precedes the first note
     * (written at tick 0 before KEYSH; charged here, at the first note). */
    if (!(s->firstNote & bit)) {
        s->firstNote |= bit;
        if (!(s->volSeen & bit)) {
            cyc += CYC_CMD_EXPLICIT_RS + M4A_CYC_CMD_TABLE + CYC_H_VOL;   /* a PrintByte: explicit */
            s->extraChg[i] |= MPT_FLG_VOLCHG;
        }
    }

    /* The command: N## (the length predicted from the track's last note)
     * or TIE, and its bytes by mid2agb's PrintNote. */
    int d = s->lastDur[i];
    uint8_t op;
    int gtp = 0;
    if (d > EST_NOTE_LEN_MAX) {
        op = 0xCF;                    /* TIE */
    } else if (d > 0) {
        int idx = clock_index(d);
        op = (uint8_t)(0xCF + idx);
        gtp = d - kClock[idx];        /* -E: the gate byte */
    } else {
        op = 0;                       /* nothing known: explicit */
    }
    uint8_t vl = vel_lut(velocity);
    bool noteChanged = key != s->lastKey[i], velChanged = vl != s->lastVel[i];
    bool dataBytes = noteChanged || velChanged || gtp > 0;
    /* PrintNote omits the name only before data bytes (a repeat with the
     * same key and velocity is the bare name: PrintOp with no format). */
    bool rs = op != 0 && dataBytes && s->keep[i] && s->lastOp[i] == op;
    s->keep[i] = false;
    s->lastOp[i] = op;                /* 0 (unknown length): corrected at the note's end */
    s->lastOpKey[i] = (uint8_t)(key + 1);
    s->keyOn[i][key] = ev_clock(s);
    int entry = M4A_CYC_NOTE_ENTRY;
    if (dataBytes) {
        entry += M4A_CYC_NOTE_KEY_BYTE;
        s->lastKey[i] = key;
        if (velChanged || gtp > 0) {
            entry += M4A_CYC_NOTE_VEL_BYTE;
            s->lastVel[i] = vl;
            if (gtp > 0)
                entry += M4A_CYC_NOTE_GATE_BYTE;
        }
    }
    cyc += (rs ? CYC_CMD_RUNNING : CYC_CMD_EXPLICIT_RS) + M4A_CYC_CMD_NOTE + entry;

    if (!voice) {
        s->cycles += cyc + M4A_CYC_NOTE_VOICE_BAD + M4A_CYC_NOTE_EXIT;
        return;
    }
    uint8_t trackType = t->currentVoice.type;
    bool romVoice = (trackType & (VOICE_KEYSPLIT | VOICE_KEYSPLIT_ALL)) != 0;
    cyc += (trackType & VOICE_KEYSPLIT_ALL) ? M4A_CYC_NOTE_VOICE_DRUM
         : (trackType & VOICE_KEYSPLIT) ? M4A_CYC_NOTE_VOICE_SPLIT : M4A_CYC_NOTE_VOICE_PLAIN;
    uint8_t type = voice->type & VOICE_TYPE_CGB_MASK;
    int noteKey = (trackType & VOICE_KEYSPLIT_ALL) ? voice->key : key;
    uint8_t priority = t->priority;
    int h;
    if (type != 0) {
        cyc += M4A_CYC_NOTE_CGB_PATH;
        h = TOTAL_PCM_CHANNELS + type - 1;
        const M4ACGBChannel *ch = &e->cgbChannels[type - 1];
        if (!(ch->status & CHN_ON))
            cyc += M4A_CYC_NOTE_CGB_FREE;
        else if (ch->status & CHN_STOP)
            cyc += M4A_CYC_NOTE_CGB_STOPPING;
        else if (ch->priority < priority)
            cyc += M4A_CYC_NOTE_CGB_PRIO;
        else if (ch->priority > priority) {
            cyc += M4A_CYC_NOTE_CGB_REJECT_PRIO + M4A_CYC_NOTE_EXIT;
            h = -1;
        } else if (ch->trackIndex >= i)
            cyc += M4A_CYC_NOTE_CGB_TRACK;
        else {
            cyc += M4A_CYC_NOTE_CGB_REJECT_TRACK + M4A_CYC_NOTE_EXIT;
            h = -1;
        }
    } else {
        cyc += M4A_CYC_NOTE_PCM_PATH + (romVoice ? M4A_CYC_NOTE_ROM_VOICE_PATH : 0);
        h = voice->wav ? scan_pcm(e, i, priority, &cyc) : -1;
        if (h < 0)
            cyc += M4A_CYC_NOTE_EXIT;
    }
    if (h < 0) {
        s->cycles += cyc;
        return;
    }

    /* A channel still chained from before this tick: the walks met it
     * before this note took it (ply_note runs after the walk). */
    ghost_walk(e, h, i);

    /* ClearChain and the prepend (ply_note _081DDC40..). */
    cyc += M4A_CYC_NOTE_TAKE;
    if (s->owner[h])
        cyc += unlink_cost(s, h, M4A_CYC_NOTE_UNLINK_00, M4A_CYC_NOTE_UNLINK_01,
                           M4A_CYC_NOTE_UNLINK_10, M4A_CYC_NOTE_UNLINK_11);
    else
        cyc += M4A_CYC_NOTE_UNLINK_NONE;
    bool chainEmpty = true;
    for (int k = 0; k < chan_count() && chainEmpty; k++)
        if (s->owner[k] == i + 1)
            chainEmpty = false;
    if (chainEmpty)
        cyc += M4A_CYC_NOTE_CHAIN_EMPTY;
    uint8_t flags = (uint8_t)((t->chgFlags | s->extraChg[i]) & 0x0F);
    if (t->lfoDelay == 0) {
        cyc += M4A_CYC_NOTE_LFODL0;
    } else {
        cyc += M4A_CYC_NOTE_LFODL;
        flags |= t->modT == 0 ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;   /* clear_modM */
    }
    cyc += M4A_CYC_NOTE_TVPS + tvps_cost(t, flags, true) + (romVoice ? M4A_CYC_TVPS_ROM_VOICE : 0);
    cyc += M4A_CYC_NOTE_CHNVOL;
    if (type == 0)
        cyc += M4A_CYC_NOTE_PCM_FREQ;
    else
        cyc += (type == 4 ? M4A_CYC_NOTE_NOISE_FREQ : M4A_CYC_NOTE_CGB_FREQ) + cgb_key_cost(t, noteKey, type == 4)
             + (romVoice ? M4A_CYC_NOTE_CGB_ROM_VOICE : 0)
             + ((!(voice->panSweep & 0x80) && (voice->panSweep & 0x70)) ? M4A_CYC_NOTE_CGB_SWEEP : 0);
    cyc += M4A_CYC_NOTE_STORE + M4A_CYC_NOTE_EXIT;
    s->cycles += cyc;

    s->owner[h] = (uint8_t)(i + 1);
    s->chSeq[h] = ++s->seqCounter;
    s->tie[h] = op == 0xCF;           /* a predicted TIE: gate 0 */
    s->chStart[h] = ev_clock(s);
    s->chExpire[h] = EST_NONE;
    s->extraChg[i] = 0;               /* ply_note's flags &= 0xF0 */
}

void m4a_est_note_off(M4AEngine *e, int i, uint8_t key)
{
    M4ASeqEstimate *s = &e->seqEst;
    uint32_t c = ev_clock(s);
    if (s->keyOn[i][key] == EST_NONE)
        return;                       /* no note-on seen: nothing mid2agb wrote */
    uint32_t d = c - s->keyOn[i][key];
    s->keyOn[i][key] = EST_NONE;
    if (d < 1)
        d = 1;                        /* mid2agb's shortest note */
    s->lastDur[i] = (uint16_t)(d > 0xFFFF ? 0xFFFF : d);
    bool tie = d > EST_NOTE_LEN_MAX;
    /* The note's real command, now its length is known: mid2agb's
     * s_lastOpName when nothing was written after it. */
    if (s->lastOpKey[i] == key + 1)
        s->lastOp[i] = (uint8_t)(tie ? 0xCF : 0xCF + clock_index((int)d));
    for (int h = 0; h < chan_count(); h++) {
        if (s->owner[h] != i + 1 || !chain_live(e, h, i))
            continue;
        if ((ch_status(e, h) & CHN_STOP) || ch_midi_key(e, h) != key)
            continue;
        if (tie) {
            s->tie[h] = true;
            s->chExpire[h] = EST_NONE;
        } else {
            s->tie[h] = false;
            s->chExpire[h] = s->chStart[h] + d;   /* its N## gate's last walk */
        }
    }
    if (!tie || !song_track(s, i))
        return;
    /* EOT (0xCE): mid2agb's PrintEndOfTieOp -- the key byte when the key is
     * not the last note's, running status only then; ply_endtie's walk
     * newest first to the channel holding the key. */
    touch(s, i);
    int cyc;
    bool keyByte = key != s->lastKey[i];
    if (keyByte && s->keep[i]) {
        cyc = op_cost(s, i, 0xCE);
    } else {
        s->lastOp[i] = 0xCE;          /* s_lastOpName reset first: explicit */
        s->lastOpKey[i] = 0;
        cyc = CYC_CMD_EXPLICIT_RS;
    }
    if (keyByte)
        s->lastKey[i] = key;
    cyc += M4A_CYC_CMD_TABLE + M4A_CYC_ENDTIE + (keyByte ? M4A_CYC_ENDTIE_KEY : 0);
    int chain[M4A_EST_CHANNELS];
    int n = chain_list(s, i, chain);
    for (int k = 0; k < n; k++) {
        int h = chain[k];
        uint8_t st = ch_status(e, h);
        if (chain_live(e, h, i) && (st & (CHN_START | CHN_ENV_MASK)) && !(st & CHN_STOP)
            && ch_midi_key(e, h) == key) {
            cyc += M4A_CYC_ENDTIE_MATCH;
            break;
        }
        cyc += M4A_CYC_ENDTIE_SKIP;
    }
    s->cycles += cyc;
}

void m4a_est_cc(M4AEngine *e, int i, uint8_t cc, uint8_t value)
{
    M4ASeqEstimate *s = &e->seqEst;
    const M4ATrack *t = &e->tracks[i];
    if (!song_track(s, i))
        return;                       /* not one of the declared song's tracks */
    uint8_t op;
    int handler;
    switch (cc) {
    case 0x01: op = 0xC4; handler = value ? CYC_H_MOD : CYC_H_MOD0; break;
    case 0x07: op = 0xBE; handler = CYC_H_VOL; s->volSeen |= (uint16_t)(1u << i); break;
    case 0x0A: op = 0xBF; handler = CYC_H_PAN; break;
    case 0x14: op = 0xC1; handler = CYC_H_BENDR; break;
    case 0x15: op = 0xC2; handler = value ? CYC_H_LFOS : CYC_H_MOD0; break;
    case 0x16: op = 0xC5; handler = value != t->modT ? CYC_H_MODT_CHG : CYC_H_MODT; break;
    case 0x18: op = 0xC8; handler = CYC_H_TUNE; break;
    case 0x1A: op = 0xC3; handler = CYC_H_LFODL; break;
    case 0x21: case 0x27:             /* PRIO: a PrintByte (0xBA, explicit); the handler est. */
        touch(s, i);
        s->keep[i] = true;
        s->cycles += CYC_CMD_EXPLICIT_LOW + M4A_CYC_CMD_TABLE + M4A_CYC_CMD_DEFAULT;
        return;
    case 0x11:                        /* a label: ResetTrackVars */
        touch(s, i);
        reset_track_vars(s, i);
        s->keep[i] = false;
        return;
    case 0x1D: case 0x1F:             /* XCMD: charged by m4a_est_xcmd */
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10:   /* MEMACC: not modelled */
    case 0x1E:                        /* the XCMD select: no event of its own in mid2agb */
        if (cc != 0x1E)
            touch(s, i);
        return;
    case 0x78: case 0x7B:             /* the host's panic: no song command */
        return;
    default:                          /* any other controller: an event without a command */
        touch(s, i);
        return;
    }
    touch(s, i);
    s->cycles += op_cost(s, i, op) + M4A_CYC_CMD_TABLE + handler;
}

void m4a_est_program(M4AEngine *e, int i)
{
    M4ASeqEstimate *s = &e->seqEst;
    if (!song_track(s, i))
        return;
    touch(s, i);
    s->cycles += op_cost(s, i, 0xBD) + M4A_CYC_CMD_TABLE + CYC_H_VOICE;
}

void m4a_est_bend(M4AEngine *e, int i)
{
    M4ASeqEstimate *s = &e->seqEst;
    if (!song_track(s, i))
        return;
    touch(s, i);
    s->cycles += op_cost(s, i, 0xC0) + M4A_CYC_CMD_TABLE + CYC_H_BEND;
}

void m4a_est_xcmd(M4AEngine *e, int i, uint8_t command)
{
    M4ASeqEstimate *s = &e->seqEst;
    if (command != M4A_XCMD_IECV && command != M4A_XCMD_IECL)
        return;                       /* mid2agb writes only xIECV / xIECL */
    if (!song_track(s, i))
        return;
    touch(s, i);
    s->cycles += op_cost(s, i, 0xCD) + M4A_CYC_CMD_TABLE + M4A_CYC_XCMD_SUB + M4A_CYC_XIEC;
}

void m4a_est_track_end(M4AEngine *e, int i)
{
    M4ASeqEstimate *s = &e->seqEst;
    uint16_t bit = (uint16_t)(1u << i);
    if (!(s->exist & bit))
        return;
    touch(s, i);
    /* The walks met the chain before FINE (the dead ones they dropped). */
    for (int h = 0; h < chan_count(); h++)
        if (s->owner[h] == i + 1)
            ghost_walk(e, h, i);
    /* FINE (0xB1, explicit) and ply_fine over the chained channels. */
    int cyc = CYC_CMD_EXPLICIT_LOW + M4A_CYC_CMD_TABLE;
    int chain[M4A_EST_CHANNELS];
    int n = chain_list(s, i, chain);
    cyc += n == 0 ? M4A_CYC_FINE_EMPTY : M4A_CYC_FINE_LOOP;
    for (int k = 0; k < n; k++) {
        cyc += M4A_CYC_FINE_CHAN + M4A_CYC_FINE_UNLINK_CALL + M4A_CYC_FINE_UNLINK_LAST;
        unchain(s, chain[k]);
    }
    cyc += M4A_CYC_FINE_DONE;
    s->cycles += cyc;
    s->exist &= (uint16_t)~bit;
    s->startPending &= (uint16_t)~bit;
    s->ended |= bit;
    s->endTick[i] = (uint8_t)tick_index(s);
    s->extraChg[i] = 0;
}

void m4a_est_tempo(M4AEngine *e)
{
    M4ASeqEstimate *s = &e->seqEst;
    if (!s->playing || s->tempoClock == ev_clock(s))
        return;                       /* nothing playing, or this tick's TEMPO is charged */
    s->tempoClock = ev_clock(s);
    /* TEMPO (0xBB, a PrintByte) on mid2agb's first track. */
    int i = 0;
    while (i < MAX_TRACKS - 1 && !(s->exist & (1u << i)))
        i++;
    touch(s, i);
    s->keep[i] = true;
    s->cycles += CYC_CMD_EXPLICIT_LOW + M4A_CYC_CMD_TABLE + CYC_H_TEMPO;
}

void m4a_est_tick_boundary(M4AEngine *e)
{
    if (e->seqEst.tickInFrame < UINT8_MAX)
        e->seqEst.tickInFrame++;
}

uint16_t m4a_est_lfo_mask(const M4AEngine *e)
{
    return e->seqEst.playing ? e->seqEst.exist : 0;
}

/* Track i's chain walk on tick j of the frame (clock `clock`;
 * _081DD886 .. _081DD8BA). */
static int walk_cost(M4AEngine *e, int i, int j, uint32_t clock)
{
    M4ASeqEstimate *s = &e->seqEst;
    int chain[M4A_EST_CHANNELS];
    int n = chain_list(s, i, chain);
    int jj = j < M4A_EST_TICKS ? j : M4A_EST_TICKS - 1;
    int walked = s->ghostWalked[i][jj];
    int c = s->ghostCycles[i][jj];
    s->ghostWalked[i][jj] = 0;
    s->ghostCycles[i][jj] = 0;
    for (int k = 0; k < n; k++) {
        int h = chain[k];
        if (s->chStart[h] >= clock)
            continue;                 /* started by this tick's commands (after the walk) or a later tick's */
        walked++;
        if (!chain_live(e, h, i))
            c += M4A_CYC_WALK_DEAD + unlink_cost(s, h, M4A_CYC_WALK_UNLINK_00, M4A_CYC_WALK_UNLINK_01,
                                                 M4A_CYC_WALK_UNLINK_10, M4A_CYC_WALK_UNLINK_11);
        else
            c += gate_cost(e, h, clock);
    }
    if (!walked)
        return M4A_CYC_TICK_NO_CHAIN;
    return c + M4A_CYC_TICK_CHAIN + (walked - 1) * M4A_CYC_WALK_NEXT + M4A_CYC_WALK_END;
}

/* The VOLCHG / PITCHG pass for track i (_081DD9C8 .. _081DDA62). */
static int apply_cost(M4AEngine *e, int i)
{
    M4ASeqEstimate *s = &e->seqEst;
    const M4ATrack *t = &e->tracks[i];
    uint8_t flags = (uint8_t)((t->chgFlags | s->extraChg[i]) & 0x0F);
    s->extraChg[i] = 0;
    if (!flags)
        return M4A_CYC_APPLY_NO_FLAGS;
    bool vol = (flags & MPT_FLG_VOLCHG) != 0, pit = (flags & MPT_FLG_PITCHG) != 0;
    /* TrkVolPitSet runs before the channel loop: the keys below see the
     * new keyM (computed on a copy; the engine's own pass follows). */
    M4ATrack tv = *t;
    m4a_track_vol_pit_set(&tv);
    int c = M4A_CYC_APPLY_FLAGGED + M4A_CYC_APPLY_TVPS + tvps_cost(&tv, flags, false);
    int chain[M4A_EST_CHANNELS];
    int n = chain_list(s, i, chain);
    if (n == 0)
        c += M4A_CYC_APPLY_TVPS_EMPTY;
    for (int k = 0; k < n; k++) {
        int h = chain[k];
        if (!chain_live(e, h, i)) {
            c += M4A_CYC_APPLY_CH_DEAD;
            unchain(s, h);
            continue;
        }
        bool isCgb = h >= TOTAL_PCM_CHANNELS;
        bool isNoise = isCgb && e->cgbChannels[h - TOTAL_PCM_CHANNELS].type == 4;
        int keyCost = isCgb ? cgb_key_cost(&tv, e->cgbChannels[h - TOTAL_PCM_CHANNELS].key, isNoise) : 0;
        c += M4A_CYC_APPLY_CH;
        c += vol ? M4A_CYC_APPLY_CH_VOL + (isCgb ? M4A_CYC_APPLY_CH_VOL_CGB : M4A_CYC_APPLY_CH_VOL_PCM)
                 : M4A_CYC_APPLY_CH_NOVOL;
        c += pit ? M4A_CYC_APPLY_CH_PIT + (isNoise ? M4A_CYC_APPLY_CH_PIT_NOISE + keyCost
                                           : isCgb ? M4A_CYC_APPLY_CH_PIT_CGB + keyCost : M4A_CYC_APPLY_CH_PIT_PCM)
                 : M4A_CYC_APPLY_CH_NOPIT;
        c += k + 1 < n ? M4A_CYC_APPLY_CH_NEXT : M4A_CYC_APPLY_CH_LAST;
    }
    return c + M4A_CYC_APPLY_TRACK_END;
}

static bool bar_mark(const M4ASeqEstimate *s, uint32_t c)
{
    return s->barLen && c >= s->barStart && (c - s->barStart) % s->barLen == 0;
}

void m4a_est_frame(M4AEngine *e, int nTicks)
{
    M4ASeqEstimate *s = &e->seqEst;
    s->allDone = false;
    if (!s->playing) {
        s->clock += (uint32_t)nTicks;
        s->tickInFrame = 0;
        memset(s->cmdTicks, 0, sizeof(s->cmdTicks));
        s->ended = 0;
        s->held = 0;
        return;
    }
    /* A frame without a tick runs no command: what the host applied waits
     * for the next tick, as the song's bytes would (its cmdTicks bit 0, its
     * channels' start ticks and the ghosts are that tick's already). */
    if (nTicks == 0) {
        s->held += s->cycles;
        s->cycles = 0;
    } else {
        s->cycles += s->held;
        s->held = 0;
    }
    int cyc = 0;
    for (int j = 0; j < nTicks; j++) {
        uint32_t clock = s->clock + (uint32_t)j;
        int jj = j < M4A_EST_TICKS ? j : M4A_EST_TICKS - 1;
        int n = 0;
        cyc += M4A_CYC_TICK_ENTER;
        for (int i = 0; i < MAX_TRACKS && n < s->slots; i++) {
            uint16_t bit = (uint16_t)(1u << i);
            bool endedNow = (s->ended & bit) && jj <= s->endTick[i];
            if (!(s->exist & bit) && !endedNow)
                continue;
            n++;
            cyc += M4A_CYC_TICK_TRACK + M4A_CYC_TICK_TRACK_END;
            cyc += walk_cost(e, i, j, clock);
            cyc += (j == 0 && (s->startPending & bit)) ? M4A_CYC_TICK_START : M4A_CYC_TICK_NO_START;
            if (endedNow && jj == s->endTick[i])
                continue;             /* FINE ran on this tick: no wait */
            bool ev = (s->cmdTicks[i] >> jj) & 1;
            if (ev || bar_mark(s, clock) || clock - s->lastWClock[i] >= EST_SPLIT) {
                cyc += CYC_W;
                s->lastWClock[i] = clock;
            }
        }
        cyc += (s->slots - n) * M4A_CYC_TICK_NO_TRACK + (s->slots - 1) * M4A_CYC_TICK_NEXT + M4A_CYC_TICK_LAST;
        if (n == 0) {
            cyc += M4A_CYC_TICK_ALL_DONE;
            s->playing = false;
            s->allDone = true;
            break;
        }
        cyc += M4A_CYC_TICK_EXIT;
    }
    if (nTicks > 0)
        s->startPending = 0;
    if (!s->allDone) {
        int n = 0;
        cyc += M4A_CYC_APPLY_ENTER + (s->slots - 1) * M4A_CYC_APPLY_NEXT + M4A_CYC_APPLY_LAST;
        for (int i = 0; i < MAX_TRACKS && n < s->slots; i++) {
            if (!(s->exist & (1u << i)))
                continue;
            n++;
            cyc += apply_cost(e, i);
        }
        cyc += (s->slots - n) * M4A_CYC_APPLY_NO_TRACK;
    }
    s->cycles += cyc;
    s->clock += (uint32_t)nTicks;
    s->tickInFrame = 0;
    if (nTicks > 0) {
        memset(s->cmdTicks, 0, sizeof(s->cmdTicks));
        s->ended = 0;
    }
}

int32_t m4a_est_publish(M4AEngine *e, int64_t stamp)
{
    M4ASeqEstimate *s = &e->seqEst;
    int64_t body = (s->playing || s->allDone) ? s->mpHead + (int64_t)s->cycles + s->mpExit
                                              : s->mpPaused + s->mpExit;
    s->cycles = 0;
    if (!s->playing)
        s->held = 0;
    /* From the player's entry (line 160 + mpEntry) as the sequencer's cyc()
     * charges the FIFO stall. */
    int64_t from = stamp - M4A_CYC_SOUNDMAIN_ENTRY + s->mpEntry;
    int64_t stall = m4a_gba_fifo_dma_stall(&e->gba, from, from + body);
    if (stall)
        stall += m4a_gba_fifo_dma_stall(&e->gba, from + body, from + body + stall);
    return (int32_t)(s->mpEntry + body + stall - M4A_CYC_M4A_SOUNDMAIN);
}
