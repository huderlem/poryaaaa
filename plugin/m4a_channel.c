#include "m4a_channel.h"
#include "m4a_tables.h"
#include <string.h>

/*
 * PCM Channel Implementation
 * Matches the SoundMainRAM mixer in m4a_1.s
 */

void m4a_pcm_channel_start(M4APCMChannel *ch, WaveData *wav, uint8_t type)
{
    ch->wav = wav;
    ch->type = type;
    ch->currentPointer = wav->data;
    ch->count = wav->size;
    ch->fw = 0;
    ch->envelopeVolume = 0;

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
        /* Pseudo-echo countdown */
        ch->pseudoEchoLength--;
        if (ch->pseudoEchoLength == 0) {
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
            envVol += ch->attack;
            if (envVol >= 0xFF) {
                envVol = 0xFF;
                ch->status--;  /* attack -> decay */
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
    if (!(ch->status & CHN_ON) || ch->status & CHN_START)
        return;

    int8_t *ptr = ch->currentPointer;
    uint32_t fw = ch->fw;
    int32_t count = ch->count;

    if (ch->type & VOICE_TYPE_FIX) {
        /* Fixed-frequency (no resample): no interpolation.
         * Advance via fw accumulator at GBA PCM rate - same mechanics as the
         * interpolating path below, but reading only ptr[0]. */
        int8_t sample = ptr[0];
        int32_t ampR = (int32_t)sample * ch->envelopeVolumeRight;
        int32_t ampL = (int32_t)sample * ch->envelopeVolumeLeft;
        *mixR += ampR >> 8;
        *mixL += ampL >> 8;

        fw += ch->frequency;
        uint32_t advance = fw >> 23;
        if (advance) {
            fw &= 0x7FFFFF;
            count -= advance;
            if (count <= 0) {
                if (ch->isLoop && ch->loopLen > 0) {
                    while (count <= 0)
                        count += ch->loopLen;
                    ptr = ch->loopStart + (ch->loopLen - count);
                } else {
                    ch->status = 0;
                    goto done;
                }
            } else {
                ptr += advance;
            }
        }
    } else {
        /* Interpolating mixer */
        int8_t s0 = ptr[0];
        int8_t s1 = ptr[1];
        int32_t diff = s1 - s0;

        /* Linear interpolation using top bits of fw as fraction */
        int32_t sample = s0 + ((diff * (int32_t)(fw >> 15)) >> 8);
        /* Note: the actual GBA uses fw bits [22:15] as an 8-bit fraction
         * via: mul lr, r9, r1 / add lr, r0, lr, asr 23 */
        /* Simpler equivalent: */
        sample = s0 + (int32_t)(((int64_t)diff * (int32_t)fw) >> 23);

        int32_t ampR = sample * ch->envelopeVolumeRight;
        int32_t ampL = sample * ch->envelopeVolumeLeft;
        *mixR += ampR >> 8;
        *mixL += ampL >> 8;

        /* Advance position */
        fw += ch->frequency;
        uint32_t advance = fw >> 23;
        if (advance) {
            fw &= 0x7FFFFF;  /* keep fractional part */
            count -= advance;
            if (count <= 0) {
                if (ch->isLoop && ch->loopLen > 0) {
                    /* Wrap around loop */
                    while (count <= 0)
                        count += ch->loopLen;
                    ptr = ch->loopStart + (ch->loopLen - count);
                } else {
                    ch->status = 0;
                    goto done;
                }
            } else {
                ptr += advance;
            }
        }
    }

done:
    ch->currentPointer = ptr;
    ch->fw = fw;
    ch->count = count;
}

/*
 * CGB Channel Implementation
 * Matches CgbSound() in m4a.c
 */

void m4a_cgb_channel_start(M4ACGBChannel *ch)
{
    ch->status = CHN_ENV_ATTACK;
    ch->modify = 0x03; /* pitch + vol */
    ch->phase = 0;
    ch->envelopeCounter = ch->attack;
    if (ch->attack == 0) {
        /* Skip attack if instantaneous */
        ch->envelopeVolume = ch->envelopeGoal;
        ch->status = CHN_ENV_DECAY;
        ch->envelopeCounter = ch->decay;
        if (ch->decay == 0) {
            /* Skip decay too */
            if (ch->sustain == 0) {
                ch->status = CHN_ENV_RELEASE;
            } else {
                ch->envelopeVolume = ch->sustainGoal;
                ch->status = CHN_ENV_SUSTAIN;
            }
        }
    } else {
        ch->envelopeVolume = 0;
    }

    /* Initialize LFSR for noise channel */
    if (ch->type == 4) {
        ch->lfsr = 0x7FFF;
    }
}

void m4a_cgb_channel_stop(M4ACGBChannel *ch)
{
    ch->status = 0;
}

/*
 * CGB mod vol calculation - matches CgbModVol in m4a.c
 */
void m4a_cgb_mod_vol(M4ACGBChannel *ch)
{
    ch->pan = 0xFF;
    ch->envelopeGoal = (uint32_t)(ch->leftVolume + ch->rightVolume) / 16;
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

    if (ch->status & CHN_START) {
        if (ch->status & CHN_STOP) {
            ch->status = 0;
            return;
        }
        ch->status = CHN_ENV_ATTACK;
        ch->modify = 0x03;
        m4a_cgb_mod_vol(ch);
        ch->envelopeCounter = ch->attack;
        if (ch->attack != 0) {
            ch->envelopeVolume = 0;
        } else {
            /* skip attack */
            ch->envelopeVolume = ch->envelopeGoal;
            ch->status = CHN_ENV_DECAY;
            ch->envelopeCounter = ch->decay;
            if (ch->decay == 0) {
                if (ch->sustain == 0) {
                    goto pseudo_echo;
                }
                ch->status = CHN_ENV_SUSTAIN;
                ch->envelopeVolume = ch->sustainGoal;
            }
        }
        goto step_complete;
    }

    if (ch->status & CHN_IEC) {
        ch->pseudoEchoLength--;
        if ((int8_t)ch->pseudoEchoLength <= 0) {
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
            goto step_complete;
        } else {
            goto pseudo_echo;
        }
    }

    {
        int doubleStep = (c15 == 0) ? 1 : 0;
        int steps = 0;

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
                        goto envelope_complete;
                    } else {
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
                        ch->envelopeVolume = ch->envelopeGoal;
                    } else {
                        if (ch->sustain == 0) {
                            ch->status &= ~CHN_ENV_MASK;
                            goto pseudo_echo;
                        }
                        ch->status--;
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
    ch->modify = 0;
}

/*
 * CGB channel render - generates one output sample by software synthesis
 */
void m4a_cgb_channel_render(M4ACGBChannel *ch, int32_t *mixL, int32_t *mixR,
                            float sampleRate)
{
    if (!(ch->status & CHN_ON) || (ch->status & CHN_START))
        return;

    int32_t sample = 0;
    uint8_t cgbType = ch->type;

    if (cgbType == 1 || cgbType == 2) {
        /* Square wave synthesis */
        static const uint8_t dutyPatterns[4] = { 0x01, 0x81, 0xE1, 0x7E };
        uint8_t pattern = dutyPatterns[ch->dutyCycle & 3];
        int bit = (ch->phase >> 29) & 7;
        sample = (pattern & (1 << bit)) ? 64 : -64;

        /* Advance phase.
         * CGB frequency register value is used to compute the actual frequency.
         * The CGB freq register value = 2048 - (131072 / freq_hz).
         * So freq_hz = 131072 / (2048 - reg_value).
         * We convert to a phase increment for our 32-bit accumulator. */
        int32_t freqReg = ch->frequency;
        if (freqReg >= 2048) freqReg = 2047;
        float freqHz = 131072.0f / (float)(2048 - freqReg);
        uint32_t phaseInc = (uint32_t)(freqHz / sampleRate * 4294967296.0f);
        ch->phase += phaseInc;
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
            sample = ((int32_t)nibble - 8) * 8;  /* center and scale */

            /* Volume control uses gCgb3Vol table */
            int volShift = gCgb3Vol[ch->envelopeVolume];
            if (volShift == 0)
                sample = 0;
            else
                sample = (sample * volShift) >> 7;

            /* Advance phase */
            int32_t freqReg = ch->frequency;
            if (freqReg >= 2048) freqReg = 2047;
            float freqHz = 2097152.0f / (float)(2048 - freqReg);
            /* Wave channel plays 32 samples per period */
            freqHz /= 32.0f;
            uint32_t phaseInc = (uint32_t)(freqHz / sampleRate * 4294967296.0f);
            ch->phase += phaseInc;
        }
    } else if (cgbType == 4) {
        /* Noise channel using LFSR */
        sample = (ch->lfsr & 1) ? 64 : -64;

        /* Advance LFSR at noise frequency rate */
        uint8_t noiseParams = ch->frequency & 0xFF;
        uint8_t divRatio = noiseParams & 0x07;
        uint8_t shiftFreq = (noiseParams >> 4) & 0x0F;
        /* bool shortMode = (noiseParams >> 3) & 1; */

        float baseFreq = 524288.0f;
        float divisor = (divRatio == 0) ? 0.5f : (float)divRatio;
        float noiseFreq = baseFreq / divisor / (float)(1 << (shiftFreq + 1));

        uint32_t phaseInc = (uint32_t)(noiseFreq / sampleRate * 4294967296.0f);
        uint32_t oldPhase = ch->phase;
        ch->phase += phaseInc;

        /* Clock LFSR for each wrap of relevant phase bits */
        if (ch->phase < oldPhase) {
            uint16_t bit = ((ch->lfsr >> 1) ^ ch->lfsr) & 1;
            ch->lfsr = (ch->lfsr >> 1) | (bit << 14);
        }
    }

    /* Apply envelope volume (for non-wave channels) */
    if (cgbType != 3) {
        sample = (sample * ch->envelopeVolume) >> 4;
    }

    /* Apply panning */
    *mixR += (sample * ch->rightVolume) >> 8;
    *mixL += (sample * ch->leftVolume) >> 8;
}
