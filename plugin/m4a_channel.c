#include "m4a_channel.h"
#include "m4a_tables.h"
#include <string.h>

/*
 * PCM Channel Implementation
 * Matches the SoundMainRAM mixer in m4a_1.s
 */

/*
 * Advance the pulse synth's duty-cycle LFO one frame and recompute the phase
 * threshold the oscillator is compared against.  Matches the pulse path of
 * C_setup_synth in the improved mixer, which runs once per mixing frame:
 * count (the GBA reuses the sample countdown field) accumulates the LFO step,
 * the offset value is folded into a triangle wave with a bitwise NOT when
 * negative, and the result scaled by the mod amount plus the base duty gives
 * the threshold.  All arithmetic wraps at 32 bits like the ARM code.
 */
static void m4a_pcm_synth_pulse_update(M4APCMChannel *ch)
{
    const uint8_t *cfg = (const uint8_t *)ch->wav->data;
    uint32_t lfo = (uint32_t)ch->count + ((uint32_t)cfg[3] << 24);
    ch->count = (int32_t)lfo;
    uint32_t folded = lfo + ((uint32_t)cfg[5] << 24);
    if ((int32_t)folded < 0)
        folded = ~folded;
    ch->synthPulseDuty = ((uint32_t)cfg[2] << 24) + (folded >> 8) * cfg[4];
}

void m4a_pcm_channel_start(M4APCMChannel *ch, WaveData *wav, uint8_t type)
{
    ch->wav = wav;
    ch->type = type;
    ch->currentPointer = wav->data;
    ch->count = wav->size;
    ch->fw = 0;
    ch->envelopeVolume = 0;
    ch->dpcmBlock = -1;

    /* Reverse voices start at the end of the data and walk toward the
     * front.  SoundMainRAM_Unk1 does this lazily on the channel's first
     * mix (`data + size - offset`, flagged with SF_SPECIAL); here the start
     * offset is always 0 so the pointer is simply the end of the sample.
     * In this mode currentPointer stays one past the sample being played
     * (== data + count), the convention the GBA loop keeps as well. */
    if (type & VOICE_TYPE_REV)
        ch->currentPointer = wav->data + wav->size;

    /* Golden Sun synth voice: a zero-length sample is a synthesized-tone
     * descriptor, not PCM data.  Matches C_channel_init_synth in the improved
     * SoundMainRAM mixer. */
    ch->synthType = M4A_SYNTH_NONE;
    if (wav->size == 0 && wav->data) {
        uint8_t waveType = (uint8_t)wav->data[1];
        if (waveType == 0)
            ch->synthType = M4A_SYNTH_PULSE;
        else if (waveType == 1)
            ch->synthType = M4A_SYNTH_SAW;
        else
            ch->synthType = M4A_SYNTH_TRIANGLE;
        /* The triangle wave starts at 90 degrees phase so the note begins at
         * the zero crossing instead of the negative peak (avoids a pop).  The
         * GBA mixer only does this for the exact descriptor value 2. */
        if (waveType == 2)
            ch->fw = 0x40000000u;
        /* Prime the duty threshold so the note doesn't render against a
         * zero threshold before the first engine tick (on the GBA the frame
         * that initializes the channel also runs C_setup_synth). */
        if (ch->synthType == M4A_SYNTH_PULSE)
            m4a_pcm_synth_pulse_update(ch);
    }

    /* Check for loop - GBA checks wav->status bits 14-15 (0xC000) */
    ch->isLoop = (wav->status & 0xC000) != 0;
    if (ch->isLoop) {
        ch->loopStart = wav->data + wav->loopStart;
        ch->loopLen = wav->size - wav->loopStart;
        if (ch->loopLen <= 0) {
            ch->isLoop = false;
            ch->loopLen = 0;
        }
    }

    /* The channel waits for the next SoundMain frame with SF_START set, as
     * ply_note leaves it on the GBA: that frame's envelope pass turns it
     * into the attack phase (envelope volume = attack) and mixes it. */
    ch->status = CHN_START;
    if (ch->isLoop)
        ch->status |= CHN_LOOP;
}

void m4a_pcm_channel_stop(M4APCMChannel *ch)
{
    ch->status = 0;
}

/*
 * PCM envelope tick - called at ~60Hz
 * Matches the envelope processing in SoundMainRAM (m4a_1.s)
 */
void m4a_pcm_channel_tick(M4APCMChannel *ch, uint8_t masterVolume)
{
    if (!(ch->status & CHN_ON))
        return;

    uint8_t envVol = ch->envelopeVolume;

    if (ch->status & CHN_START) {
        /* Channel just started - handled in render path start */
        if (ch->status & CHN_STOP) {
            /* Immediate stop */
            ch->status = 0;
            return;
        }
        ch->status = CHN_ENV_ATTACK;
        if (ch->isLoop)
            ch->status |= CHN_LOOP;
        envVol = 0;
        ch->fw = 0;
        /* Fall through to attack */
    }

    if (ch->status & CHN_IEC) {
        /* Pseudo-echo countdown.  The GBA's subs/bhi is an unsigned check on
         * the pre-decrement value: a starting length of 0 borrows and stops
         * the channel immediately rather than wrapping to 255, while lengths
         * above 128 keep counting (unlike CgbSound's signed compare). */
        uint8_t echoLength = ch->pseudoEchoLength--;
        if (echoLength <= 1) {
            ch->status = 0;
            return;
        }
    } else if (ch->status & CHN_STOP) {
        /* Release phase */
        envVol = (envVol * ch->release) >> 8;
        if (envVol <= ch->pseudoEchoVolume) {
            if (ch->pseudoEchoVolume == 0) {
                ch->status = 0;
                return;
            }
            envVol = ch->pseudoEchoVolume;
            ch->status |= CHN_IEC;
        }
    } else {
        uint8_t envState = ch->status & CHN_ENV_MASK;
        if (envState == CHN_ENV_DECAY) {
            envVol = (envVol * ch->decay) >> 8;
            if (envVol <= ch->sustain) {
                envVol = ch->sustain;
                if (envVol == 0) {
                    /* Sustain is 0, go to pseudo-echo */
                    if (ch->pseudoEchoVolume == 0) {
                        ch->status = 0;
                        return;
                    }
                    envVol = ch->pseudoEchoVolume;
                    ch->status = (ch->status & ~CHN_ENV_MASK) | CHN_IEC;
                } else {
                    ch->status--;  /* decay -> sustain */
                }
            }
        } else if (envState == CHN_ENV_ATTACK) {
            /* Use 32-bit arithmetic to match GBA behavior (ldrb zero-extends,
             * so the GBA does this addition in 32-bit registers, not 8-bit). */
            uint32_t sum = (uint32_t)envVol + ch->attack;
            if (sum >= 0xFF) {
                envVol = 0xFF;
                ch->status--;  /* attack -> decay */
            } else {
                envVol = (uint8_t)sum;
            }
        }
        /* sustain: envVol stays as-is */
    }

    ch->envelopeVolume = envVol;

    /* Calculate final per-channel volumes
     * Matches: masterVolume+1 * envVol >> 4, then * rightVolume >> 8 */
    uint32_t vol = ((uint32_t)(masterVolume + 1) * envVol) >> 4;
    ch->envelopeVolumeRight = ((uint32_t)ch->rightVolume * vol) >> 8;
    ch->envelopeVolumeLeft = ((uint32_t)ch->leftVolume * vol) >> 8;

    /* Golden Sun pulse synth: the duty-cycle LFO advances once per frame,
     * mirroring C_setup_synth running once per SoundMainRAM call. */
    if (ch->synthType == M4A_SYNTH_PULSE && ch->wav)
        m4a_pcm_synth_pulse_update(ch);
}

/*
 * Fetch sample `pos` of a DPCM-compressed WaveData through the channel's
 * one-block decode cache.  Matches SoundMainRAM_Unk2 in m4a_1.s: block
 * pos>>6 is decoded into the buffer only when it differs from the cached
 * block index (xpi), then the sample at pos&63 is read back.
 *
 * Positions before the data (pos < 0) only occur on the reverse path's
 * look-behind read at the very first sample; the GBA reads whatever memory
 * sits there (a header byte for uncompressed data, an unrelated block for
 * compressed) -- returned as silence here.  Positions past the last block
 * of a truncated file decode the loader's zero fill.
 */
static int32_t m4a_dpcm_sample(M4APCMChannel *ch, int32_t pos)
{
    if (pos < 0)
        return 0;
    int32_t block = pos / M4A_DPCM_BLOCK_SAMPLES;
    if (block != ch->dpcmBlock) {
        ch->dpcmBlock = block;
        const uint8_t *src = (const uint8_t *)ch->wav->data
                             + (size_t)block * M4A_DPCM_BLOCK_BYTES;
        uint8_t level = src[0];
        ch->dpcmBuf[0] = (int8_t)level;
        /* Second byte: only its low nibble carries a delta (sample 1). */
        level = (uint8_t)(level + gDeltaEncodingTable[src[1] & 0x0F]);
        ch->dpcmBuf[1] = (int8_t)level;
        int out = 2;
        for (int i = 2; i < M4A_DPCM_BLOCK_BYTES; i++) {
            level = (uint8_t)(level + gDeltaEncodingTable[src[i] >> 4]);
            ch->dpcmBuf[out++] = (int8_t)level;
            level = (uint8_t)(level + gDeltaEncodingTable[src[i] & 0x0F]);
            ch->dpcmBuf[out++] = (int8_t)level;
        }
    }
    return ch->dpcmBuf[pos % M4A_DPCM_BLOCK_SAMPLES];
}

/* Sample `pos` of the channel's WaveData, decoding DPCM data as needed.
 * pos may be -1 (see m4a_dpcm_sample) or wav->size (the forward path's
 * look-ahead; the loader appends a safety sample for that). */
static int32_t m4a_pcm_sample_at(M4APCMChannel *ch, int32_t pos)
{
    if (ch->wav->type != 0)
        return m4a_dpcm_sample(ch, pos);
    if (pos < 0)
        return 0;
    return ch->wav->data[pos];
}

/* Accumulate one mixed sample into the stereo bus at the channel's envelope
 * volumes (the `mul` / `add` tail of every SoundMainRAM mix loop). */
static inline void m4a_pcm_mix_sample(const M4APCMChannel *ch, int32_t sample,
                                      int32_t *mixL, int32_t *mixR)
{
    *mixR += (sample * ch->envelopeVolumeRight) >> 8;
    *mixL += (sample * ch->envelopeVolumeLeft) >> 8;
}

/*
 * Advance a PCM channel's playback position by one output sample, shared by
 * both mixer paths.  fw accumulates the frequency step; whole-sample carries
 * move `pos` (the index of the sample being played, forward or backward) and
 * drain `count`.  When count runs out, a forward looping voice wraps back
 * into the loop region so that `count` samples remain before the loop end
 * (loops are never consulted in reverse); otherwise the channel switches
 * off and pos is left untouched.
 */
static inline void m4a_pcm_channel_advance(M4APCMChannel *ch, uint32_t *fw,
                                           int32_t *count, int32_t *pos,
                                           bool reverse)
{
    *fw += ch->frequency;
    uint32_t advance = *fw >> 23;
    if (!advance)
        return;
    *fw &= 0x7FFFFF;  /* keep fractional part */
    *count -= (int32_t)advance;
    if (*count <= 0) {
        if (!reverse && ch->isLoop && ch->loopLen > 0) {
            /* Wrap around loop */
            while (*count <= 0)
                *count += ch->loopLen;
            *pos = (int32_t)(ch->loopStart - ch->wav->data) + (ch->loopLen - *count);
        } else {
            ch->status = 0;
        }
    } else {
        *pos = reverse ? *pos - (int32_t)advance : *pos + (int32_t)advance;
    }
}

/*
 * PCM channel render for the reverse (TONEDATA_TYPE_REV) and DPCM-compressed
 * (WaveData.type != 0) voices -- the SoundMainRAM_Unk1 paths of m4a_1.s,
 * which SoundMainRAM branches into whenever the tone type has the REV or CMP
 * bit set.  (A CMP-flagged voice with uncompressed data takes the plain
 * forward paths of Unk1, which mix identically to the main routine, so this
 * function only claims REV voices and compressed data.)
 *
 * The position is tracked as a sample index rather than a pointer, as Unk1
 * itself does for compressed data.  Forward: `pos` is the sample being
 * played and count the samples left including it.  Reverse: the sample
 * being played is count-1 (currentPointer = data + count, one past it) and
 * the interpolation runs toward the previous sample; the sample ends when
 * count runs out -- loops are never consulted in reverse.
 *
 * Both directions interpolate exactly like the main mixer (fw's top bits are
 * the fraction toward the next sample in playback order), and like the main
 * mixer a fixed-frequency (FIX) voice is sample-and-held instead: its step
 * is only exactly one source sample when the PCM mix rate is the GBA's and
 * no pitch refresh has touched the channel, so the flag must be honoured
 * here too for the two paths to agree.
 */
static void m4a_pcm_channel_render_special(M4APCMChannel *ch, int32_t *mixL, int32_t *mixR)
{
    const bool reverse = (ch->type & VOICE_TYPE_REV) != 0;
    int8_t *data = ch->wav->data;
    uint32_t fw = ch->fw;
    int32_t count = ch->count;

    /* Sample being played and, when interpolating, its neighbour in
     * playback order. */
    int32_t pos = reverse ? count - 1 : (int32_t)(ch->currentPointer - data);
    int32_t sample = m4a_pcm_sample_at(ch, pos);
    if (!(ch->type & VOICE_TYPE_FIX)) {
        int32_t s1 = m4a_pcm_sample_at(ch, reverse ? pos - 1 : pos + 1);
        sample += (int32_t)(((int64_t)(s1 - sample) * (int32_t)fw) >> 23);
    }

    m4a_pcm_mix_sample(ch, sample, mixL, mixR);
    m4a_pcm_channel_advance(ch, &fw, &count, &pos, reverse);

    ch->currentPointer = data + (reverse ? count : pos);
    ch->fw = fw;
    ch->count = count;
}

/*
 * PCM channel render - generates one output sample
 * Matches the interpolating mixer in SoundMainRAM (m4a_1.s)
 *
 * The GBA mixer uses a 23-bit fractional sample position (fw field).
 * For non-fixed-frequency voices, it does linear interpolation between
 * adjacent samples. For fixed-frequency voices (type & 0x08), it just
 * reads one sample per output sample (no interpolation).
 */
void m4a_pcm_channel_render(M4APCMChannel *ch, int32_t *mixL, int32_t *mixR)
{
    if (!(ch->status & CHN_ON) || (ch->status & CHN_START))
        return;

    /* Golden Sun synth voices generate their tone instead of reading sample
     * data.  fw is the 32-bit oscillator phase (one wave period = 2^32),
     * advanced by frequency << 3 per output sample -- the pitch of a
     * 64-sample looped wave at the descriptor's header frequency.  Matches
     * the C_synth_* loops in the improved SoundMainRAM mixer.
     *
     * The "value" amplitudes below are expressed relative to a normal
     * sample's -128..127 range.  Careful when reading the assembly: the
     * improved mixer's normal path accumulates samples at DOUBLE scale (its
     * interpolation computes `base << 1` + `diff * frac >> 22`) against the
     * halved 0-127 volume, while the synth loops use their raw values with
     * the unhalved (pulse, triangle) or halved (saw) volume.  Folding that in
     * gives equivalent amplitudes of +/-64 (pulse), ~+/-112 (saw, after its
     * filter's DC gain of 2), and +/-128 (triangle) -- confirmed against
     * agbplay, ipatix's reference player, which mixes these synths at 0.5 /
     * ~0.875 / 1.0 of full scale respectively. */
    if (ch->synthType != M4A_SYNTH_NONE) {
        uint32_t phase = ch->fw;
        int32_t value;
        if (ch->synthType == M4A_SYNTH_PULSE) {
            /* Compared against the duty threshold before the phase advances.
             * GBA: +/-(vol << 6) at unhalved volume = equivalent +/-64. */
            value = (phase < ch->synthPulseDuty) ? 64 : -64;
            phase += ch->frequency << 3;
        } else if (ch->synthType == M4A_SYNTH_SAW) {
            /* Pseudo sawtooth: two rising ramps with a jump at mid-period,
             * smoothed by a one-pole filter (y = x + y/2) kept in count at
             * full precision.  The GBA mixes the filter state at halved
             * volume; applied here as a final halving of the value. */
            phase += ch->frequency << 3;
            int32_t raw = (int32_t)(phase >> 24) - 0x70
                        - (int32_t)((phase << 1) >> 27);
            ch->count = raw + (ch->count >> 1);
            value = ch->count >> 1;
        } else {
            /* Triangle ramping -128..+128 at unhalved volume = equivalent
             * +/-128, i.e. exactly a full-scale sample. */
            phase += ch->frequency << 3;
            if ((int32_t)phase < 0)
                value = 0x180 - (int32_t)(phase >> 23);
            else
                value = (int32_t)(phase >> 23) - 0x80;
        }
        ch->fw = phase;
        *mixR += (value * ch->envelopeVolumeRight) >> 8;
        *mixL += (value * ch->envelopeVolumeLeft) >> 8;
        return;
    }

    if ((ch->type & VOICE_TYPE_REV) || ch->wav->type != 0) {
        m4a_pcm_channel_render_special(ch, mixL, mixR);
        return;
    }

    int8_t *data = ch->wav->data;
    int32_t pos = (int32_t)(ch->currentPointer - data);
    uint32_t fw = ch->fw;
    int32_t count = ch->count;
    int32_t sample = data[pos];

    if (!(ch->type & VOICE_TYPE_FIX)) {
        /* Interpolating mixer: linear interpolation toward the next sample
         * using the top bits of fw as the fraction.  (Fixed-frequency
         * voices are sample-and-held: no interpolation.) */
        int32_t diff = data[pos + 1] - sample;
        sample += (int32_t)(((int64_t)diff * (int32_t)fw) >> 23);
    }

    m4a_pcm_mix_sample(ch, sample, mixL, mixR);
    m4a_pcm_channel_advance(ch, &fw, &count, &pos, false);

    ch->currentPointer = data + pos;
    ch->fw = fw;
    ch->count = count;
}

/*
 * CGB Channel Implementation: CgbSound() from m4a.c as a register driver.
 *
 * The four PSG channels are synthesized by the mgba-derived model in
 * m4a_psg.c; this file is the driver side, the per-frame CgbSound pass that
 * runs the software envelope and writes the sound registers.  It is a
 * transliteration of CgbSound/CgbOscOff/CgbModVol (pokeemerald src/m4a.c)
 * one channel at a time, keeping the labels of the original so the two can be
 * read side by side, with the register accesses going to the PSG instance the
 * channel belongs to (the real pool or the polyphony-debug shadow pool).
 *
 * Every write is stamped on the CPU clock: the channel tick carries a cycle
 * cursor that starts where CgbSound reaches the channel and advances through
 * the offsets measured in mgba between consecutive register writes
 * (docs/ACCURACY_PLAN.md, "SoundMain timing probe"); the hardware is run up
 * to each write (m4a_gba_run) and the write is preceded by the DAC catch-up
 * mgba performs in GBAAudioSample (m4a_gba_catch_up), so a level change
 * lands on the same 65536 Hz slot it does in mgba.
 *
 * Offsets inside a channel (cycles from the channel's start):
 *   start sequence, first write: square 1 +231, square 2 +239, wave +257,
 *     noise +246; then NR10->NR11 10; NR30->WAVE 26, WAVE->WAVE 15, WAVE->
 *     NR30 6, NR30->NR31 4; NR41->NR43 8; start block -> MO_PIT NRx3: square
 *     202, wave 158, noise 162.
 *   MO_PIT without a start: NRx3 +163; NRx3->NRx4 32 (noise 24).
 *   MO_VOL: NR51 33 after the MO_PIT NRx4, or +207 (square) / +416 (wave,
 *     noise) when nothing was written before; NR51->NRx2 28 (wave 32),
 *     NRx2->NRx4 10, wave NR32->NR30 16, NR30->NR34 6.
 *   CgbOscOff: square NRx2 +203, noise NR42 +220, wave NR30 +229; NRx2->NRx4
 *     14 (square) / 6 (noise).
 *   The envelope double-step (prevC15 == 0) adds 199 before the writes.
 *   A channel ends 93 cycles after its last write, or after 244 if it wrote
 *   nothing.
 */

#define MO_VOL 0x01
#define MO_PIT 0x02
#define MO_DUTY 0x04   /* engine-side: pulse-width modulation changed dutyCycle */

typedef struct {
    M4APsg *psg;
    M4AGbaAudio *gba;
    int64_t cursor;     /* CPU cycle of the next register write */
    bool wrote;         /* a write has happened in this channel's pass */
} CgbIo;

/* Move the CPU clock to the cursor: hardware events due before it run
 * first; a cursor in the past (the polyphony-debug shadow pool replaying
 * the real pool's timeline) writes at the current cycle. */
static void cgb_sync(CgbIo *io)
{
    if (io->cursor > io->gba->now) {
        m4a_gba_run(io->gba, io->cursor - 1);
        io->gba->now = io->cursor;
    }
}

static void cgb_write(CgbIo *io, uint32_t reg, uint8_t value)
{
    cgb_sync(io);
    m4a_gba_catch_up(io->gba, io->gba->now);
    m4a_psg_write8(io->psg, reg, value, io->gba->now);
    io->wrote = true;
}

static uint8_t cgb_read(CgbIo *io, uint32_t reg)
{
    return m4a_psg_read8(io->psg, reg);
}

/* REG_WAVE_RAMn = word: a 32-bit store, which mgba routes straight to the
 * wave RAM (GBAIOWrite32 -> GBAAudioWriteWaveRAM) without a DAC catch-up. */
static void cgb_write_wave_ram(CgbIo *io, int index, uint32_t value)
{
    cgb_sync(io);
    m4a_psg_write_wave_ram(io->psg, index, value, io->gba->now);
    io->wrote = true;
}

/* The register set of hardware channel `chn` (1..4), as CgbSound picks it. */
static void cgb_regs(int chn, uint32_t *nrx0, uint32_t *nrx1, uint32_t *nrx2,
                     uint32_t *nrx3, uint32_t *nrx4)
{
    switch (chn) {
    case 1:
        *nrx0 = PSG_REG_NR10; *nrx1 = PSG_REG_NR11; *nrx2 = PSG_REG_NR12;
        *nrx3 = PSG_REG_NR13; *nrx4 = PSG_REG_NR14;
        break;
    case 2:
        *nrx0 = PSG_REG_NR10 + 1; *nrx1 = PSG_REG_NR21; *nrx2 = PSG_REG_NR22;
        *nrx3 = PSG_REG_NR23; *nrx4 = PSG_REG_NR24;
        break;
    case 3:
        *nrx0 = PSG_REG_NR30; *nrx1 = PSG_REG_NR31; *nrx2 = PSG_REG_NR32;
        *nrx3 = PSG_REG_NR33; *nrx4 = PSG_REG_NR34;
        break;
    default:
        *nrx0 = PSG_REG_NR30 + 1; *nrx1 = PSG_REG_NR41; *nrx2 = PSG_REG_NR42;
        *nrx3 = PSG_REG_NR43; *nrx4 = PSG_REG_NR44;
        break;
    }
}

/* CgbOscOff.  `io->cursor` is the cycle of the first write. */
static void cgb_osc_off(M4ACGBChannel *ch, CgbIo *io)
{
    switch (ch->type) {
    case 1:
        cgb_write(io, PSG_REG_NR12, 8);
        io->cursor += 14;
        cgb_write(io, PSG_REG_NR14, 0x80);
        break;
    case 2:
        cgb_write(io, PSG_REG_NR22, 8);
        io->cursor += 14;
        cgb_write(io, PSG_REG_NR24, 0x80);
        break;
    case 3:
        cgb_write(io, PSG_REG_NR30, 0);
        break;
    default:
        cgb_write(io, PSG_REG_NR42, 8);
        io->cursor += 6;
        cgb_write(io, PSG_REG_NR44, 0x80);
        break;
    }
}

/* CgbOscOff from outside CgbSound (TrackStop): writes from `*cursor` on. */
void m4a_cgb_osc_off(M4ACGBChannel *ch, M4APsg *psg, M4AGbaAudio *gba, int64_t *cursor)
{
    CgbIo io = { psg, gba, *cursor, false };
    cgb_osc_off(ch, &io);
    *cursor = io.cursor;
}

/* ply_note's share of a CGB note start: the channel is flagged to start at
 * the next CgbSound pass, which performs the register setup (the caller has
 * already stored the voice parameters, frequency and wave table). */
void m4a_cgb_channel_start(M4ACGBChannel *ch)
{
    ch->status = CHN_START;
    ch->hwResync = false;
}

/*
 * CGB pan calculation - matches CgbPan() in m4a.c.
 * Determines whether the channel is hard-panned to one side.
 * Sets ch->pan to 0x0F (hard right) or 0xF0 (hard left) and returns 1,
 * or leaves ch->pan unchanged and returns 0 (center/both sides).
 */
static int cgb_pan(M4ACGBChannel *ch)
{
    uint32_t rightVolume = (uint8_t)ch->rightVolume;
    uint32_t leftVolume  = (uint8_t)ch->leftVolume;

    if (rightVolume >= leftVolume) {
        if (rightVolume / 2 >= leftVolume) {
            ch->pan = 0x0F;
            return 1;
        }
    } else {
        if (leftVolume / 2 >= rightVolume) {
            ch->pan = 0xF0;
            return 1;
        }
    }
    return 0;
}

/*
 * CGB mod vol calculation - matches CgbModVol in m4a.c.
 * Converts the software left/right volumes (from velocity + CC7 + pan) into
 * the 4-bit hardware envelope goal and the NR51 pan routing bits.
 */
void m4a_cgb_mod_vol(M4ACGBChannel *ch)
{
    /* The C source reads `chan->envelopeGoal = (u32)(left + right);
     * envelopeGoal /= 16;` on a u8 field, which would wrap the sum at 256
     * before the divide -- but the ROM agbcc builds keeps the sum in a
     * register and stores (left + right) / 16 (0..31): the wrapped form
     * nulls 12 dB worse against mgba on lfo_psg.  So no wrap here, and the
     * center-pan goal can exceed 15 (see cgb3_vol). */
    if (!cgb_pan(ch)) {
        /* Center (or near-center) pan: output to both sides */
        ch->pan = 0xFF;
        ch->envelopeGoal = (uint32_t)(ch->leftVolume + ch->rightVolume) / 16;
    } else {
        /* Hard-panned: pan already set by cgb_pan(); clamp goal to hardware max */
        ch->envelopeGoal = (uint32_t)(ch->leftVolume + ch->rightVolume) / 16;
        if (ch->envelopeGoal > 15)
            ch->envelopeGoal = 15;
    }
    ch->sustainGoal = (ch->envelopeGoal * ch->sustain + 15) >> 4;
    ch->pan &= ch->panMask;
}

/* NR32 value for a software envelope volume.  gCgb3Vol has 16 entries, but
 * the center-pan envelopeGoal is unclamped (up to 31) and the GBA reads past
 * the table into gClockTable, whose first entries are 0x00..0x18: values
 * below 0x20, whose NR32 volume field (bits 5-7) is 0 = mute.  So an
 * over-range volume is silence on the GBA, and here. */
static uint8_t cgb3_vol(uint8_t envelopeVolume)
{
    return envelopeVolume < 16 ? gCgb3Vol[envelopeVolume] : 0;
}

/* The start-frame register setup CgbSound performs for a channel: sweep,
 * duty/length, the wave RAM upload, the noise period; sets n4 and the
 * initial envelopeStepTimeAndDir. */
static void cgb_start_registers(M4ACGBChannel *ch, CgbIo *io, int chn,
                                uint32_t nrx0, uint32_t nrx1, uint32_t nrx3,
                                int *envelopeStepTimeAndDir)
{
    switch (chn) {
    case 1:
        cgb_write(io, nrx0, ch->sweep);
        io->cursor += 10;
        /* fallthrough */
    case 2:
        cgb_write(io, nrx1, (uint8_t)(((uint32_t)ch->dutyCycle << 6) + ch->length));
        goto init_env_step_time_dir;
    case 3:
        if (ch->wavePointer != ch->currentWavePointer) {
            cgb_write(io, nrx0, 0x40);
            io->cursor += 26;
            cgb_write_wave_ram(io, 0, ch->wavePointer[0]);
            io->cursor += 15;
            cgb_write_wave_ram(io, 1, ch->wavePointer[1]);
            io->cursor += 15;
            cgb_write_wave_ram(io, 2, ch->wavePointer[2]);
            io->cursor += 15;
            cgb_write_wave_ram(io, 3, ch->wavePointer[3]);
            io->cursor += 6;
            ch->currentWavePointer = ch->wavePointer;
        }
        cgb_write(io, nrx0, 0);
        io->cursor += 4;
        cgb_write(io, nrx1, ch->length);
        if (ch->length)
            ch->n4 = 0xC0;
        else
            ch->n4 = 0x80;
        break;
    default:
        cgb_write(io, nrx1, ch->length);
        io->cursor += 8;
        /* (u32)wavePointer << 3: the noise voice's period bit, which the
         * engine keeps in bit 3 of ch->frequency. */
        cgb_write(io, nrx3, (uint8_t)(ch->frequency & 0x08));
    init_env_step_time_dir:
        *envelopeStepTimeAndDir = ch->attack + 0x08; /* CGB_NRx2_ENV_DIR_INC */
        if (ch->length)
            ch->n4 = 0x40;
        else
            ch->n4 = 0x00;
        break;
    }
}

/*
 * CgbSound() for one channel.  `c15` is SoundInfo.c15 after this frame's
 * decrement (the engine does that once per frame for all channels).
 */
void m4a_cgb_channel_tick(M4ACGBChannel *ch, uint8_t c15, M4APsg *psg, M4AGbaAudio *gba,
                          int64_t *cursor)
{
    if (!(ch->status & CHN_ON))
        return;

    const int64_t chanStart = *cursor;
    CgbIo io = { psg, gba, chanStart, false };
    int chn = ch->type;
    /* First register write of a start sequence, a pitch-only update, a
     * volume-only update and an oscillator-off, from the channel's start. */
    int startAt = chn == 1 ? 231 : chn == 2 ? 239 : chn == 3 ? 255 : 246;
    if (chn == 3 && ch->wavePointer == ch->currentWavePointer)
        startAt = 261;                   /* no wave RAM upload: NR30 = 0 is the first write */
    /* Start block -> MO_PIT write, for a start that goes straight to the
     * decay phase; the decay -> sustain transition on a start (decay 0)
     * costs 36 more (sq1 202 vs sq1_env 166). */
    int startToPit = chn == 3 ? 158 : chn == 4 ? 162 : 166;
    const int fixPit = (chn < 4 && ch->fixedFreq) ? 40 : 0;   /* TONEDATA_TYPE_FIX rounding */
    const int pitAt = 166 + fixPit;
    int volAt = 207;                     /* decay start (and other transitions) */
    int offAt = chn == 3 ? 237 : chn == 4 ? 228 : 211;   /* release 0 / no echo -> off */
    int envExtra = 0;                    /* envelope passes before the first write */
    uint32_t nrx0, nrx1, nrx2, nrx3, nrx4;
    cgb_regs(chn, &nrx0, &nrx1, &nrx2, &nrx3, &nrx4);

    /* Most comparison operations that cast to s8 perform 'and' by 0xFF. */
    const int mask = 0xff;
    int prevC15 = c15;
    int envelopeStepTimeAndDir = cgb_read(&io, nrx2);
    int envelopeVolume, sustainGoal;

    /* A sound moved to the polyphony-debug shadow pool mid-note: give its
     * PSG the start-frame registers it never saw, then re-apply pitch and
     * volume this frame (a trigger with the current envelope volume). */
    if (ch->hwResync && !(ch->status & CHN_START)) {
        int unused;
        ch->hwResync = false;
        io.cursor = chanStart + startAt;
        cgb_start_registers(ch, &io, chn, nrx0, nrx1, nrx3, &unused);
        if (chn == 3)
            ch->n4 |= 0x80;
        ch->modify |= MO_PIT | MO_VOL;
    }

    /* 2. calculate envelope volume */
    if (ch->status & CHN_START) {
        if (!(ch->status & CHN_STOP)) {
            ch->status = CHN_ENV_ATTACK;
            ch->modify = MO_PIT | MO_VOL;
            m4a_cgb_mod_vol(ch);
            io.cursor = chanStart + startAt;
            cgb_start_registers(ch, &io, chn, nrx0, nrx1, nrx3, &envelopeStepTimeAndDir);
            ch->envelopeCounter = ch->attack;
            if ((int8_t)(ch->attack & mask)) {
                ch->envelopeVolume = 0;
                goto envelope_step_complete;
            } else {
                /* skip attack phase if attack is instantaneous (=0) */
                goto envelope_decay_start;
            }
        } else {
            goto oscillator_off;
        }
    } else if (ch->status & CHN_IEC) {
        ch->pseudoEchoLength--;
        if ((int8_t)(ch->pseudoEchoLength & mask) <= 0) {
            offAt = chn == 3 ? 175 : chn == 4 ? 166 : 149;   /* echo expired */
        oscillator_off:
            io.cursor = chanStart + offAt;
            cgb_osc_off(ch, &io);
            ch->status = 0;
            goto channel_complete;
        }
        goto envelope_complete;
    } else if ((ch->status & CHN_STOP) && (ch->status & CHN_ENV_MASK)) {
        ch->status &= ~CHN_ENV_MASK;
        ch->envelopeCounter = ch->release;
        volAt = 215;                     /* release start */
        if ((int8_t)(ch->release & mask)) {
            ch->modify |= MO_VOL;
            if (chn != 3)
                envelopeStepTimeAndDir = ch->release | 0x00; /* CGB_NRx2_ENV_DIR_DEC */
            goto envelope_step_complete;
        } else {
            goto envelope_pseudoecho_start;
        }
    } else {
    envelope_step_repeat:
        if (ch->envelopeCounter == 0) {
            if (chn == 3)
                ch->modify |= MO_VOL;
            if (chn != 3)
                envExtra += 163;         /* CgbModVol + the step (lfo_psg sustain steps) */

            m4a_cgb_mod_vol(ch);
            if ((ch->status & CHN_ENV_MASK) == CHN_ENV_RELEASE) {
                ch->envelopeVolume--;
                if ((int8_t)(ch->envelopeVolume & mask) <= 0) {
                envelope_pseudoecho_start:
                    ch->envelopeVolume = (uint8_t)(((ch->envelopeGoal * ch->pseudoEchoVolume) + 0xFF) >> 8);
                    if (ch->envelopeVolume) {
                        ch->status |= CHN_IEC;
                        ch->modify |= MO_VOL;
                        volAt = 376;     /* pseudo-echo start */
                        if (chn != 3)
                            envelopeStepTimeAndDir = 0 | 0x08;
                        goto envelope_complete;
                    } else {
                        goto oscillator_off;
                    }
                } else {
                    ch->envelopeCounter = ch->release;
                }
            } else if ((ch->status & CHN_ENV_MASK) == CHN_ENV_SUSTAIN) {
            envelope_sustain:
                ch->envelopeVolume = ch->sustainGoal;
                ch->envelopeCounter = 7;
                if (volAt == 207)
                    volAt = 350;         /* a plain sustain step (wave writes NR51/NR32) */
            } else if ((ch->status & CHN_ENV_MASK) == CHN_ENV_DECAY) {
                ch->envelopeVolume--;
                envelopeVolume = (int8_t)(ch->envelopeVolume & mask);
                sustainGoal = (int8_t)(ch->sustainGoal);
                if (envelopeVolume <= sustainGoal) {
                envelope_sustain_start:
                    if (ch->sustain == 0) {
                        ch->status &= ~CHN_ENV_MASK;
                        goto envelope_pseudoecho_start;
                    } else {
                        ch->status--;
                        ch->modify |= MO_VOL;
                        volAt = chn == 3 ? 416 : chn == 4 ? 417 : 380;   /* decay -> sustain transition */
                        if (io.wrote)
                            startToPit += 36;           /* reached from the start block */
                        if (chn != 3)
                            envelopeStepTimeAndDir = 0 | 0x08;
                        goto envelope_sustain;
                    }
                } else {
                    ch->envelopeCounter = ch->decay;
                }
            } else {
                ch->envelopeVolume++;
                if ((uint8_t)(ch->envelopeVolume & mask) >= ch->envelopeGoal) {
                envelope_decay_start:
                    ch->status--;
                    ch->envelopeCounter = ch->decay;
                    if ((uint8_t)(ch->envelopeCounter & mask)) {
                        ch->modify |= MO_VOL;
                        ch->envelopeVolume = ch->envelopeGoal;
                        if (chn != 3)
                            envelopeStepTimeAndDir = ch->decay | 0x00;
                    } else {
                        goto envelope_sustain_start;
                    }
                } else {
                    ch->envelopeCounter = ch->attack;
                }
            }
        }
    }

envelope_step_complete:
    /* every 15 frames, envelope calculation has to be done twice
     * to keep up with the hardware envelope rate (1/64 s) */
    ch->envelopeCounter--;
    if (prevC15 == 0) {
        prevC15--;
        envExtra += 36;                  /* the second pass */
        goto envelope_step_repeat;
    }

envelope_complete:
    /* Opt-in pulse-width modulation (not stock CgbSound): a duty change on a
     * sounding square is written to NRx1, which the PSG applies at once. */
    if ((ch->modify & MO_DUTY) && (chn == 1 || chn == 2))
        cgb_write(&io, nrx1, (uint8_t)(((uint32_t)ch->dutyCycle << 6) + ch->length));

    /* 3. apply pitch to HW registers */
    if (ch->modify & MO_PIT) {
        /* TONEDATA_TYPE_FIX rounding for the 65536 Hz DAC (SOUNDBIAS_H =
         * 0x40); the engine already applies it when computing ch->frequency
         * (m4a_cgb_fix_freq), and the rounding is idempotent. */
        if (chn < 4 && ch->fixedFreq)
            ch->frequency = (ch->frequency + 1) & 0x7fe;

        io.cursor = io.wrote ? io.cursor + startToPit + fixPit : chanStart + pitAt + envExtra;
        if (chn != 4)
            cgb_write(&io, nrx3, (uint8_t)ch->frequency);
        else
            cgb_write(&io, nrx3, (uint8_t)((cgb_read(&io, nrx3) & 0x08) | ch->frequency));
        ch->n4 = (uint8_t)((ch->n4 & 0xC0) + ((ch->frequency >> 8) & 0xFF));
        io.cursor += chn == 4 ? 24 : 32;
        cgb_write(&io, nrx4, ch->n4);
    }

    /* 4. apply envelope & volume to HW registers */
    if (ch->modify & MO_VOL) {
        io.cursor = io.wrote ? io.cursor + 33 : chanStart + volAt + (envExtra % 163);
        cgb_write(&io, PSG_REG_NR51,
                  (uint8_t)((cgb_read(&io, PSG_REG_NR51) & ~ch->panMask) | ch->pan));
        if (chn == 3) {
            io.cursor += 32;
            cgb_write(&io, nrx2, cgb3_vol(ch->envelopeVolume));
            if (ch->n4 & 0x80) {
                io.cursor += 16;
                cgb_write(&io, nrx0, 0x80);
                io.cursor += 6;
                cgb_write(&io, nrx4, ch->n4);
                ch->n4 &= 0x7f;
            }
        } else {
            uint32_t envMask = 0xF;
            io.cursor += 28;
            cgb_write(&io, nrx2, (uint8_t)((envelopeStepTimeAndDir & envMask) + (ch->envelopeVolume << 4)));
            io.cursor += 10;
            cgb_write(&io, nrx4, ch->n4 | 0x80);
            if (chn == 1 && !(cgb_read(&io, nrx0) & 0x08)) {
                io.cursor += 10;
                cgb_write(&io, nrx4, ch->n4 | 0x80);
            }
        }
    }

channel_complete:
    ch->modify = 0;
    *cursor = io.wrote ? io.cursor + 93 : chanStart + 244 + envExtra;
}
