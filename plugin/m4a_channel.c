#include "m4a_channel.h"
#include "m4a_tables.h"
#include <string.h>

/* Number of samples over which the wave channel (type 3) fades to zero on
 * note-off, preventing a DC-offset pop. ~5.8 ms at 44100 Hz. */
#define DECLICK_SAMPLES 256

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

    /* Set status to attack and immediately process first envelope step.
     * On the GBA, the channel starts with CHN_START flag and the mixer
     * handles the transition. Since our tick runs at ~60Hz but render runs
     * at the DAW sample rate, we need the envelope to be non-zero from
     * the first render call to avoid silence at the start. */
    ch->status = CHN_ENV_ATTACK;
    if (ch->isLoop)
        ch->status |= CHN_LOOP;

    /* Immediately process attack to get a non-zero envelope volume */
    uint8_t envVol = ch->attack;
    if (envVol >= 0xFF) {
        envVol = 0xFF;
        ch->status = CHN_ENV_DECAY | (ch->status & CHN_LOOP);
    }
    ch->envelopeVolume = envVol;
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
        /* Pseudo-echo countdown.  Signed check matches the GBA's subs/bhi:
         * a starting length of 0 underflows and must stop the channel
         * immediately rather than wrapping to 255. */
        ch->pseudoEchoLength--;
        if ((int8_t)ch->pseudoEchoLength <= 0) {
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
 * CGB Channel Implementation
 * Matches CgbSound() in m4a.c
 */

/*
 * Square-1 frequency sweep (NR10).
 *
 * The m4a engine programs the sweep once per note: ply_note stores the
 * voice's pan_sweep byte in the channel (forced to the inert value 8 when the
 * byte holds a pan or its time bits are zero) and CgbSound writes it to NR10
 * at note start.  From then on the Game Boy hardware sweeps on its own, so
 * this emulation follows mGBA's sweep unit (_writeSweep / _resetSweep /
 * _updateSweep in src/gb/audio.c) rather than anything in m4a.c:
 *
 *  - a 128 Hz clock (frame-sequencer steps 2 and 6) decrements sweepStep;
 *    when it hits 0 the unit computes f' = f +/- (f >> shift) from its
 *    internal shadow register and writes the result to both the shadow and
 *    the frequency register (downward only when f' >= 0, upward only when
 *    shift != 0 and f' < 2048; an upward result >= 2048 disables the channel)
 *  - a trigger (NRx4 bit 7) re-enables the channel, reloads the shadow from
 *    the frequency register, resets the timer, and -- when shift != 0 --
 *    immediately runs one overflow check that never writes the frequency.
 *
 * Because CgbSound sets the trigger bit on every MO_VOL register update, the
 * unit is retriggered at every envelope phase transition and track volume
 * change; MO_PIT frequency writes do NOT touch the shadow, so vibrato/bend on
 * a sweeping note is overridden at the unit's next calculation.
 * Likewise the sweep's own frequency writes are deliberately NOT quantized
 * for fixedFreq voices -- see m4a_cgb_fix_freq() in m4a_engine.c.
 */

static inline int cgb_sweep_time(const M4ACGBChannel *ch)
{
    int time = (ch->sweep >> 4) & 7;
    return time ? time : 8;
}

/* One sweep calculation.  `initial` is the trigger-time overflow check, which
 * computes but never writes.  Returns false when an upward sweep overflows
 * past 2047 (the hardware then clears the channel's NR52 enable bit). */
static bool cgb_sweep_calc(M4ACGBChannel *ch, bool initial)
{
    int shift = ch->sweep & 7;

    if (initial || cgb_sweep_time(ch) != 8) {
        int32_t freq = ch->sweepShadowFreq;
        if (ch->sweep & 0x08) {
            freq -= freq >> shift;
            if (!initial && freq >= 0) {
                ch->sweepShadowFreq = (uint16_t)freq;
                ch->frequency = (uint32_t)freq;
            }
        } else {
            freq += freq >> shift;
            if (freq >= 2048)
                return false;
            if (!initial && shift) {
                ch->sweepShadowFreq = (uint16_t)freq;
                ch->frequency = (uint32_t)freq;
                /* Writing a new frequency immediately re-runs the overflow
                 * check against the next step's value. */
                if (!cgb_sweep_calc(ch, true))
                    return false;
            }
        }
    }

    ch->sweepStep = (uint8_t)cgb_sweep_time(ch);
    return true;
}

/* NRx4 trigger-bit write as seen by the sweep unit. */
static void cgb_sweep_retrigger(M4ACGBChannel *ch)
{
    int time = cgb_sweep_time(ch);
    int shift = ch->sweep & 7;

    ch->sweepMuted = false;
    ch->sweepShadowFreq = (uint16_t)(ch->frequency & 0x7FF);
    ch->sweepStep = (uint8_t)time;
    ch->sweepEnabled = (time != 8) || shift != 0;
    if (shift && !cgb_sweep_calc(ch, true))
        ch->sweepMuted = true;
}

/* One 128 Hz frame-sequencer sweep clock. */
static void cgb_sweep_clock(M4ACGBChannel *ch)
{
    if (!ch->sweepEnabled || ch->sweepMuted)
        return;
    if (--ch->sweepStep != 0)
        return;
    if (!cgb_sweep_calc(ch, false))
        ch->sweepMuted = true;
}

/* CgbSound's MO_VOL register write as seen by the hardware envelope unit:
 * NRx2 = (envelopeVolume << 4) | hwEnvStepDir plus the NRx4 trigger bit.
 * The trigger reloads the unit's volume and step counter (mGBA
 * _resetEnvelope) and resets the noise LFSR.  The 64 Hz clock accumulator is
 * deliberately NOT reset: the frame sequencer free-runs on hardware, which is
 * what gives identical percussion hits their audible level variation. */
static void cgb_hw_env_write(M4ACGBChannel *ch)
{
    uint8_t stepTime = ch->hwEnvStepDir & 0x07;
    bool dirInc = (ch->hwEnvStepDir & 0x08) != 0;

    ch->hwEnvVolume = ch->envelopeVolume & 0x0F;
    ch->hwEnvNextStep = stepTime;
    if (stepTime == 0)
        ch->hwEnvDead = 1;
    else if (!dirInc && ch->hwEnvVolume == 0)
        ch->hwEnvDead = 1;
    else if (dirInc && ch->hwEnvVolume == 15)
        ch->hwEnvDead = 1;
    else
        ch->hwEnvDead = 0;

    if (ch->type == 4)
        ch->lfsr = (ch->frequency & 0x08) ? 0x7F : 0x7FFF;
}

void m4a_cgb_channel_start(M4ACGBChannel *ch)
{
    ch->status = CHN_ENV_ATTACK;
    ch->modify = 0x03; /* pitch + vol */

    /* Waveform position at note start, per channel type, modeled on what the
     * NRx4 trigger CgbSound writes here actually does in mGBA's gb/audio.c
     * (GB_AUDIO_GBA style) -- verified against in-game recordings:
     *
     * - Squares: NOT touched.  The 8-step duty counter only ever advances
     *   with time (GBAudioWriteNR14/NR24 leave ch->index and its timer
     *   alone), so a retrigger -- a new note, or the trigger CgbSound writes
     *   on every volume change -- continues the running waveform.  Restarting
     *   from phase 0 here put a hard edge at every note boundary (an audible
     *   click on back-to-back notes the console plays seamlessly) and
     *   re-aligned the two squares' relative phase at every note.  Squares
     *   keep free-running even while off, see cgb_square_advance().
     *
     * - Wave: on the GBA the wave RAM is a rotating shift register (mGBA
     *   rotates wavedata32 a nibble per step; the trigger's window reset is
     *   DMG-only), so the position persists across triggers too.  It restarts
     *   only when CgbSound rewrites the wave RAM, which it does at note start
     *   only if the voice's table differs from the one already loaded
     *   (wavePointer != currentPointer): fresh RAM plays from position 0.
     *   Otherwise the trigger merely re-arms the step timer
     *   (nextUpdate = now + one full step), so the current step restarts from
     *   its beginning: a slip of up to one step, the faint tick audible at
     *   each repeated wave note in-game.
     *
     * - Noise: the LFSR and its timer are reset (below). */
    if (ch->type == 3) {
        if (ch->wavePointer != ch->currentWavePointer) {
            ch->phase = 0;
            ch->currentWavePointer = ch->wavePointer;
        } else {
            ch->phase &= 0xF8000000u; /* keep the step (top 5 bits), restart it */
        }
    } else if (ch->type == 4) {
        ch->phase = 0;
    }

    /* Invalidate the cached phase increment / wave DC sum so the first
     * rendered sample of this note recomputes them from the current
     * frequency and wave table. */
    ch->phaseIncFreq = 0xFFFFFFFFu;
    ch->waveSumPointer = NULL;
    ch->envelopeCounter = ch->attack;
    if (ch->attack == 0) {
        /* Skip attack if instantaneous */
        ch->envelopeVolume = ch->envelopeGoal;
        ch->status = CHN_ENV_DECAY;
        ch->envelopeCounter = ch->decay;
        ch->hwEnvStepDir = ch->decay & 0x07;
        if (ch->decay == 0) {
            /* Skip decay too */
            if (ch->sustain == 0) {
                ch->status = CHN_ENV_RELEASE;
                ch->hwEnvStepDir = ch->release & 0x07;
            } else {
                ch->envelopeVolume = ch->sustainGoal;
                ch->status = CHN_ENV_SUSTAIN;
                ch->hwEnvStepDir = 0x08; /* step 0: frozen at sustain */
            }
        }
    } else {
        ch->envelopeVolume = 0;
        ch->hwEnvStepDir = (ch->attack & 0x07) | 0x08;
    }

    /* Cancel any in-progress declick so the new note starts cleanly. */
    ch->declickSample = 0;
    ch->declickSamplesRemaining = 0;

    /* Square 1: note start writes NR10 and sets the NRx4 trigger bit,
     * (re)arming the hardware frequency sweep unit.  The caller has already
     * set ch->sweep and ch->frequency. */
    if (ch->type == 1) {
        ch->sweepClockAccum = 0.0f;
        cgb_sweep_retrigger(ch);
    }

    /* Initialize LFSR for noise channel.
     * Bit 3 of frequency is the period/mode bit (NR43 bit 3):
     * 0 = 15-bit LFSR, 1 = 7-bit short-period LFSR. */
    if (ch->type == 4) {
        if (ch->frequency & 0x08)
            ch->lfsr = 0x7F;    /* 7-bit */
        else
            ch->lfsr = 0x7FFF;  /* 15-bit */
    }

    /* Note start is CgbSound's first MO_VOL write: arm the hardware envelope
     * from the initial software value (goal, or 0 for a real attack).  m4a
     * performs the start-frame register writes and clears `modify` in the same
     * CgbSound pass; this start path runs outside the tick, so clear it here
     * or the next tick would re-apply the write and re-arm the step timer. */
    if (ch->type != 3)
        cgb_hw_env_write(ch);
    ch->modify = 0;
}

void m4a_cgb_channel_stop(M4ACGBChannel *ch)
{
    if (ch->type == 3)
        ch->declickSamplesRemaining = DECLICK_SAMPLES;
    ch->status = 0;
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

/*
 * CGB envelope tick - matches CgbSound() envelope logic in m4a.c
 * Called at ~60Hz, with double-step every 15 frames (when c15==0)
 */
void m4a_cgb_channel_tick(M4ACGBChannel *ch, uint8_t c15)
{
    if (!(ch->status & CHN_ON))
        return;

    /* Declared before the goto targets below: the start and release-start
     * paths jump into the envelope block, and must see initialized values so
     * they still honor the every-15th-frame double step (CgbSound initializes
     * prevC15 before any of the equivalent branches). */
    int doubleStep = (c15 == 0) ? 1 : 0;
    int steps = 0;

    if (ch->status & CHN_START) {
        if (ch->status & CHN_STOP) {
            if (ch->type == 3)
                ch->declickSamplesRemaining = DECLICK_SAMPLES;
            ch->status = 0;
            return;
        }
        ch->status = CHN_ENV_ATTACK;
        ch->modify = 0x03;
        m4a_cgb_mod_vol(ch);
        ch->envelopeCounter = ch->attack;
        if (ch->attack != 0) {
            ch->envelopeVolume = 0;
            ch->hwEnvStepDir = (ch->attack & 0x07) | 0x08;
        } else {
            /* skip attack */
            ch->envelopeVolume = ch->envelopeGoal;
            ch->status = CHN_ENV_DECAY;
            ch->envelopeCounter = ch->decay;
            ch->hwEnvStepDir = ch->decay & 0x07;
            if (ch->decay == 0) {
                if (ch->sustain == 0) {
                    goto pseudo_echo;
                }
                ch->status = CHN_ENV_SUSTAIN;
                ch->envelopeVolume = ch->sustainGoal;
                ch->hwEnvStepDir = 0x08;
            }
        }
        goto step_complete;
    }

    if (ch->status & CHN_IEC) {
        ch->pseudoEchoLength--;
        if ((int8_t)ch->pseudoEchoLength <= 0) {
            if (ch->type == 3)
                ch->declickSamplesRemaining = DECLICK_SAMPLES;
            ch->status = 0;
            return;
        }
        goto envelope_complete;
    }

    if ((ch->status & CHN_STOP) && (ch->status & CHN_ENV_MASK)) {
        ch->status &= ~CHN_ENV_MASK;
        ch->envelopeCounter = ch->release;
        if (ch->release != 0) {
            ch->modify |= 0x01;
            ch->hwEnvStepDir = ch->release & 0x07;
            goto step_complete;
        } else {
            goto pseudo_echo;
        }
    }

    {
step_repeat:
        if (ch->envelopeCounter == 0) {
            m4a_cgb_mod_vol(ch);
            uint8_t envState = ch->status & CHN_ENV_MASK;

            if (envState == CHN_ENV_RELEASE) {
                ch->envelopeVolume--;
                if ((int8_t)ch->envelopeVolume <= 0) {
                pseudo_echo:
                    ch->envelopeVolume = ((ch->envelopeGoal * ch->pseudoEchoVolume) + 0xFF) >> 8;
                    if (ch->envelopeVolume) {
                        ch->status |= CHN_IEC;
                        ch->modify |= 0x01;
                        ch->hwEnvStepDir = 0x08; /* step 0: frozen at echo volume */
                        goto envelope_complete;
                    } else {
                        if (ch->type == 3)
                            ch->declickSamplesRemaining = DECLICK_SAMPLES;
                        ch->status = 0;
                        return;
                    }
                }
                ch->envelopeCounter = ch->release;
            } else if (envState == CHN_ENV_SUSTAIN) {
                ch->envelopeVolume = ch->sustainGoal;
                ch->envelopeCounter = 7;
            } else if (envState == CHN_ENV_DECAY) {
                ch->envelopeVolume--;
                if ((int8_t)ch->envelopeVolume <= (int8_t)ch->sustainGoal) {
                    if (ch->sustain == 0) {
                        ch->status &= ~CHN_ENV_MASK;
                        goto pseudo_echo;
                    }
                    ch->status--;  /* decay -> sustain */
                    ch->modify |= 0x01;
                    ch->hwEnvStepDir = 0x08; /* step 0: frozen at sustain */
                    ch->envelopeVolume = ch->sustainGoal;
                    ch->envelopeCounter = 7;
                    goto step_complete;
                }
                ch->envelopeCounter = ch->decay;
            } else {
                /* Attack */
                ch->envelopeVolume++;
                if (ch->envelopeVolume >= ch->envelopeGoal) {
                    ch->status--;  /* attack -> decay */
                    ch->envelopeCounter = ch->decay;
                    if (ch->decay != 0) {
                        ch->modify |= 0x01;
                        ch->hwEnvStepDir = ch->decay & 0x07;
                        ch->envelopeVolume = ch->envelopeGoal;
                    } else {
                        if (ch->sustain == 0) {
                            ch->status &= ~CHN_ENV_MASK;
                            goto pseudo_echo;
                        }
                        ch->status--;
                        ch->modify |= 0x01;
                        ch->hwEnvStepDir = 0x08; /* step 0: frozen at sustain */
                        ch->envelopeVolume = ch->sustainGoal;
                        ch->envelopeCounter = 7;
                    }
                    goto step_complete;
                }
                ch->envelopeCounter = ch->attack;
            }
        }

step_complete:
        ch->envelopeCounter--;
        /* Double step every 15 frames (when c15==0) to match hardware 1/64s rate */
        if (doubleStep && steps == 0) {
            steps = 1;
            goto step_repeat;
        }
    }

envelope_complete:
    /* CgbSound applies MO_VOL by rewriting NRx2 and setting the NRx4 trigger
     * bit, which retriggers the square-1 hardware sweep unit and reloads the
     * hardware envelope from the software value. */
    if (ch->modify & 0x01) {
        if (ch->type == 1)
            cgb_sweep_retrigger(ch);
        if (ch->type != 3)
            cgb_hw_env_write(ch);
    }
    ch->modify = 0;
}

/* Advance a square channel's free-running duty phase by one output sample.
 *
 * The square oscillators run continuously on hardware -- an NRx4 trigger does
 * not reset the duty position, so a channel that has been silent resumes at
 * whatever phase the elapsed time puts it (mGBA GBAudioRun catches ch->index
 * up by wall-clock time on the next register write, even when the channel
 * is not playing).  So this runs every sample, on or off, muted or not, and
 * uses the frequency register's current value (0 = 64 Hz, the reset state)
 * for the catch-up.  Called before the sample level is read: `bit` is
 * therefore the phase this sample sits at, mirroring GBAudioRun's
 * advance-then-sample order (a one-sample shift either way is inaudible).
 *
 * The CGB frequency register value is 2048 - (131072 / freq_hz), so
 * freq_hz = 131072 / (2048 - reg_value); one full period is 2^32 in the
 * 32-bit accumulator whose top three bits index the 8-step duty pattern.
 * The increment is constant between (tick-rate) frequency changes, so it is
 * cached and only recomputed when ch->frequency changes. */
static void cgb_square_advance(M4ACGBChannel *ch, float sampleRate)
{
    if (ch->frequency != ch->phaseIncFreq) {
        int32_t freqReg = ch->frequency;
        if (freqReg >= 2048) freqReg = 2047;
        float freqHz = 131072.0f / (float)(2048 - freqReg);
        ch->phaseInc = (uint32_t)(freqHz / sampleRate * 4294967296.0f);
        ch->phaseIncFreq = ch->frequency;
    }
    ch->phase += ch->phaseInc;
}

/*
 * CGB channel render - generates one output sample by software synthesis
 */
void m4a_cgb_channel_render(M4ACGBChannel *ch, int32_t *mixL, int32_t *mixR,
                            float sampleRate)
{
    /* Squares free-run whether or not the channel is sounding. */
    if (ch->type == 1 || ch->type == 2)
        cgb_square_advance(ch, sampleRate);

    if (!(ch->status & CHN_ON)) {
        /* Wave channel declick: linearly fade the last sample to zero over
         * DECLICK_SAMPLES frames to prevent a pop caused by the DC offset. */
        if (ch->type == 3 && ch->declickSamplesRemaining > 0) {
            ch->declickSamplesRemaining--;
            int32_t faded = (ch->declickSample * (int32_t)ch->declickSamplesRemaining) / DECLICK_SAMPLES;
            if (ch->pan & 0x0F) *mixR += faded;
            if (ch->pan & 0xF0) *mixL += faded;
        }
        return;
    }
    if (ch->status & CHN_START)
        return;

    int32_t sample = 0;
    uint8_t cgbType = ch->type;

    /* Advance the hardware envelope's free-running 64 Hz clock (frame
     * sequencer rate, independent of the engine tick).  One volume step per
     * `stepTime` clocks; the unit stops at the 0/15 bound (mGBA
     * _updateEnvelope) until the next MO_VOL write reloads it. */
    if (cgbType != 3) {
        ch->hwEnvClockAccum += 64.0f / sampleRate;
        while (ch->hwEnvClockAccum >= 1.0f) {
            ch->hwEnvClockAccum -= 1.0f;
            if (ch->hwEnvDead || (ch->hwEnvStepDir & 0x07) == 0)
                continue;
            if (--ch->hwEnvNextStep != 0)
                continue;
            if (ch->hwEnvStepDir & 0x08) {
                if (++ch->hwEnvVolume >= 15) {
                    ch->hwEnvVolume = 15;
                    ch->hwEnvDead = 1;
                } else {
                    ch->hwEnvNextStep = ch->hwEnvStepDir & 0x07;
                }
            } else {
                if (--ch->hwEnvVolume == 0)
                    ch->hwEnvDead = 1;
                else
                    ch->hwEnvNextStep = ch->hwEnvStepDir & 0x07;
            }
        }
    }

    if (cgbType == 1) {
        /* Advance the frequency sweep's 128 Hz clock (frame-sequencer rate,
         * independent of the engine tick). */
        if (ch->sweepEnabled) {
            ch->sweepClockAccum += 128.0f / sampleRate;
            while (ch->sweepClockAccum >= 1.0f) {
                ch->sweepClockAccum -= 1.0f;
                cgb_sweep_clock(ch);
            }
        }
        /* An overflowed upward sweep has cleared the channel's NR52 enable
         * bit: no output until a retrigger re-evaluates the overflow check
         * (the software envelope keeps running, unaware). */
        if (ch->sweepMuted)
            return;
    }

    if (cgbType == 1 || cgbType == 2) {
        /* Square wave synthesis: read the duty level at the phase
         * cgb_square_advance() already moved us to for this sample. */
        static const uint8_t dutyPatterns[4] = { 0x01, 0x81, 0xE1, 0x7E };
        uint8_t pattern = dutyPatterns[ch->dutyCycle & 3];
        int bit = (ch->phase >> 29) & 7;
        sample = (pattern & (1 << bit)) ? 64 : -64;
    } else if (cgbType == 3) {
        /* Programmable wave channel */
        if (ch->wavePointer) {
            /* 32 4-bit samples packed into 16 bytes (4 uint32_t) */
            uint8_t *waveData = (uint8_t *)ch->wavePointer;
            int pos = (ch->phase >> 27) & 0x1F;  /* 5 bits = 0-31 */
            uint8_t nibble;
            if (pos & 1)
                nibble = waveData[pos >> 1] & 0x0F;
            else
                nibble = (waveData[pos >> 1] >> 4) & 0x0F;
            /* Sum of all 32 raw nibbles for DC offset removal.  The wave mean
             * varies per waveform, so using a fixed midpoint of 8 leaves a DC
             * offset that causes clicks at note start/end.  The sum depends
             * only on the wave table contents, so it is cached and recomputed
             * only when wavePointer changes. */
            if (ch->wavePointer != ch->waveSumPointer) {
                int32_t waveSum = 0;
                for (int i = 0; i < 16; i++) {
                    waveSum += (waveData[i] >> 4) & 0x0F;
                    waveSum += waveData[i] & 0x0F;
                }
                ch->waveSum = waveSum;
                ch->waveSumPointer = ch->wavePointer;
            }
            int32_t waveSum = ch->waveSum;

            /* Volume control: apply shift to raw 4-bit nibble to match
             * GBA hardware quantization (NR32 register).
             * On real hardware, the right-shift is lossy on the small
             * 4-bit value, creating quantized "plateaus" in the output.
             * Apply the same volume logic to the mean for accurate DC removal. */
            int32_t shifted = (int32_t)nibble;
            /* envelopeVolume can exceed 15 (CgbModVol's center-pan goal is
             * unclamped, up to 31); hardware truncates on the register write,
             * so only the low 4 bits are audible. */
            int nr32 = gCgb3Vol[ch->envelopeVolume & 0x0F];
            int32_t meanShifted;
            if (nr32 == 0) {
                shifted = 0;
                meanShifted = 0;
            } else if (nr32 & 0x80) {
                /* GBA 75% mode: (nibble * 3) >> 2; mean = (waveSum * 3) >> 7 */
                shifted = (shifted + (shifted << 1)) >> 2;
                meanShifted = (waveSum * 3) >> 7;
            } else {
                int shift = ((nr32 >> 5) & 3) - 1;
                shifted >>= shift;
                meanShifted = waveSum >> (5 + shift);
            }
            /* Center around the waveform's actual mean to remove DC offset */
            sample = (shifted - meanShifted) * 8;

            /* Advance phase (cached; recomputed only on frequency change). */
            if (ch->frequency != ch->phaseIncFreq) {
                int32_t freqReg = ch->frequency;
                if (freqReg >= 2048) freqReg = 2047;
                float freqHz = 2097152.0f / (float)(2048 - freqReg);
                /* Wave channel plays 32 samples per period */
                freqHz /= 32.0f;
                ch->phaseInc = (uint32_t)(freqHz / sampleRate * 4294967296.0f);
                ch->phaseIncFreq = ch->frequency;
            }
            ch->phase += ch->phaseInc;
        }
    } else if (cgbType == 4) {
        /* Noise channel using LFSR.
         *
         * The LFSR clocks at up to 524288 Hz -- typically far above the
         * output sample rate -- so a single output sample spans many LFSR
         * steps.  Point-sampling the LFSR once per output sample aliases
         * that fast clock into a full-scale two-level square ("rigid" noise);
         * hardware recordings and mGBA (_coalesceNoiseChannel box-averages
         * every LFSR step inside each output sample) show the band-limited
         * mean instead.  Walk every LFSR step that falls inside this sample
         * and output the time-weighted average of the two-level waveform.
         *
         * For noise, ch->phaseInc holds Q16.16 LFSR clocks per output sample
         * and ch->phase holds the Q16 fraction (always < 0x10000) of the
         * current LFSR period already elapsed. */
        uint8_t noiseParams = ch->frequency & 0xFF;

        /* Phase increment is constant between frequency changes; cache it. */
        if (ch->frequency != ch->phaseIncFreq) {
            uint8_t divRatio = noiseParams & 0x07;
            uint8_t shiftFreq = (noiseParams >> 4) & 0x0F;
            /* bool shortMode = (noiseParams >> 3) & 1; */

            float baseFreq = 524288.0f;
            float divisor = (divRatio == 0) ? 0.5f : (float)divRatio;
            float noiseFreq = baseFreq / divisor / (float)(1 << (shiftFreq + 1));

            ch->phaseInc = (uint32_t)(noiseFreq / sampleRate * 65536.0f);
            ch->phaseIncFreq = ch->frequency;
        }

        uint32_t remaining = ch->phaseInc;
        uint32_t untilClock = 0x10000u - ch->phase;
        if (remaining < untilClock) {
            /* No LFSR clock lands inside this sample: constant level. */
            sample = (ch->lfsr & 1) ? 64 : -64;
            ch->phase += remaining;
        } else {
            /* One or more LFSR clocks inside this sample: accumulate each
             * level weighted by the time it was held.
             * Bit 3 of frequency = period mode: 0 = 15-bit, 1 = 7-bit LFSR. */
            int32_t acc = (int32_t)untilClock * ((ch->lfsr & 1) ? 64 : -64);
            remaining -= untilClock;
            for (;;) {
                uint16_t bit = ((ch->lfsr >> 1) ^ ch->lfsr) & 1;
                if (noiseParams & 0x08)
                    ch->lfsr = (ch->lfsr >> 1) | (bit << 6);   /* 7-bit */
                else
                    ch->lfsr = (ch->lfsr >> 1) | (bit << 14);  /* 15-bit */
                if (remaining < 0x10000u)
                    break;
                acc += (int32_t)(0x10000u * ((ch->lfsr & 1) ? 64 : -64));
                remaining -= 0x10000u;
            }
            acc += (int32_t)remaining * ((ch->lfsr & 1) ? 64 : -64);
            ch->phase = remaining;
            sample = acc / (int32_t)ch->phaseInc;
        }
    }

    /* Apply envelope volume (for non-wave channels).
     * The audible level is the HARDWARE envelope unit's volume, reloaded from
     * the software envelope at every MO_VOL write and stepping on its own
     * 64 Hz clock in between (the software value alone would hold percussive
     * envelopes too long -- see hwEnv* in the channel struct). */
    if (cgbType != 3)
        sample = (sample * ch->hwEnvVolume) >> 4;

    /* Scale CGB to match the GBA hardware mixing ratio.
     * SOUNDCNT_H is initialised with SOUND_ALL_MIX_FULL (volume bits = 2), so
     * mGBA applies psgShift = 4 - 2 = 2 (CGB >> 2) while PCM is << 2.
     * That is a 16:1 ratio; >> 2 here keeps us in the same integer domain as
     * the PCM mixer which already incorporates the << 2 implicitly through its
     * larger sample values (~±127 vs CGB's ~±60). */
    sample >>= 1;

    /* Track last sample for wave channel declick on note-off. */
    if (cgbType == 3)
        ch->declickSample = sample;

    /* Route to left/right using NR51-style pan bits in ch->pan.
     * Panning is binary on the GBA (NR51 enable bits), not a level multiplier. */
    if (ch->pan & 0x0F)
        *mixR += sample;
    if (ch->pan & 0xF0)
        *mixL += sample;
}
