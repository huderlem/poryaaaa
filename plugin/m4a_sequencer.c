/*
 * Song sequencer: port of MPlayMain and the ply_* handlers.  See
 * m4a_sequencer.h.  Comments name the GBA routine each piece comes from;
 * the structure follows m4a_1.s so it can be checked line by line.
 */

#include "m4a_sequencer.h"
#include "m4a_reverb.h"

#include <stdio.h>
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

/* The player's track i as the engine knows it (M4ASequencer.trackBase). */
static inline int etrk(const M4ASequencer *s, int i) { return s->trackBase + i; }

/* The track state the handlers read and write: the engine's track, or in
 * timing mode (m4a_sequencer_init_timing) the sequencer's own copy.  Every
 * channel access below goes the same way: the engine's channel, or the
 * sequencer's copies, whose statuses are the engine's as of the last SoundMain
 * plus what this MPlayMain has done to them since. */
static inline M4ATrack *seq_track(M4ASequencer *s, int i)
{
    return s->timingOnly ? &s->tmTracks[i] : &s->engine->tracks[s->trackBase + i];
}
#define ETRK(s, i) (*seq_track((M4ASequencer *)(s), (i)))

static inline uint8_t *chan_status(M4ASequencer *s, int h)
{
    M4AEngine *e = s->engine;
    if (s->timingOnly)
        return &s->tmStatus[h];
    return chan_is_pcm(h) ? &e->pcmChannels[h].status : &e->cgbChannels[h - TOTAL_PCM_CHANNELS].status;
}

static inline uint8_t *chan_gate_time(M4ASequencer *s, int h)
{
    M4AEngine *e = s->engine;
    if (s->timingOnly)
        return &s->tmGate[h];
    return chan_is_pcm(h) ? &e->pcmChannels[h].gateTime : &e->cgbChannels[h - TOTAL_PCM_CHANNELS].gateTime;
}

static inline int chan_track(const M4ASequencer *s, int h)
{
    const M4AEngine *e = s->engine;
    if (s->timingOnly)
        return s->tmTrack[h];
    return chan_is_pcm(h) ? e->pcmChannels[h].trackIndex : e->cgbChannels[h - TOTAL_PCM_CHANNELS].trackIndex;
}

/* channel->track = 0 (RealClearChain / TrackStop): the channel no longer
 * belongs to any track, so the track's later VOLCHG / PITCHG pass and
 * ply_fine leave it alone and ply_note's tie-break ranks it lowest. */
static inline void chan_clear_track(M4ASequencer *s, int h)
{
    M4AEngine *e = s->engine;
    if (s->timingOnly)
        s->tmTrack[h] = M4A_NO_TRACK;
    else if (chan_is_pcm(h))
        e->pcmChannels[h].trackIndex = M4A_NO_TRACK;
    else
        e->cgbChannels[h - TOTAL_PCM_CHANNELS].trackIndex = M4A_NO_TRACK;
}

static inline uint8_t chan_midi_key(const M4ASequencer *s, int h)
{
    const M4AEngine *e = s->engine;
    if (s->timingOnly)
        return s->tmMidiKey[h];
    return chan_is_pcm(h) ? e->pcmChannels[h].midiKey : e->cgbChannels[h - TOTAL_PCM_CHANNELS].midiKey;
}

static inline uint8_t chan_priority(const M4ASequencer *s, int h)
{
    const M4AEngine *e = s->engine;
    if (s->timingOnly)
        return s->tmPriority[h];
    return chan_is_pcm(h) ? e->pcmChannels[h].priority : e->cgbChannels[h - TOTAL_PCM_CHANNELS].priority;
}

/* A CGB channel's type (1..4, fixed by its index) and the key its frequency
 * was computed from (ply_note's key: the drum voice's own for a rhythm
 * voice). */
static inline uint8_t chan_cgb_type(const M4ASequencer *s, int h)
{
    if (s->timingOnly)
        return (uint8_t)((h - TOTAL_PCM_CHANNELS) % MAX_CGB_CHANNELS + 1);
    return s->engine->cgbChannels[h - TOTAL_PCM_CHANNELS].type;
}

static inline uint8_t chan_cgb_key(const M4ASequencer *s, int h)
{
    if (s->timingOnly)
        return s->tmKey[h];
    return s->engine->cgbChannels[h - TOTAL_PCM_CHANNELS].key;
}

/* Iterate the real (non-shadow) channel pools: the ones the GBA has. */
static inline bool chan_is_real(int h)
{
    return chan_is_pcm(h) ? h < MAX_PCM_CHANNELS : (h - TOTAL_PCM_CHANNELS) < MAX_CGB_CHANNELS;
}

/* A channel "belongs to the track's chain" while it is ON and owned by the
 * track.  (The GBA keeps an explicit linked list; dead channels drop out of
 * it via ClearChain, which this test reproduces.) */
static inline bool chan_in_chain(M4ASequencer *s, int h, int track)
{
    return chan_is_real(h) && (*chan_status(s, h) & CHN_ON) && chan_track(s, h) == track;
}

/* ------------------------------------------------------------------------
 * The GBA's per-track channel chain, for the timing model
 *
 * MusicPlayerTrack.chan heads a doubly linked list of the channels ply_note
 * started on the track, newest first (ply_note prepends).  A channel leaves
 * it through RealClearChain: when the tick's walk finds it dead, when
 * another note takes it over, or at ply_fine / TrackStop.  The engine keeps
 * no list; chainOwner[] (track + 1 while chained) and chanSeq[] (start
 * order) reproduce the membership and the order, which decide the cost of
 * each unlink (prev / next pointer stores) and the walk.
 * ------------------------------------------------------------------------ */

/* The chain of `track`, newest first.  Returns the count. */
static int chain_list(const M4ASequencer *s, int track, int out[CHAN_COUNT])
{
    int n = 0;
    for (int h = 0; h < CHAN_COUNT; h++)
        if (s->chainOwner[h] == track + 1)
            out[n++] = h;
    /* insertion sort by descending chanSeq (the chains are short) */
    for (int i = 1; i < n; i++) {
        int h = out[i], j = i;
        while (j > 0 && s->chanSeq[out[j - 1]] < s->chanSeq[h]) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = h;
    }
    return n;
}

/* RealClearChain's cost for channel h by its links (prev set when a newer
 * channel is still chained before it, next when an older one follows it);
 * the four measured values are passed in.  Drops h from its chain. */
static int unlink_cost(M4ASequencer *s, int h, int c00, int c01, int c10, int c11)
{
    int owner = s->chainOwner[h];
    bool prev = false, next = false;
    for (int k = 0; k < CHAN_COUNT; k++) {
        if (k == h || s->chainOwner[k] != owner)
            continue;
        if (s->chanSeq[k] > s->chanSeq[h]) prev = true;
        else next = true;
    }
    s->chainOwner[h] = 0;
    return prev ? (next ? c11 : c10) : (next ? c01 : c00);
}

/* The other chained player whose track still chains channel h (the GBA's
 * channel->track pointing into another MusicPlayerTrack array), or NULL:
 * a free channel, or one no chain holds any more. */
static M4ASequencer *chain_owner_of(M4ASequencer *s, int h)
{
    M4ASequencer *m = s;
    while (m->inner)
        m = m->inner;
    for (; m; m = m->outer)
        if (m != s && m->chainOwner[h] != 0)
            return m;
    return NULL;
}

/* Charge n cycles of MPlayMain work.  frameCycles counts from VBlank line
 * 160; the SoundMain stamp (line 160 + M4A_CYC_SOUNDMAIN_ENTRY) places it on
 * the CPU clock, where the sound FIFO's refill DMA stalls the CPU for 18
 * cycles once every 16 samples (m4a_gba_fifo_dma_stall). */
static inline void cyc(M4ASequencer *s, int n)
{
    int64_t base = s->engine->soundMainStamp - M4A_CYC_SOUNDMAIN_ENTRY;
    if (s->engine->soundMainStamp >= 0)
        n += (int)m4a_gba_fifo_dma_stall(&s->engine->gba, base + s->frameCycles, base + s->frameCycles + n);
    s->frameCycles += n;
}

/* Publish the frame's sum as the CgbSound entry (cycles from the SoundMain
 * entry) and trace it when asked: the chain's last member does, after the
 * members before it have written their per-command / per-track lines. */
static void publish_cycles(M4ASequencer *s)
{
    s->frameCount++;
    if (s->outer)
        return;
    s->engine->cgbEntryCycles = s->frameCycles - M4A_CYC_SOUNDMAIN_ENTRY;
    if (s->traceFile)   /* the ROM's first song frame is engine frame 3 */
        fprintf((FILE *)s->traceFile, "%u %d\n", s->frameCount + 1, s->frameCycles);
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

/* The timing model's M4A_CYC_* handler costs are ROM-song measurements; a
 * song whose data the ROM keeps in IWRAM (the cry song) saves
 * M4A_CYC_ROM_BYTE per song-data byte the handler reads (plan, "SoundMain
 * timing probe, step 5").  Negative: the correction for n reads. */
static inline int ram_bytes(const M4ASequencer *s, int n)
{
    return (s->song && s->song->dataInRam) ? -n * M4A_CYC_ROM_BYTE : 0;
}

/* ------------------------------------------------------------------------
 * Engine track helpers
 * ------------------------------------------------------------------------ */

static void set_engine_priority(M4ASequencer *s, int i)
{
    uint32_t p = (uint32_t)s->priority + s->tracks[i].priority;
    ETRK(s, i).priority = p > 0xFF ? 0xFF : (uint8_t)p;
}

/* TrackStop: silence and release every channel of the track. */
static void track_stop(M4ASequencer *s, int i)
{
    if (!(s->tracks[i].flags & MPT_FLG_EXIST))
        return;
    /* TrackStop walks track->chan: only the channels still chained to the
     * track are killed.  A channel ply_fine already unchained (its release
     * still running) keeps going, as on the GBA. */
    for (int h = 0; h < CHAN_COUNT; h++) {
        if (s->chainOwner[h] != i + 1)
            continue;
        s->chainOwner[h] = 0;                /* track->chan = 0, channel->track = 0 */
        if (!chan_is_real(h) || !*chan_status(s, h))
            continue;
        if (s->timingOnly)
            *chan_status(s, h) = 0;          /* the timing copy only */
        else
            m4a_engine_kill_channel(s->engine, h);   /* CgbOscOff for CGB channels */
    }
}

/* MPlayMain's MPT_FLG_START handling: Clear64byte(track) then the defaults.
 * The engine track mirrors MusicPlayerTrack's parameter fields. */
static void track_start(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    M4ATrack *et = &ETRK(s, i);
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
    et->sampleStart = 0;            /* unk_3C is inside Clear64byte's 64 bytes */
    et->currentProgram = 0;
    memset(&et->currentVoice, 0, sizeof(et->currentVoice));
    et->currentVoice.type = 1;
    set_engine_priority(s, i);
    m4a_track_vol_pit_set(et);
}

/* clear_modM */
static void clear_mod_m(M4ASequencer *s, int i)
{
    M4ATrack *et = &ETRK(s, i);
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
    /* RealClearChain on every chained channel, newest first: each unlink
     * sees prev == 0 (the one before it was just removed) and next set
     * except for the last, whose RealClearChain -> ply_fine_done is the
     * measured 50.  A chain of two or more at FINE is unmeasured: the
     * others are charged est. 50 too (the prev 0 / next set unlink is 4
     * cheaper than prev 0 / next 0 in the walk, and the loop's bne is then
     * taken, about 4 more). */
    int chain[CHAN_COUNT];
    int n = chain_list(s, i, chain);
    cyc(s, n == 0 ? M4A_CYC_FINE_EMPTY : M4A_CYC_FINE_LOOP);
    for (int k = 0; k < n; k++) {
        /* ply_fine_loop: SF_STOP if the channel is on (m4a_1.s 0xC7 test),
         * then RealClearChain: unlink and channel->track = 0.  The channel
         * keeps releasing on its own (SoundMainRAM / CgbSound only test
         * SF_ON), owned by no track. */
        int h = chain[k];
        uint8_t *st = chan_status(s, h);
        if (*st & CHN_ON)
            *st |= CHN_STOP;
        chan_clear_track(s, h);
        s->chainOwner[h] = 0;
        cyc(s, M4A_CYC_FINE_CHAN + M4A_CYC_FINE_UNLINK_CALL + M4A_CYC_FINE_UNLINK_LAST);
    }
    cyc(s, M4A_CYC_FINE_DONE);
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
    ETRK(s, i).keyShift = (int8_t)next_byte(s, &s->tracks[i]);
    s->tracks[i].flags |= MPT_FLG_PITCHG;
}

static void ply_voice(M4ASequencer *s, int i)
{
    uint8_t program = next_byte(s, &s->tracks[i]);
    if (!s->timingOnly) {
        m4a_engine_program_change_from(s->engine, etrk(s, i), s->voiceGroup, program);
        return;
    }
    /* m4a_engine_program_change_from on the timing copy's track */
    const ToneData *vg = s->voiceGroup ? s->voiceGroup : s->engine->voiceGroup;
    if (!vg || program > 127)
        return;
    s->tmTracks[i].currentProgram = program;
    s->tmTracks[i].currentVoice = vg[program];
}

static void ply_vol(M4ASequencer *s, int i)
{
    uint8_t v = next_byte(s, &s->tracks[i]);
    /* The master volume is baked into the song, so no songMasterVolume scaling. */
    ETRK(s, i).rawVolume = v;
    ETRK(s, i).volume = v;
    s->tracks[i].flags |= MPT_FLG_VOLCHG;
}

static void ply_pan(M4ASequencer *s, int i)
{
    ETRK(s, i).pan = (int8_t)(next_byte(s, &s->tracks[i]) - C_V);
    s->tracks[i].flags |= MPT_FLG_VOLCHG;
}

static void ply_bend(M4ASequencer *s, int i)
{
    ETRK(s, i).bend = (int8_t)(next_byte(s, &s->tracks[i]) - C_V);
    s->tracks[i].flags |= MPT_FLG_PITCHG;
}

static void ply_bendr(M4ASequencer *s, int i)
{
    ETRK(s, i).bendRange = next_byte(s, &s->tracks[i]);
    s->tracks[i].flags |= MPT_FLG_PITCHG;
}

static void ply_lfos(M4ASequencer *s, int i)
{
    uint8_t v = next_byte(s, &s->tracks[i]);
    ETRK(s, i).lfoSpeed = v;
    if (v == 0)
        clear_mod_m(s, i);
}

static void ply_lfodl(M4ASequencer *s, int i)
{
    ETRK(s, i).lfoDelay = next_byte(s, &s->tracks[i]);
}

static void ply_mod(M4ASequencer *s, int i)
{
    uint8_t v = next_byte(s, &s->tracks[i]);
    ETRK(s, i).mod = v;
    if (v == 0)
        clear_mod_m(s, i);
}

static void ply_modt(M4ASequencer *s, int i)
{
    uint8_t v = next_byte(s, &s->tracks[i]);
    if (ETRK(s, i).modT != v) {
        ETRK(s, i).modT = v;
        s->tracks[i].flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

static void ply_tune(M4ASequencer *s, int i)
{
    ETRK(s, i).tune = (int8_t)(next_byte(s, &s->tracks[i]) - C_V);
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
    case 0x0A: case 0x0B:                        /* xleng xswee */
        t->cmdPtr += 1;
        break;
    case 0x07:                                    /* xrele: track->tone.release, which the next
                                                   * ply_note copies into its channel (the cry
                                                   * song's SetPokemonCryRelease value) */
        ETRK(s, i).currentVoice.release = next_byte(s, t);
        break;
    case 0x08:                                    /* xiecv */
        if (s->timingOnly)
            s->tmTracks[i].pseudoEchoVolume = next_byte(s, t);
        else
            m4a_engine_xcmd(s->engine, etrk(s, i), M4A_XCMD_IECV, next_byte(s, t));
        break;
    case 0x09:                                    /* xiecl */
        if (s->timingOnly)
            s->tmTracks[i].pseudoEchoLength = next_byte(s, t);
        else
            m4a_engine_xcmd(s->engine, etrk(s, i), M4A_XCMD_IECL, next_byte(s, t));
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
    case 0x0D: {                                  /* ply_xcmd_0D (m4a.c): track->unk_3C, the
                                                   * sample offset ply_note starts the next
                                                   * DirectSound channel at (SetPokemonCryProgress) */
        uint32_t v = rd8(s, t->cmdPtr) | ((uint32_t)rd8(s, t->cmdPtr + 1) << 8)
                   | ((uint32_t)rd8(s, t->cmdPtr + 2) << 16) | ((uint32_t)rd8(s, t->cmdPtr + 3) << 24);
        ETRK(s, i).sampleStart = v;
        t->cmdPtr += 4;
        break;
    }
    default:                                      /* ply_xxx: gMPlayJumpTable[0] = ply_fine */
        ply_fine(s, i);
        break;
    }
}

/* ply_endtie */
static void ply_endtie(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
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
        if (!chan_in_chain(s, h, etrk(s, i)))
            continue;
        uint8_t st = *chan_status(s, h);
        if (!(st & (CHN_START | CHN_ENV_MASK)) || (st & CHN_STOP))
            continue;
        if (chan_midi_key(s, h) != key)
            continue;
        if (best < 0 || s->chanSeq[h] > bestSeq) {
            best = h;
            bestSeq = s->chanSeq[h];
        }
    }
    if (best >= 0)
        *chan_status(s, best) |= CHN_STOP;
}

/* TrkVolPitSet's cost for the track's pending flags (ply_note and the
 * VOLCHG/PITCHG pass share it). */
static int tvps_cost(const M4ASequencer *s, int i, uint8_t flags, bool inNote)
{
    const M4ATrack *et = &ETRK(s, i);
    int c = 0;
    if (flags & MPT_FLG_VOLSET)
        c += M4A_CYC_TVPS_VOLSET + (et->modT == 1 ? M4A_CYC_TVPS_VOLSET_MODT1 : 0);
    if (flags & MPT_FLG_PITSET)
        c += M4A_CYC_TVPS_PITSET + (inNote && et->modT != 0 ? M4A_CYC_TVPS_PITSET_MODT : 0);
    return c;
}

/* MidiKeyToCgbFreq's short path for a square / wave key at or below 35
 * (key + keyM as ply_note and the pitch pass compute it). */
static int cgb_freq_key_cost(const M4ASequencer *s, int i, int key, bool noise)
{
    int k = key + ETRK(s, i).keyM;
    if (k < 0)
        k = 0;
    if (noise)
        return k <= 20 ? M4A_CYC_NOISE_FREQ_LOW_KEY : k > 80 ? M4A_CYC_NOISE_FREQ_CLAMP : 0;
    return k <= 35 ? M4A_CYC_CGB_FREQ_LOW_KEY : 0;
}

/* ply_note's DirectSound channel search (_081DDBEC.._081DDC40), cycle by
 * cycle: the same walk as allocate_pcm_channel in the engine, over the
 * channel statuses as they are before the note.  Returns the channel it
 * would take (-1 for none) and adds the cost. */
static int scan_pcm_cost(M4ASequencer *s, int track, uint8_t priority)
{
    int n = s->engine->maxPcmChannels;
    bool stopping = false;          /* r2 */
    uint8_t bestPrio = priority;    /* r6 */
    int bestTrack = track;          /* r7 (the GBA compares track addresses) */
    int chosen = -1;                /* r8 */
    cyc(s, M4A_CYC_NOTE_SCAN_ENTER);
    for (int c = 0; c < n; c++) {
        uint8_t status = *chan_status(s, c);
        uint8_t chPriority = chan_priority(s, c);
        int chTrack = chan_track(s, c);
        if (!(status & CHN_ON)) {
            cyc(s, M4A_CYC_SCAN_FREE);
            return c;
        }
        bool compare;
        if (status & CHN_STOP) {
            if (!stopping) {
                cyc(s, M4A_CYC_SCAN_FIRST_STOP);
                stopping = true;
                bestPrio = chPriority;
                bestTrack = chTrack;
                chosen = c;
                compare = false;
            } else {
                cyc(s, M4A_CYC_SCAN_STOP);
                compare = true;
            }
        } else {
            cyc(s, M4A_CYC_SCAN_ON);
            if (stopping) {
                cyc(s, M4A_CYC_SCAN_ON_SKIP);
                compare = false;
            } else {
                cyc(s, M4A_CYC_SCAN_ON_CMP);
                compare = true;
            }
        }
        if (compare) {
            if (chPriority < bestPrio) {
                cyc(s, M4A_CYC_SCAN_LOWER);
                bestPrio = chPriority;
                bestTrack = chTrack;
                chosen = c;
            } else {
                cyc(s, M4A_CYC_SCAN_CMP);
                if (chPriority > bestPrio) {
                    cyc(s, M4A_CYC_SCAN_HIGHER);
                } else if (chTrack > bestTrack) {
                    cyc(s, M4A_CYC_SCAN_LATER_TRACK);
                    bestTrack = chTrack;
                    chosen = c;
                } else if (chTrack == bestTrack) {
                    cyc(s, M4A_CYC_SCAN_SAME_TRACK);
                    chosen = c;
                } else {
                    cyc(s, M4A_CYC_SCAN_EARLIER_TRACK);
                }
            }
        }
        cyc(s, c + 1 < n ? M4A_CYC_SCAN_NEXT : (chosen >= 0 ? M4A_CYC_SCAN_DONE : M4A_CYC_SCAN_NONE));
    }
    return chosen;
}

/* ply_note: cmd is the note length index (0 = TIE). */
static void ply_note(M4ASequencer *s, int i, uint8_t cmd)
{
    M4ASeqTrack *t = &s->tracks[i];
    M4AEngine *e = s->engine;
    M4ATrack *et = &ETRK(s, i);
    t->gateTime = gClockTable[cmd];

    int entry = M4A_CYC_NOTE_ENTRY;
    int reads = 1;                      /* each test of the next byte is a song-data read */
    uint8_t b = rd8(s, t->cmdPtr);
    if (b < 0x80) {
        t->key = b;
        t->cmdPtr++;
        entry += M4A_CYC_NOTE_KEY_BYTE;
        reads++;
        b = rd8(s, t->cmdPtr);
        if (b < 0x80) {
            t->velocity = b;
            t->cmdPtr++;
            entry += M4A_CYC_NOTE_VEL_BYTE;
            reads++;
            b = rd8(s, t->cmdPtr);
            if (b < 0x80) {
                t->gateTime = (uint8_t)(t->gateTime + b);
                t->cmdPtr++;
                entry += M4A_CYC_NOTE_GATE_BYTE;
            }
        }
    }
    cyc(s, entry + ram_bytes(s, reads));

    /* The work ply_note does before and after the engine's note-on, costed
     * from the state the GBA tests at each branch.  Voice resolution: */
    uint8_t trackType = et->currentVoice.type;
    bool romVoice = (trackType & (VOICE_KEYSPLIT | VOICE_KEYSPLIT_ALL)) != 0;
    const ToneData *voice = m4a_engine_resolve_voice(&et->currentVoice, t->key);
    if (!voice) {
        cyc(s, M4A_CYC_NOTE_VOICE_BAD + M4A_CYC_NOTE_EXIT);
        if (!s->timingOnly)
            (void)m4a_engine_note_on(e, etrk(s, i), t->key, t->velocity);
        return;
    }
    cyc(s, (trackType & VOICE_KEYSPLIT_ALL) ? M4A_CYC_NOTE_VOICE_DRUM
         : (trackType & VOICE_KEYSPLIT) ? M4A_CYC_NOTE_VOICE_SPLIT : M4A_CYC_NOTE_VOICE_PLAIN);
    uint8_t type = voice->type & VOICE_TYPE_CGB_MASK;
    int noteKey = (trackType & VOICE_KEYSPLIT_ALL) ? voice->key : t->key;
    uint8_t priority = et->priority;
    uint8_t flags = t->flags;                 /* what TrkVolPitSet will see */
    int expect;                               /* the channel the GBA takes */
    if (type != 0) {
        cyc(s, M4A_CYC_NOTE_CGB_PATH);
        expect = TOTAL_PCM_CHANNELS + type - 1;
        uint8_t chStatus = *chan_status(s, expect);
        uint8_t chPriority = chan_priority(s, expect);
        if (!(chStatus & CHN_ON)) {
            cyc(s, M4A_CYC_NOTE_CGB_FREE);
        } else if (chStatus & CHN_STOP) {
            cyc(s, M4A_CYC_NOTE_CGB_STOPPING);
        } else if (chPriority < priority) {
            cyc(s, M4A_CYC_NOTE_CGB_PRIO);
        } else if (chPriority > priority) {
            cyc(s, M4A_CYC_NOTE_CGB_REJECT_PRIO + M4A_CYC_NOTE_EXIT);
            expect = -1;
        } else if (chan_track(s, expect) >= etrk(s, i)) {
            cyc(s, M4A_CYC_NOTE_CGB_TRACK);
        } else {
            cyc(s, M4A_CYC_NOTE_CGB_REJECT_TRACK + M4A_CYC_NOTE_EXIT);
            expect = -1;
        }
    } else {
        cyc(s, M4A_CYC_NOTE_PCM_PATH + (romVoice ? M4A_CYC_NOTE_ROM_VOICE_PATH : 0));
        expect = scan_pcm_cost(s, etrk(s, i), priority);
        if (expect < 0)
            cyc(s, M4A_CYC_NOTE_EXIT);
    }

    /* Voice resolution, priority, channel allocation and the note start are
     * the engine's note-on (see m4a_engine_note_on for the correspondence).
     * It returns the channel it started, if any. */
    int h;
    if (s->timingOnly) {
        /* No engine note: the channel the GBA's walk took, in the timing
         * copy, as ply_note leaves it (SF_START, the track's priority,
         * channel->track, the keys). */
        /* m4a_engine_note_on's work on the track, which the --song path's
         * costs read later: TrkVolPitSet for every resolved note, and for
         * a note that gets a channel ply_note's LFO-delay restart (the
         * externalSequencer form of note_on_restart_lfo_delay). */
        m4a_track_vol_pit_set(et);
        h = expect;
        if (h < 0)
            return;
        et->lfoDelayC = et->lfoDelay;
        if (et->lfoDelay != 0) {
            et->modM = 0;
            et->lfoSpeedC = 0;
            m4a_track_vol_pit_set(et);
        }
        s->tmStatus[h] = CHN_START;
        s->tmPriority[h] = priority;
        s->tmTrack[h] = (int8_t)etrk(s, i);
        s->tmMidiKey[h] = t->key;
        s->tmKey[h] = (uint8_t)noteKey;
    } else {
        h = m4a_engine_note_on(e, etrk(s, i), t->key, t->velocity);
    }
    if (h < 0)
        return;
    /* A note the GBA rejected (expect < 0) was charged its exit above; the
     * polyphony-debug invert mode may still have played it on a shadow
     * channel, which the GBA's accounting never sees.  (expect >= 0 with a
     * different h would be an allocation bug in one of the two walks; the
     * cost follows the GBA's walk above either way.) */
    if (expect < 0)
        return;

    /* ClearChain(channel) and the prepend to this track's chain.  The GBA
     * unlinks the channel from whichever track still chains it, through
     * channel->track, with that chain's prev / next (RealClearChain): a
     * channel of this player's own track as before, and a channel another
     * chained player's track holds -- the cross-player steal of plan step
     * 12 (a cry over music, frame 62: the cry's ply_note takes a releasing
     * BGM channel, NOTE_UNLINK_11 66 + CHAIN_EMPTY 4 = 70) -- is unlinked
     * from that owner's chain at once, so the owner's next walk sees one
     * channel fewer and never meets it as dead.  The same-player path is
     * literally the one before. */
    cyc(s, M4A_CYC_NOTE_TAKE);
    int unlink;
    if (s->chainOwner[h] == 0) {
        M4ASequencer *owner = chain_owner_of(s, h);
        if (owner)
            unlink = unlink_cost(owner, h, M4A_CYC_NOTE_UNLINK_00, M4A_CYC_NOTE_UNLINK_01,
                                 M4A_CYC_NOTE_UNLINK_10, M4A_CYC_NOTE_UNLINK_11);
        else
            unlink = M4A_CYC_NOTE_UNLINK_NONE;
    } else {
        unlink = unlink_cost(s, h, M4A_CYC_NOTE_UNLINK_00, M4A_CYC_NOTE_UNLINK_01,
                             M4A_CYC_NOTE_UNLINK_10, M4A_CYC_NOTE_UNLINK_11);
    }
    bool chainEmpty = true;
    for (int k = 0; k < CHAN_COUNT && chainEmpty; k++)
        if (s->chainOwner[k] == i + 1)
            chainEmpty = false;
    cyc(s, unlink + (chainEmpty ? M4A_CYC_NOTE_CHAIN_EMPTY : 0));
    if (et->lfoDelay == 0) {
        cyc(s, M4A_CYC_NOTE_LFODL0);
    } else {
        cyc(s, M4A_CYC_NOTE_LFODL);
        flags |= (et->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;   /* clear_modM */
    }
    cyc(s, M4A_CYC_NOTE_TVPS + tvps_cost(s, i, flags, true) + (romVoice ? M4A_CYC_TVPS_ROM_VOICE : 0));
    cyc(s, M4A_CYC_NOTE_CHNVOL);
    if (type == 0)
        cyc(s, M4A_CYC_NOTE_PCM_FREQ);
    else
        cyc(s, (type == 4 ? M4A_CYC_NOTE_NOISE_FREQ : M4A_CYC_NOTE_CGB_FREQ) + cgb_freq_key_cost(s, i, noteKey, type == 4)
               + (romVoice ? M4A_CYC_NOTE_CGB_ROM_VOICE : 0)
               + ((!(voice->panSweep & 0x80) && (voice->panSweep & 0x70)) ? M4A_CYC_NOTE_CGB_SWEEP : 0));
    cyc(s, M4A_CYC_NOTE_STORE + M4A_CYC_NOTE_EXIT);

    *chan_gate_time(s, h) = t->gateTime;
    s->chanSeq[h] = ++s->seqCounter;
    s->chainOwner[h] = (uint8_t)(i + 1);
    s->noteCount++;

    /* ply_note ran TrkVolPitSet for the new note; the pending flags are
     * dropped, so older channels wait for the next change. */
    t->flags &= 0xF0;
}

/* A handler's cost from its entry back to _081DD938 (the handler, its
 * return and MPlayMain's flags test); the table dispatch before it is
 * M4A_CYC_CMD_TABLE.  Unmeasured handlers take the default. */
static int xcmd_cycles(const M4ASequencer *s, const M4ASeqTrack *t)
{
    uint8_t sub = rd8(s, t->cmdPtr);
    switch (sub) {
    case 0x08: case 0x09:                         /* xiecv xiecl */
        return M4A_CYC_XCMD_SUB + M4A_CYC_XIEC + ram_bytes(s, 2);
    case 0x07:                                    /* xrele */
        return M4A_CYC_XCMD_SUB + M4A_CYC_XRELE + ram_bytes(s, 2);
    case 0x0D:                                    /* xcmd_0D */
        return M4A_CYC_XCMD_SUB + M4A_CYC_XCMD_0D + ram_bytes(s, 5);
    case 0x0C: {                                  /* xwait: the tick it expires on is cheaper */
        uint16_t len = (uint16_t)(rd8(s, t->cmdPtr + 1) | (rd8(s, t->cmdPtr + 2) << 8));
        return M4A_CYC_XCMD_SUB + (t->timer < len ? M4A_CYC_XWAIT : M4A_CYC_XWAIT_DONE) + ram_bytes(s, 3);
    }
    case 0x01:                                    /* xwave (est.: as xiec with four bytes) */
        return M4A_CYC_XCMD_SUB + M4A_CYC_XIEC + ram_bytes(s, 5);
    case 0x00: case 0x03:                         /* ply_xxx = gMPlayJumpTable[0] = ply_fine, which charges itself */
        return M4A_CYC_XCMD_SUB + ram_bytes(s, 1);
    default:                                      /* xtype xatta xdeca xsust xleng xswee (est.: as xiec) */
        return M4A_CYC_XCMD_SUB + M4A_CYC_XIEC + ram_bytes(s, 2);
    }
}

/* ply_endtie's walk over the track's chain, newest first (m4a_1.s
 * _081DDD22): stops at the first channel that is ON, not STOP and holds
 * the key.  Costed before the handler runs, from the same state. */
static int endtie_cycles(M4ASequencer *s, int i)
{
    const M4ASeqTrack *t = &s->tracks[i];
    uint8_t b = rd8(s, t->cmdPtr);
    bool keyByte = b < 0x80;
    uint8_t key = keyByte ? b : t->key;
    int c = M4A_CYC_ENDTIE + (keyByte ? M4A_CYC_ENDTIE_KEY : 0) + ram_bytes(s, 1);
    int chain[CHAN_COUNT];
    int n = chain_list(s, i, chain);
    for (int k = 0; k < n; k++) {
        int h = chain[k];
        uint8_t st = *chan_status(s, h);
        uint8_t midiKey = chan_midi_key(s, h);
        if ((st & (CHN_START | CHN_ENV_MASK)) && !(st & CHN_STOP) && midiKey == key) {
            c += M4A_CYC_ENDTIE_MATCH;
            break;
        }
        c += M4A_CYC_ENDTIE_SKIP;
    }
    return c;
}

static int cmd_cycles(M4ASequencer *s, int i, uint8_t cmd)
{
    const M4ASeqTrack *t = &s->tracks[i];
    const M4ATrack *et = &ETRK(s, i);
    /* ROM-song measurements (step 3); ram_bytes() takes off the song-data
     * reads for a song in IWRAM (step 5: measured on tune, goto, voice, vol,
     * pan, xcmd and TIE; the others follow the same per-byte rule, est.). */
    switch (cmd) {
    case 0xB2: return 110 + ram_bytes(s, 4);   /* goto */
    case 0xB3: return 140 + ram_bytes(s, 4);   /* patt (30 + goto) */
    case 0xB4: return t->patternLevel != 0 ? 40 : 34;   /* pend 18 + 22; level 0 (the taken beq) 12 + 22 */
    case 0xB5: return M4A_CYC_CMD_DEFAULT + ram_bytes(s, 1);   /* rept */
    case 0xB9: return M4A_CYC_CMD_DEFAULT + ram_bytes(s, 3);   /* memacc */
    case 0xBB: return 91 + ram_bytes(s, 1);    /* tempo */
    case 0xBC: return 89 + ram_bytes(s, 1);    /* keysh */
    case 0xBD: return 184 + ram_bytes(s, 1);   /* voice */
    case 0xBE: return 89 + ram_bytes(s, 1);    /* vol */
    case 0xBF: return 91 + ram_bytes(s, 1);    /* pan */
    case 0xC0: return 91 + ram_bytes(s, 1);    /* bend */
    case 0xC1: return 89 + ram_bytes(s, 1);    /* bendr */
    case 0xC2: return (rd8(s, t->cmdPtr) != 0 ? 69 : 109) + ram_bytes(s, 1);   /* lfos; value 0 branches to clear_modM like mod 0 (est.: the same 51 + 58) */
    case 0xC3: return 81 + ram_bytes(s, 1);    /* lfodl */
    case 0xC4: return (rd8(s, t->cmdPtr) != 0 ? 69 : 109) + ram_bytes(s, 1);   /* mod; 51 + clear_modM 58 for 0 */
    case 0xC5: return (rd8(s, t->cmdPtr) != et->modT ? 95 : 91) + ram_bytes(s, 1);   /* modt */
    case 0xC8: return 91 + ram_bytes(s, 1);    /* tune: 84 measured on the cry (RAM, step 5); ROM est. 84 + one byte */
    case 0xCC: return M4A_CYC_CMD_DEFAULT + ram_bytes(s, 2);   /* port */
    case 0xCD: return xcmd_cycles(s, t);       /* xcmd */
    case 0xCE: return endtie_cycles(s, i);     /* endtie */
    case 0xB1: return 0;     /* fine: charged by ply_fine itself (M4A_CYC_FINE_*: 44 with an empty chain, 114 with one channel) */
    default:   return M4A_CYC_CMD_DEFAULT + ram_bytes(s, 1);   /* prio */
    }
}

/* gMPlayJumpTable, indexed by command - 0xB1, after m4aSoundInit's patches. */
static void dispatch(M4ASequencer *s, int i, uint8_t cmd)
{
    cyc(s, M4A_CYC_CMD_TABLE);
    if (s->traceFile)   /* the handler's entry, for probe comparison */
        fprintf((FILE *)s->traceFile, "  c%d %02X %d\n", etrk(s, i), cmd, s->frameCycles);
    cyc(s, cmd_cycles(s, i, cmd));
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

/* The per-tick LFO step (MPlayMain, after the wait countdown), with the
 * cost of each branch from _081DD938 to _081DD994. */
static void lfo_step(M4ASequencer *s, int i)
{
    M4ATrack *et = &ETRK(s, i);
    if (et->lfoSpeed == 0) {
        cyc(s, M4A_CYC_WAIT_LFOS0);
        return;
    }
    if (et->mod == 0) {
        cyc(s, M4A_CYC_WAIT_NO_LFO);
        return;
    }
    if (et->lfoDelayC != 0) {
        et->lfoDelayC--;
        cyc(s, M4A_CYC_WAIT_LFO_DELAY);
        return;
    }
    cyc(s, M4A_CYC_LFO_STEP);
    /* `adds r0, r1; strb r0, lfoSpeedC; adds r1, r0, 0`: the stored
     * accumulator is the low byte, but the rising half computes 0x80 - the
     * untruncated sum (up to 510), which differs once lfoSpeed >= 0x41. */
    int32_t sum = (int32_t)et->lfoSpeedC + et->lfoSpeed;
    et->lfoSpeedC = (uint8_t)sum;
    int32_t r;
    if ((int8_t)(et->lfoSpeedC - 0x40) < 0) {
        r = (int8_t)et->lfoSpeedC;
        cyc(s, M4A_CYC_LFO_NEG);
    } else {
        r = 0x80 - sum;
        cyc(s, M4A_CYC_LFO_POS);
    }
    r = (et->mod * r) >> 6;
    if ((uint8_t)(et->modM ^ r) != 0) {
        et->modM = (int8_t)r;
        s->tracks[i].flags |= (et->modT == 0) ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
        cyc(s, et->modT == 0 ? M4A_CYC_LFO_PIT : M4A_CYC_LFO_VOL);
    } else {
        cyc(s, M4A_CYC_LFO_SAME);
    }
}

/* One song tick for one existing track (_081DD886 .. _081DD994): the
 * chain walk, START init, command loop. */
static void track_tick(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];

    /* Walk the track's chain, newest first: count down gate times; channels
     * that have died drop out (ClearChain), which changes the links of
     * the ones after them. */
    int chain[CHAN_COUNT];
    int n = chain_list(s, i, chain);
    cyc(s, n ? M4A_CYC_TICK_CHAIN : M4A_CYC_TICK_NO_CHAIN);
    for (int k = 0; k < n; k++) {
        int h = chain[k];
        if (chan_in_chain(s, h, etrk(s, i))) {
            uint8_t *gate = chan_gate_time(s, h);
            if (*gate == 0) {
                cyc(s, M4A_CYC_WALK_GATE0);
            } else if (--*gate == 0) {
                *chan_status(s, h) |= CHN_STOP;
                cyc(s, M4A_CYC_WALK_GATE_EXPIRE);
            } else {
                cyc(s, M4A_CYC_WALK_GATE_DEC);
            }
        } else {
            cyc(s, M4A_CYC_WALK_DEAD + unlink_cost(s, h, M4A_CYC_WALK_UNLINK_00, M4A_CYC_WALK_UNLINK_01,
                                                   M4A_CYC_WALK_UNLINK_10, M4A_CYC_WALK_UNLINK_11));
        }
        cyc(s, k + 1 < n ? M4A_CYC_WALK_NEXT : M4A_CYC_WALK_END);
    }

    if (t->flags & MPT_FLG_START) {
        cyc(s, M4A_CYC_TICK_START);
        track_start(s, i);
    } else {
        cyc(s, M4A_CYC_TICK_NO_START);
    }

    for (;;) {
        if (t->wait != 0) {
            t->wait--;
            lfo_step(s, i);
            return;
        }

        cyc(s, M4A_CYC_CMD_FETCH);
        uint8_t c = rd8(s, t->cmdPtr);   /* _081DD8E0's ldrb: the one song-data read of this segment */
        if (c < 0x80) {
            cyc(s, M4A_CYC_CMD_RUNNING + ram_bytes(s, 1));
            c = t->runningStatus;
            if (c < 0x80) {
                /* A data byte before any command: the GBA indexes
                 * gClockTable out of bounds and hangs on the same byte. */
                ply_fine(s, i);
                return;
            }
        } else {
            t->cmdPtr++;
            cyc(s, M4A_CYC_CMD_BYTE + ram_bytes(s, 1));
            if (c >= 0xBD) {
                t->runningStatus = c;
                cyc(s, M4A_CYC_CMD_BYTE_RS);
            } else {
                cyc(s, M4A_CYC_CMD_BYTE_LOW);
            }
        }

        if (c >= 0xCF) {
            cyc(s, M4A_CYC_CMD_NOTE);
            if (s->traceFile)
                fprintf((FILE *)s->traceFile, "  c%d %02X %d\n", etrk(s, i), c, s->frameCycles);
            ply_note(s, i, (uint8_t)(c - 0xCF));
        } else if (c > 0xB0) {
            dispatch(s, i, c);
            if (t->flags == 0)
                return;          /* FINE: no LFO step this tick */
        } else {
            t->wait = gClockTable[c - 0x80];
            cyc(s, M4A_CYC_CMD_WAIT);
        }
    }
}

/* The VOLCHG/PITCHG pass for one flagged track (_081DD9C8 .. _081DDA58). */
static void track_apply(M4ASequencer *s, int i)
{
    M4ASeqTrack *t = &s->tracks[i];
    bool vol = (t->flags & MPT_FLG_VOLCHG) != 0;
    bool pit = (t->flags & MPT_FLG_PITCHG) != 0;

    cyc(s, M4A_CYC_APPLY_FLAGGED + M4A_CYC_APPLY_TVPS + tvps_cost(s, i, t->flags, false));
    /* TrkVolPitSet runs before the channel loop, so the key tests below
     * see the new keyM; the engine's apply does the same work. */
    if (s->timingOnly)
        m4a_track_vol_pit_set(seq_track(s, i));   /* no engine channel to update */
    else
        m4a_engine_track_apply(s->engine, etrk(s, i), vol, pit);
    int chain[CHAN_COUNT];
    int n = chain_list(s, i, chain);
    if (n == 0) {
        cyc(s, M4A_CYC_APPLY_TVPS_EMPTY);
    } else {
        for (int k = 0; k < n; k++) {
            int h = chain[k];
            if (!chan_in_chain(s, h, etrk(s, i))) {
                cyc(s, M4A_CYC_APPLY_CH_DEAD);
                s->chainOwner[h] = 0;
                continue;
            }
            bool isCgb = !chan_is_pcm(h);
            bool isNoise = isCgb && chan_cgb_type(s, h) == 4;
            int keyCost = isCgb ? cgb_freq_key_cost(s, i, chan_cgb_key(s, h), isNoise) : 0;
            cyc(s, M4A_CYC_APPLY_CH);
            cyc(s, vol ? M4A_CYC_APPLY_CH_VOL + (isCgb ? M4A_CYC_APPLY_CH_VOL_CGB : M4A_CYC_APPLY_CH_VOL_PCM)
                       : M4A_CYC_APPLY_CH_NOVOL);
            cyc(s, pit ? M4A_CYC_APPLY_CH_PIT + (isNoise ? M4A_CYC_APPLY_CH_PIT_NOISE + keyCost
                                                : isCgb ? M4A_CYC_APPLY_CH_PIT_CGB + keyCost : M4A_CYC_APPLY_CH_PIT_PCM)
                       : M4A_CYC_APPLY_CH_NOPIT);
            cyc(s, k + 1 < n ? M4A_CYC_APPLY_CH_NEXT : M4A_CYC_APPLY_CH_LAST);
        }
    }
    cyc(s, M4A_CYC_APPLY_TRACK_END);
    t->flags &= 0xF0;
}

/* MPlayMain's body for this player, from where the deeper player left
 * off (frameCycles already positioned) to its own _081DDA6C. */
static void frame_body(M4ASequencer *s)
{
    if (!s->song || (s->status & MUSICPLAYER_STATUS_PAUSE)) {
        cyc(s, s->mpPaused);
        return;
    }
    cyc(s, s->mpHead);

    /* FadeOutBody: no fades are started by this host, so nothing to do. */

    int nTracks = s->maxTracks;      /* MusicPlayerInfo.trackCount: both loops run over all of them */
    s->tempoC = (uint16_t)(s->tempoC + s->tempoI);
    while (s->tempoC >= 150) {
        uint32_t trackBits = 0;
        cyc(s, M4A_CYC_TICK_ENTER);
        for (int i = 0; i < nTracks; i++) {
            if (s->tracks[i].flags & MPT_FLG_EXIST) {
                cyc(s, M4A_CYC_TICK_TRACK);
                trackBits |= 1u << i;
                track_tick(s, i);
                cyc(s, M4A_CYC_TICK_TRACK_END);
            } else {
                cyc(s, M4A_CYC_TICK_NO_TRACK);
            }
            if (s->traceFile)   /* _081DD998 of track i, for probe comparison */
                fprintf((FILE *)s->traceFile, "  t%d %d\n", etrk(s, i), s->frameCycles);
            cyc(s, i + 1 < nTracks ? M4A_CYC_TICK_NEXT : M4A_CYC_TICK_LAST);
        }
        s->clock++;
        if (trackBits == 0) {
            s->status = MUSICPLAYER_STATUS_PAUSE;
            cyc(s, M4A_CYC_TICK_ALL_DONE);
            return;              /* skips the volume/pitch pass, as the GBA does */
        }
        s->status = trackBits;
        s->tempoC = (uint16_t)(s->tempoC - 150);
        cyc(s, M4A_CYC_TICK_EXIT);
    }

    /* Apply the frame's parameter changes to the sounding channels. */
    cyc(s, M4A_CYC_APPLY_ENTER);
    for (int i = 0; i < nTracks; i++) {
        M4ASeqTrack *t = &s->tracks[i];
        if (!(t->flags & MPT_FLG_EXIST))
            cyc(s, M4A_CYC_APPLY_NO_TRACK);
        else if (!(t->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG)))
            cyc(s, M4A_CYC_APPLY_NO_FLAGS);
        else
            track_apply(s, i);
        cyc(s, i + 1 < nTracks ? M4A_CYC_APPLY_NEXT : M4A_CYC_APPLY_LAST);
    }
}

void m4a_sequencer_frame(M4ASequencer *s)
{
    /* SoundMain timing model: the CgbSound entry follows MPlayMain's work.
     * Costs are the probe measurements in m4a_engine.h, summed along
     * MPlayMain's control flow; the sum is published as cycles from the
     * SoundMain entry.  A lone player: its entry (the deeper players'
     * paused costs), its body, its exit (the outer players' idle costs and
     * the tail).  A chain (plan, "Engine: two cry players"): the deepest
     * member's entry, each member's body in MPlayOpen order from the
     * previous member's _081DDA6C -- a playing member costs its head
     * (M4A_CYC_MP_SE_HEAD 142) and body, a paused one its idle 63 -- and
     * the last member's exit; the one-cry 118 = 63 + 55 is the same sum
     * split, so a lone player's frame is unchanged. */
    s->frameCycles = s->mpEntry;
    frame_body(s);
    M4ASequencer *last = s;
    for (M4ASequencer *o = s->outer; o; o = o->outer) {
        o->frameCycles = last->frameCycles;
        /* players between the two in the chain, none of them attached:
         * paused, M4A_CYC_MP_IDLE each (none between the cry players; the
         * three SE players between BGM and cry 0, SE2 / SE3 between SE1
         * and cry 0 -- plan step 12's 1007 + (79 + body) per playing
         * player) */
        cyc(o, (o->player - last->player - 1) * M4A_CYC_MP_IDLE);
        frame_body(o);
        publish_cycles(last);
        last = o;
    }
    cyc(last, last->mpExit);
    publish_cycles(last);
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

static void frame_hook(M4AEngine *engine, void *user)
{
    (void)engine;
    m4a_sequencer_frame((M4ASequencer *)user);
}

/* The timing copy's channel statuses from the engine's real channels, at the end
 * of a SoundMain: what the next MPlayMain finds (the ROM's channels die in
 * CgbSound / SoundMainRAM, before it). */
static void timing_snapshot(M4AEngine *engine, void *user)
{
    M4ASequencer *s = (M4ASequencer *)user;
    for (int h = 0; h < CHAN_COUNT; h++)
        s->tmStatus[h] = chan_is_real(h)
            ? (chan_is_pcm(h) ? engine->pcmChannels[h].status
                              : engine->cgbChannels[h - TOTAL_PCM_CHANNELS].status)
            : 0;
}

void m4a_sequencer_init_timing(M4ASequencer *s, M4AEngine *engine)
{
    memset(s, 0, sizeof(*s));
    s->engine = engine;
    s->status = MUSICPLAYER_STATUS_PAUSE;
    s->loopTrack = -1;
    s->timingOnly = true;
    for (int h = 0; h < CHAN_COUNT; h++)
        s->tmTrack[h] = M4A_NO_TRACK;
    m4a_sequencer_set_player(s, M4A_PLAYER_BGM);
    /* The trace numbers frames as the ROM's engine frames: attached after
     * the boot, frame 2's SoundMain (whose MPlayMain a sequencer attached
     * before it counts) has already run. */
    s->frameCount = engine->booted ? 1 : 0;
    engine->frameHook = frame_hook;
    engine->frameHookUser = s;
    engine->frameEndHook = timing_snapshot;
    engine->frameEndHookUser = s;
    timing_snapshot(engine, s);
}

void m4a_sequencer_init(M4ASequencer *s, M4AEngine *engine)
{
    memset(s, 0, sizeof(*s));
    s->engine = engine;
    s->status = MUSICPLAYER_STATUS_PAUSE;
    s->loopTrack = -1;
    m4a_sequencer_set_player(s, M4A_PLAYER_BGM);
    engine->frameHook = frame_hook;
    engine->frameHookUser = s;
    engine->externalSequencer = true;
}

int m4a_sequencer_init_chained(M4ASequencer *s, M4ASequencer *inner, int trackBase)
{
    /* the next player up the chain (cry 1 after cry 0) */
    if (!inner || inner->player + 1 >= M4A_PLAYER_COUNT)
        return -1;      /* nothing is opened after cry player 1 */
    return m4a_sequencer_init_chained_player(s, inner, trackBase, inner->player + 1);
}

int m4a_sequencer_init_chained_player(M4ASequencer *s, M4ASequencer *inner, int trackBase, int player)
{
    if (!inner || inner->outer || trackBase < inner->trackBase + inner->maxTracks
        || trackBase >= MAX_TRACKS)
        return -1;
    memset(s, 0, sizeof(*s));
    s->engine = inner->engine;
    s->status = MUSICPLAYER_STATUS_PAUSE;
    s->loopTrack = -1;
    s->trackBase = (uint8_t)trackBase;
    s->inner = inner;
    s->frameCount = inner->frameCount;
    inner->outer = s;
    /* set_player checks the chain order and the track range; the players
     * skipped between inner and this one stay paused, their M4A_CYC_MP_IDLE
     * each charged by m4a_sequencer_frame */
    if (m4a_sequencer_set_player(s, player) != 0) {
        inner->outer = NULL;
        return -1;
    }
    return 0;
}

void m4a_sequencer_set_voicegroup(M4ASequencer *s, const ToneData *voiceGroup)
{
    s->voiceGroup = voiceGroup;
}

bool m4a_sequencer_player_free(const M4ASequencer *s)
{
    /* SetPokemonCryTone (m4a.c): track->flags == 0 and (track->chan == 0
     * or chan->track != track).  track->chan is the chain head: the
     * newest channel chainOwner still holds for track 0. */
    if (s->tracks[0].flags != 0)
        return false;
    int chain[CHAN_COUNT];
    int n = chain_list(s, 0, chain);
    return n == 0 || chan_track(s, chain[0]) != etrk(s, 0);
}

int m4a_sequencer_set_player(M4ASequencer *s, int player)
{
    /* NUM_TRACKS_BGM / SE1 / SE2 / SE3 in pokeemerald sound/music_player_table.inc;
     * the cry players' 2 is MPlayOpen's argument in m4aSoundInit (m4a.c). */
    static const uint8_t trackCounts[M4A_PLAYER_COUNT] = { 10, 3, 9, 1, 2, 2 };
    if (player < 0 || player >= M4A_PLAYER_COUNT)
        return -1;
    if ((s->inner && player <= s->inner->player) || (s->outer && player >= s->outer->player)
        || s->trackBase + trackCounts[player] > MAX_TRACKS
        || (s->outer && s->trackBase + trackCounts[player] > s->outer->trackBase))
        return -1;      /* an inner player's tracks must stay below its outer's */
    s->player = (uint8_t)player;
    s->maxTracks = trackCounts[player];
    /* Chain order (MPlayOpen appends to the head; MPlayMain calls the next
     * player before its own body): BGM runs first at its entry 566, then
     * each outer player resumes at the deeper player's _081DDA6C.  With the
     * other players paused, the deeper ones cost M4A_CYC_MP_PAUSED (BGM) and
     * M4A_CYC_MP_IDLE each, and the outer ones unwind M4A_CYC_MP_IDLE each
     * before the M4A_CYC_MP_TAIL to CgbSound.  Measured (plan, "milestone 4
     * slice 1"): _081DD9BC at 716 / 779 / 842 / 905, _081DDA6C -> CgbSound
     * 370 / 307 / 244 / 181 for BGM / SE1 / SE2 / SE3; every paused frame
     * 1007 whichever player is selected.  Cry 0 (step 5, four cries): body
     * at SE3's _081DDA6C 826, _081DD9BC 968, exit 118; cry 1 by the same
     * rule (unmeasured): 889 / 1031 / 55. */
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
    return 0;
}

void m4a_sequencer_detach(M4ASequencer *s)
{
    if (s->inner) {
        s->inner->outer = s->outer;
        if (s->outer)
            s->outer->inner = s->inner;
        s->inner = s->outer = NULL;
        return;
    }
    if (s->outer) {
        s->outer->inner = NULL;
        s->outer = NULL;
    }
    if (s->engine && s->engine->frameHookUser == s) {
        s->engine->frameHook = NULL;
        s->engine->frameHookUser = NULL;
        s->engine->externalSequencer = false;
    }
    if (s->engine && s->engine->frameEndHookUser == s) {
        s->engine->frameEndHook = NULL;
        s->engine->frameEndHookUser = NULL;
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

    if ((song->reverb & 0x80) && !s->timingOnly)   /* a timing-only sequencer plays nothing */
        m4a_reverb_set_amount(&e->reverb, song->reverb & 0x7F);
}

/* m4aMPlayStop */
void m4a_sequencer_stop(M4ASequencer *s)
{
    s->status |= MUSICPLAYER_STATUS_PAUSE;
    for (int i = 0; i < M4A_SONG_MAX_TRACKS; i++)
        track_stop(s, i);
}
