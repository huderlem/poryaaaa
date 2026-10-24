/*
 * Song sequencer: port of MPlayMain and the ply_* handlers.  See
 * m4a_sequencer.h.  Comments name the GBA routine each piece comes from;
 * the structure follows m4a_1.s so it can be checked line by line.
 */

#include "m4a_sequencer.h"
#include "m4a_reverb.h"

#include <string.h>

/* gClockTable (m4a_tables.c): wait / note lengths for the single-byte forms. */
static const uint8_t gClockTable[49] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68,
    72, 76, 78, 80, 84, 88, 90, 92, 96
};

#define C_V 0x40

/* ------------------------------------------------------------------------
 * Channel access by handle (m4a_engine_note_on's return value)
 * ------------------------------------------------------------------------ */

#define CHAN_COUNT (TOTAL_PCM_CHANNELS + TOTAL_CGB_CHANNELS)

static inline bool chan_is_pcm(int h) { return h < TOTAL_PCM_CHANNELS; }

static inline uint8_t *chan_status(M4AEngine *e, int h)
{
    return chan_is_pcm(h) ? &e->pcmChannels[h].status : &e->cgbChannels[h - TOTAL_PCM_CHANNELS].status;
}

static inline uint8_t *chan_gate_time(M4AEngine *e, int h)
{
    return chan_is_pcm(h) ? &e->pcmChannels[h].gateTime : &e->cgbChannels[h - TOTAL_PCM_CHANNELS].gateTime;
}

static inline int chan_track(M4AEngine *e, int h)
{
    return chan_is_pcm(h) ? e->pcmChannels[h].trackIndex : e->cgbChannels[h - TOTAL_PCM_CHANNELS].trackIndex;
}

static inline uint8_t chan_midi_key(M4AEngine *e, int h)
{
    return chan_is_pcm(h) ? e->pcmChannels[h].midiKey : e->cgbChannels[h - TOTAL_PCM_CHANNELS].midiKey;
}

/* Iterate the real (non-shadow) channel pools: the ones the GBA has. */
static inline bool chan_is_real(int h)
{
    return chan_is_pcm(h) ? h < MAX_PCM_CHANNELS : (h - TOTAL_PCM_CHANNELS) < MAX_CGB_CHANNELS;
}

/* A channel "belongs to the track's chain" while it is ON and owned by the
 * track.  (The GBA keeps an explicit linked list; dead channels drop out of
 * it via ClearChain, which this test reproduces.) */
static inline bool chan_in_chain(M4AEngine *e, int h, int track)
{
    return chan_is_real(h) && (*chan_status(e, h) & CHN_ON) && chan_track(e, h) == track;
}

/* ------------------------------------------------------------------------
 * Song data access
 * ------------------------------------------------------------------------ */

static inline uint8_t rd8(const M4ASequencer *s, uint32_t off)
{
    return off < s->song->size ? s->song->data[off] : 0xB1; /* FINE past the end */
}

static inline uint32_t rd32(const M4ASequencer *s, uint32_t off)
{
    return (uint32_t)rd8(s, off) | ((uint32_t)rd8(s, off + 1) << 8)
         | ((uint32_t)rd8(s, off + 2) << 16) | ((uint32_t)rd8(s, off + 3) << 24);
}

/* ld_r3_tp_adr_i: fetch the next command byte. */
static inline uint8_t next_byte(M4ASequencer *s, M4ASeqTrack *t)
{
    return rd8(s, t->cmdPtr++);
}

/* ------------------------------------------------------------------------
 * Engine track helpers
 * ------------------------------------------------------------------------ */

static void set_engine_priority(M4ASequencer *s, int i)
{
    uint32_t p = (uint32_t)s->priority + s->tracks[i].priority;
    s->engine->tracks[i].priority = p > 0xFF ? 0xFF : (uint8_t)p;
}

/* TrackStop: silence and release every channel of the track. */
static void track_stop(M4ASequencer *s, int i)
{
    M4AEngine *e = s->engine;
    if (!(s->tracks[i].flags & MPT_FLG_EXIST))
        return;
    for (int h = 0; h < CHAN_COUNT; h++) {
        if (!chan_is_real(h) || chan_track(e, h) != i)
            continue;
        if (*chan_status(e, h))
            m4a_engine_kill_channel(e, h);   /* CgbOscOff for CGB channels */
    }
}

/* MPlayMain's MPT_FLG_START handling: Clear64byte(track) then the defaults.
 * The engine track mirrors MusicPlayerTrack's parameter fields. */
static void track_start(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    M4ATrack *et = &s->engine->tracks[i];
    uint32_t cmdPtr = t->cmdPtr;    /* Clear64byte stops short of cmdPtr / patternStack */
    uint32_t stack[3];
    memcpy(stack, t->patternStack, sizeof(stack));
    memset(t, 0, sizeof(*t));
    t->cmdPtr = cmdPtr;
    memcpy(t->patternStack, stack, sizeof(stack));
    t->flags = MPT_FLG_EXIST;

    et->rawVolume = 0;
    et->volume = 0;
    et->volX = 0x40;
    et->pan = 0;
    et->panX = 0;
    et->bend = 0;
    et->bendRange = 2;
    et->lfoSpeed = 0x16;
    et->lfoSpeedC = 0;
    et->lfoDelay = 0;
    et->lfoDelayC = 0;
    et->mod = 0;
    et->modT = 0;
    et->modM = 0;
    et->keyShift = 0;
    et->keyShiftX = 0;
    et->tune = 0;
    et->pitX = 0;
    et->pseudoEchoVolume = 0;
    et->pseudoEchoLength = 0;
    et->currentProgram = 0;
    memset(&et->currentVoice, 0, sizeof(et->currentVoice));
    et->currentVoice.type = 1;
    set_engine_priority(s, i);
    m4a_track_vol_pit_set(et);
}

/* clear_modM */
static void clear_mod_m(M4ASequencer *s, int i)
{
    M4ATrack *et = &s->engine->tracks[i];
    et->modM = 0;
    et->lfoSpeedC = 0;
    s->tracks[i].flags |= (et->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
}

/* ------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------ */

/* ply_fine */
static void ply_fine(M4ASequencer *s, int i)
{
    M4AEngine *e = s->engine;
    for (int h = 0; h < CHAN_COUNT; h++) {
        if (!chan_is_real(h) || chan_track(e, h) != i)
            continue;
        uint8_t *st = chan_status(e, h);
        if (*st & CHN_ON)
            *st |= CHN_STOP;
    }
    s->tracks[i].flags = 0;
}

/* ply_goto: the word at cmdPtr is the target (an offset here, an address on
 * the GBA). */
static void ply_goto(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    t->cmdPtr = rd32(s, t->cmdPtr);
}

static void ply_patt(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    if (t->patternLevel >= 3) {
        ply_fine(s, i);
        return;
    }
    t->patternStack[t->patternLevel] = t->cmdPtr + 4;
    t->patternLevel++;
    ply_goto(s, i);
}

static void ply_pend(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    if (t->patternLevel != 0) {
        t->patternLevel--;
        t->cmdPtr = t->patternStack[t->patternLevel];
    }
}

static void ply_rept(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    uint8_t n = rd8(s, t->cmdPtr);
    if (n == 0) {
        t->cmdPtr++;
        ply_goto(s, i);
        return;
    }
    t->repN++;
    uint32_t base = t->cmdPtr;
    t->cmdPtr++;
    if (t->repN < n) {
        ply_goto(s, i);
    } else {
        t->repN = 0;
        t->cmdPtr = base + 5;
    }
}

#define MEMACC_COND_JUMP(cond) do { if (cond) ply_goto(s, i); else t->cmdPtr += 4; return; } while (0)

/* ply_memacc (m4a.c) */
static void ply_memacc(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    uint8_t op = next_byte(s, t);
    uint8_t *addr = &s->memAccArea[next_byte(s, t) & 0x0F];
    uint8_t data = next_byte(s, t);
    uint8_t *mem = s->memAccArea;

    switch (op) {
    case 0: *addr = data; return;
    case 1: *addr += data; return;
    case 2: *addr -= data; return;
    case 3: *addr = mem[data & 0x0F]; return;
    case 4: *addr += mem[data & 0x0F]; return;
    case 5: *addr -= mem[data & 0x0F]; return;
    case 6: MEMACC_COND_JUMP(*addr == data);
    case 7: MEMACC_COND_JUMP(*addr != data);
    case 8: MEMACC_COND_JUMP(*addr > data);
    case 9: MEMACC_COND_JUMP(*addr >= data);
    case 10: MEMACC_COND_JUMP(*addr <= data);
    case 11: MEMACC_COND_JUMP(*addr < data);
    case 12: MEMACC_COND_JUMP(*addr == mem[data & 0x0F]);
    case 13: MEMACC_COND_JUMP(*addr != mem[data & 0x0F]);
    case 14: MEMACC_COND_JUMP(*addr > mem[data & 0x0F]);
    case 15: MEMACC_COND_JUMP(*addr >= mem[data & 0x0F]);
    case 16: MEMACC_COND_JUMP(*addr <= mem[data & 0x0F]);
    case 17: MEMACC_COND_JUMP(*addr < mem[data & 0x0F]);
    default: return;
    }
}

static void ply_prio(M4ASequencer *s, int i)
{
    s->tracks[i].priority = next_byte(s, &s->tracks[i]);
    set_engine_priority(s, i);
}

static void ply_tempo(M4ASequencer *s, int i)
{
    s->tempoD = (uint16_t)(next_byte(s, &s->tracks[i]) << 1);
    s->tempoI = (uint16_t)((s->tempoD * s->tempoU) >> 8);
}

static void ply_keysh(M4ASequencer *s, int i)
{
    s->engine->tracks[i].keyShift = (int8_t)next_byte(s, &s->tracks[i]);
    s->tracks[i].flags |= MPT_FLG_PITCHG;
}

static void ply_voice(M4ASequencer *s, int i)
{
    m4a_engine_program_change(s->engine, i, next_byte(s, &s->tracks[i]));
}

static void ply_vol(M4ASequencer *s, int i)
{
    uint8_t v = next_byte(s, &s->tracks[i]);
    /* The master volume is baked into the song, so no songMasterVolume scaling. */
    s->engine->tracks[i].rawVolume = v;
    s->engine->tracks[i].volume = v;
    s->tracks[i].flags |= MPT_FLG_VOLCHG;
}

static void ply_pan(M4ASequencer *s, int i)
{
    s->engine->tracks[i].pan = (int8_t)(next_byte(s, &s->tracks[i]) - C_V);
    s->tracks[i].flags |= MPT_FLG_VOLCHG;
}

static void ply_bend(M4ASequencer *s, int i)
{
    s->engine->tracks[i].bend = (int8_t)(next_byte(s, &s->tracks[i]) - C_V);
    s->tracks[i].flags |= MPT_FLG_PITCHG;
}

static void ply_bendr(M4ASequencer *s, int i)
{
    s->engine->tracks[i].bendRange = next_byte(s, &s->tracks[i]);
    s->tracks[i].flags |= MPT_FLG_PITCHG;
}

static void ply_lfos(M4ASequencer *s, int i)
{
    uint8_t v = next_byte(s, &s->tracks[i]);
    s->engine->tracks[i].lfoSpeed = v;
    if (v == 0)
        clear_mod_m(s, i);
}

static void ply_lfodl(M4ASequencer *s, int i)
{
    s->engine->tracks[i].lfoDelay = next_byte(s, &s->tracks[i]);
}

static void ply_mod(M4ASequencer *s, int i)
{
    uint8_t v = next_byte(s, &s->tracks[i]);
    s->engine->tracks[i].mod = v;
    if (v == 0)
        clear_mod_m(s, i);
}

static void ply_modt(M4ASequencer *s, int i)
{
    uint8_t v = next_byte(s, &s->tracks[i]);
    if (s->engine->tracks[i].modT != v) {
        s->engine->tracks[i].modT = v;
        s->tracks[i].flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

static void ply_tune(M4ASequencer *s, int i)
{
    s->engine->tracks[i].tune = (int8_t)(next_byte(s, &s->tracks[i]) - C_V);
    s->tracks[i].flags |= MPT_FLG_PITCHG;
}

/* ply_port writes a sound register directly; nothing to emulate here. */
static void ply_port(M4ASequencer *s, int i)
{
    s->tracks[i].cmdPtr += 2;
}

/* ply_xcmd and gXcmdTable (m4a.c).  Only the pseudo-echo commands affect
 * playback through the engine; the rest consume their operands. */
static void ply_xcmd(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    uint8_t n = next_byte(s, t);
    switch (n) {
    case 0x01: t->cmdPtr += 4; break;             /* xwave */
    case 0x02: case 0x04: case 0x05: case 0x06:  /* xtype xatta xdeca xsust */
    case 0x07: case 0x0A: case 0x0B:             /* xrele xleng xswee */
        t->cmdPtr += 1;
        break;
    case 0x08:                                    /* xiecv */
        m4a_engine_xcmd(s->engine, i, M4A_XCMD_IECV, next_byte(s, t));
        break;
    case 0x09:                                    /* xiecl */
        m4a_engine_xcmd(s->engine, i, M4A_XCMD_IECL, next_byte(s, t));
        break;
    case 0x0C: {                                  /* xwait */
        uint16_t len = (uint16_t)(rd8(s, t->cmdPtr) | (rd8(s, t->cmdPtr + 1) << 8));
        if (t->timer < len) {
            t->timer++;
            t->cmdPtr -= 2;
            t->wait = 1;
        } else {
            t->timer = 0;
            t->cmdPtr += 2;
        }
        break;
    }
    case 0x0D: t->cmdPtr += 4; break;             /* unk_3C */
    default:                                      /* ply_xxx: gMPlayJumpTable[0] = ply_fine */
        ply_fine(s, i);
        break;
    }
}

/* ply_endtie */
static void ply_endtie(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    M4AEngine *e = s->engine;
    uint8_t key = rd8(s, t->cmdPtr);
    if (key < 0x80) {
        t->key = key;
        t->cmdPtr++;
    } else {
        key = t->key;
    }

    /* Newest matching channel first (the chain is built by prepending). */
    int best = -1;
    uint32_t bestSeq = 0;
    for (int h = 0; h < CHAN_COUNT; h++) {
        if (!chan_in_chain(e, h, i))
            continue;
        uint8_t st = *chan_status(e, h);
        if (!(st & (CHN_START | CHN_ENV_MASK)) || (st & CHN_STOP))
            continue;
        if (chan_midi_key(e, h) != key)
            continue;
        if (best < 0 || s->chanSeq[h] > bestSeq) {
            best = h;
            bestSeq = s->chanSeq[h];
        }
    }
    if (best >= 0)
        *chan_status(e, best) |= CHN_STOP;
}

/* ply_note: cmd is the note length index (0 = TIE). */
static void ply_note(M4ASequencer *s, int i, uint8_t cmd)
{
    M4ASeqTrack *t = &s->tracks[i];
    t->gateTime = gClockTable[cmd];

    uint8_t b = rd8(s, t->cmdPtr);
    if (b < 0x80) {
        t->key = b;
        t->cmdPtr++;
        b = rd8(s, t->cmdPtr);
        if (b < 0x80) {
            t->velocity = b;
            t->cmdPtr++;
            b = rd8(s, t->cmdPtr);
            if (b < 0x80) {
                t->gateTime = (uint8_t)(t->gateTime + b);
                t->cmdPtr++;
            }
        }
    }

    /* Voice resolution, priority, channel allocation and the note start are
     * the engine's note-on (see m4a_engine_note_on for the correspondence).
     * It returns the channel it started, if any. */
    bool cgbBusy[MAX_CGB_CHANNELS];
    for (int c = 0; c < MAX_CGB_CHANNELS; c++)
        cgbBusy[c] = s->engine->cgbChannels[c].status != 0;
    int h = m4a_engine_note_on(s->engine, i, t->key, t->velocity);
    if (h < 0) {
        s->frameCycles += M4A_CYC_NOTE_NONE;
        return;
    }
    if (chan_is_pcm(h)) {
        s->frameCycles += M4A_CYC_NOTE_PCM + M4A_CYC_NOTE_PCM_SCAN * s->engine->lastNoteScan;
    } else {
        int c = h - TOTAL_PCM_CHANNELS;
        s->frameCycles += s->engine->cgbChannels[c].type == 4 ? M4A_CYC_NOTE_NOISE : M4A_CYC_NOTE_CGB;
        /* A CGB channel still sounding is taken over (ClearChain etc.). */
        if (c < MAX_CGB_CHANNELS && cgbBusy[c])
            s->frameCycles += M4A_CYC_NOTE_CGB_BUSY;
    }
    *chan_gate_time(s->engine, h) = t->gateTime;
    s->chanSeq[h] = ++s->seqCounter;
    s->chainOwner[h] = (uint8_t)(i + 1);
    s->noteCount++;

    /* ply_note ran TrkVolPitSet for the new note; the pending flags are
     * dropped, so older channels wait for the next change. */
    t->flags &= 0xF0;
}

/* Measured handler + dispatch cost of a command (delta from its ply_* entry
 * to the next breakpoint in mgba); unmeasured ones take the default. */
static int cmd_cycles(uint8_t cmd)
{
    switch (cmd) {
    case 0xBB: return 184;   /* tempo */
    case 0xBC: return 182;   /* keysh */
    case 0xBD: return 277;   /* voice */
    case 0xBE: return 182;   /* vol */
    case 0xBF: return 184;   /* pan */
    case 0xC0: return 168;   /* bend */
    case 0xC1: return 182;   /* bendr */
    case 0xC2: return 162;   /* lfos */
    case 0xC4: return 164;   /* mod */
    case 0xC5: return 184;   /* modt */
    case 0xC8: return 184;   /* tune */
    case 0xCD: return 203;   /* xcmd + its sub-command */
    default:   return M4A_CYC_CMD_DEFAULT;
    }
}

/* gMPlayJumpTable, indexed by command - 0xB1, after m4aSoundInit's patches. */
static void dispatch(M4ASequencer *s, int i, uint8_t cmd)
{
    s->frameCycles += cmd_cycles(cmd);
    switch (cmd) {
    case 0xB1: ply_fine(s, i); break;
    case 0xB2: {
        M4ASeqTrack *t = &s->tracks[i];
        uint32_t from = t->cmdPtr;
        ply_goto(s, i);
        if (t->cmdPtr < from) {
            if (s->loopTrack < 0)
                s->loopTrack = i;
            if (s->loopTrack == i)
                s->loopPasses++;
        }
        break;
    }
    case 0xB3: ply_patt(s, i); break;
    case 0xB4: ply_pend(s, i); break;
    case 0xB5: ply_rept(s, i); break;
    case 0xB9: ply_memacc(s, i); break;
    case 0xBA: ply_prio(s, i); break;
    case 0xBB: ply_tempo(s, i); break;
    case 0xBC: ply_keysh(s, i); break;
    case 0xBD: ply_voice(s, i); break;
    case 0xBE: ply_vol(s, i); break;
    case 0xBF: ply_pan(s, i); break;
    case 0xC0: ply_bend(s, i); break;
    case 0xC1: ply_bendr(s, i); break;
    case 0xC2: ply_lfos(s, i); break;
    case 0xC3: ply_lfodl(s, i); break;
    case 0xC4: ply_mod(s, i); break;
    case 0xC5: ply_modt(s, i); break;
    case 0xC8: ply_tune(s, i); break;
    case 0xCC: ply_port(s, i); break;
    case 0xCD: ply_xcmd(s, i); break;
    case 0xCE: ply_endtie(s, i); break;
    default:   ply_fine(s, i); break;   /* 0xB6-B8, C6, C7, C9-CB */
    }
}

/* ------------------------------------------------------------------------
 * MPlayMain
 * ------------------------------------------------------------------------ */

/* The per-tick LFO step (MPlayMain, after the wait countdown). */
static void lfo_step(M4ASequencer *s, int i)
{
    M4ATrack *et = &s->engine->tracks[i];
    if (et->lfoSpeed == 0 || et->mod == 0)
        return;
    if (et->lfoDelayC != 0) {
        et->lfoDelayC--;
        return;
    }
    s->frameCycles += M4A_CYC_LFO_STEP;
    et->lfoSpeedC = (uint8_t)(et->lfoSpeedC + et->lfoSpeed);
    int32_t r;
    if ((int8_t)(et->lfoSpeedC - 0x40) < 0)
        r = (int8_t)et->lfoSpeedC;
    else
        r = 0x80 - et->lfoSpeedC;
    r = (et->mod * r) >> 6;
    if ((uint8_t)(et->modM ^ r) != 0) {
        et->modM = (int8_t)r;
        s->tracks[i].flags |= (et->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
    }
}

/* One song tick for one track: gate times, START init, command loop. */
static void track_tick(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    M4AEngine *e = s->engine;

    /* Walk the track's channels: count down gate times; channels that have
     * died drop out of the chain (ClearChain). */
    for (int h = 0; h < CHAN_COUNT; h++) {
        if (s->chainOwner[h] == i + 1 && !chan_in_chain(e, h, i)) {
            /* The channel died (or was taken over): ClearChain drops it. */
            s->chainOwner[h] = 0;
            if (chan_track(e, h) == i)
                s->frameCycles += M4A_CYC_CLEAR_CHAIN;
        }
        if (!chan_in_chain(e, h, i))
            continue;
        uint8_t *gate = chan_gate_time(e, h);
        if (*gate != 0) {
            if (--*gate == 0) {
                *chan_status(e, h) |= CHN_STOP;
                s->frameCycles += M4A_CYC_GATE_EXPIRY;
            }
        }
    }

    if (t->flags & MPT_FLG_START) {
        s->frameCycles += M4A_CYC_TRACK_START;
        track_start(s, i);
    }

    for (;;) {
        if (t->wait != 0) {
            t->wait--;
            lfo_step(s, i);
            return;
        }

        uint8_t c = rd8(s, t->cmdPtr);
        bool explicitCmd = c >= 0x80;
        if (c < 0x80) {
            c = t->runningStatus;
            if (c < 0x80) {
                /* A data byte before any command: the GBA indexes
                 * gClockTable out of bounds and hangs on the same byte. */
                ply_fine(s, i);
                return;
            }
        } else {
            t->cmdPtr++;
            if (c >= 0xBD)
                t->runningStatus = c;
        }

        if (c >= 0xCF) {
            if (explicitCmd)
                s->frameCycles += M4A_CYC_NOTE_CMD_BYTE;   /* explicit N## byte read */
            ply_note(s, i, (uint8_t)(c - 0xCF));
        } else if (c > 0xB0) {
            dispatch(s, i, c);
            if (t->flags == 0)
                return;          /* FINE: no LFO step this tick */
        } else {
            t->wait = gClockTable[c - 0x80];
            s->frameCycles += M4A_CYC_WAIT_CMD;
        }
    }
}

void m4a_sequencer_frame(M4ASequencer *s)
{
    /* SoundMain timing model: the CgbSound entry follows MPlayMain's work.
     * Costs are the probe measurements in m4a_engine.h; the running sum is
     * published as cycles from the SoundMain entry. */
    s->frameCycles = M4A_CYC_CGB_ENTRY_PAUSED;
    if (!s->song || (s->status & MUSICPLAYER_STATUS_PAUSE)) {
        s->engine->cgbEntryCycles = s->frameCycles - M4A_CYC_SOUNDMAIN_ENTRY;
        return;
    }

    int exist = 0;
    for (int i = 0; i < s->trackCount; i++)
        if (s->tracks[i].flags & MPT_FLG_EXIST)
            exist++;
    s->frameCycles = M4A_CYC_CGB_ENTRY_BASE + M4A_CYC_PER_TRACK * exist;

    /* FadeOutBody: no fades are started by this host, so nothing to do. */

    s->tempoC = (uint16_t)(s->tempoC + s->tempoI);
    while (s->tempoC >= 150) {
        uint32_t trackBits = 0;
        s->frameCycles += M4A_CYC_TICK_BASE;
        for (int i = 0; i < s->trackCount; i++) {
            if (!(s->tracks[i].flags & MPT_FLG_EXIST))
                continue;
            s->frameCycles += trackBits ? M4A_CYC_TICK_PER_TRACK : M4A_CYC_TICK_FIRST_TRACK;
            trackBits |= 1u << i;
            track_tick(s, i);
        }
        s->clock++;
        if (trackBits == 0) {
            s->status = MUSICPLAYER_STATUS_PAUSE;
            s->engine->cgbEntryCycles = s->frameCycles - M4A_CYC_SOUNDMAIN_ENTRY;
            return;              /* skips the volume/pitch pass, as the GBA does */
        }
        s->status = trackBits;
        s->tempoC = (uint16_t)(s->tempoC - 150);
    }

    /* Apply the frame's parameter changes to the sounding channels. */
    for (int i = 0; i < s->trackCount; i++) {
        M4ASeqTrack *t = &s->tracks[i];
        if (!(t->flags & MPT_FLG_EXIST) || !(t->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG)))
            continue;
        for (int h = 0; h < CHAN_COUNT; h++)
            if (chan_in_chain(s->engine, h, i))
                s->frameCycles += M4A_CYC_TRACK_APPLY;
        m4a_engine_track_apply(s->engine, i,
                               (t->flags & MPT_FLG_VOLCHG) != 0,
                               (t->flags & MPT_FLG_PITCHG) != 0);
        t->flags &= 0xF0;
    }
    s->engine->cgbEntryCycles = s->frameCycles - M4A_CYC_SOUNDMAIN_ENTRY;
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

static void frame_hook(M4AEngine *engine, void *user)
{
    (void)engine;
    m4a_sequencer_frame((M4ASequencer *)user);
}

void m4a_sequencer_init(M4ASequencer *s, M4AEngine *engine)
{
    memset(s, 0, sizeof(*s));
    s->engine = engine;
    s->status = MUSICPLAYER_STATUS_PAUSE;
    s->loopTrack = -1;
    s->maxTracks = M4A_SEQ_DEFAULT_MAX_TRACKS;
    engine->frameHook = frame_hook;
    engine->frameHookUser = s;
    engine->externalSequencer = true;
}

void m4a_sequencer_detach(M4ASequencer *s)
{
    if (s->engine && s->engine->frameHookUser == s) {
        s->engine->frameHook = NULL;
        s->engine->frameHookUser = NULL;
        s->engine->externalSequencer = false;
    }
}

/* MPlayStart */
void m4a_sequencer_start(M4ASequencer *s, const M4ASong *song)
{
    M4AEngine *e = s->engine;

    s->song = song;
    s->status = 0;
    s->priority = song->priority;
    s->clock = 0;
    s->tempoD = 150;
    s->tempoI = 150;
    s->tempoU = 0x100;
    s->tempoC = 0;
    s->trackCount = song->trackCount > s->maxTracks ? s->maxTracks : song->trackCount;
    s->loopPasses = 0;
    s->loopTrack = -1;
    s->noteCount = 0;
    s->seqCounter = 0;
    memset(s->chanSeq, 0, sizeof(s->chanSeq));
    memset(s->chainOwner, 0, sizeof(s->chainOwner));

    int i = 0;
    for (; i < s->trackCount; i++) {
        track_stop(s, i);
        memset(&s->tracks[i], 0, sizeof(s->tracks[i]));
        s->tracks[i].flags = MPT_FLG_EXIST | MPT_FLG_START;
        s->tracks[i].cmdPtr = song->trackOffsets[i];
    }
    for (; i < M4A_SONG_MAX_TRACKS; i++) {
        track_stop(s, i);
        s->tracks[i].flags = 0;
    }

    if (song->reverb & 0x80)
        m4a_reverb_set_amount(&e->reverb, song->reverb & 0x7F);
}

/* m4aMPlayStop */
void m4a_sequencer_stop(M4ASequencer *s)
{
    s->status |= MUSICPLAYER_STATUS_PAUSE;
    for (int i = 0; i < M4A_SONG_MAX_TRACKS; i++)
        track_stop(s, i);
}
