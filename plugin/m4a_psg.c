/* Copyright (c) 2013-2016 Jeffrey Pfau (mGBA, https://mgba.io)
 * Copyright (c) 2026 Marcus Huderle
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file is derived from mGBA 0.10.5 (src/gb/audio.c, src/gba/audio.c,
 * src/gba/io.c) and is therefore licensed under the MPL 2.0, not under the
 * MIT license that covers the rest of this project.  See
 * THIRD_PARTY_LICENSES.md and licenses/MPL-2.0.txt. */
/*
 * GBA PSG model: a port of mgba 0.10.5 src/gb/audio.c (GB_AUDIO_GBA style)
 * plus the GBA-side register glue from src/gba/audio.c and src/gba/io.c.
 * Function-by-function correspondence is noted in the comments; the only
 * structural changes are int64 timestamps (mgba's wrap every 128 s) and the
 * removal of the DMG/CGB-only paths (noise coalescing, DMG wave quirks).
 */
#include "m4a_psg.h"
#include <string.h>

static const int sSquareDuty[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0 },
};

#define IO_INDEX(reg) (((reg) - 0x60) >> 1)
#define NR52_IMAGE(p) ((p)->io[IO_INDEX(PSG_REG_NR52)])

/* ---- envelope / sweep helpers (mgba statics of the same names) ---- */

static void update_envelope_dead(M4APsgEnvelope *e)
{
    if (!e->stepTime) {
        e->dead = e->currentVolume ? 1 : 2;
    } else if (!e->direction && !e->currentVolume) {
        e->dead = 2;
    } else if (e->direction && e->currentVolume == 0xF) {
        e->dead = 1;
    } else if (e->dead) {
        /* GB_AUDIO_GBA reloads the step when a dead envelope comes back. */
        e->nextStep = e->stepTime;
        e->dead = 0;
    }
}

static bool reset_envelope(M4APsgEnvelope *e)
{
    e->currentVolume = e->initialVolume;
    e->nextStep = e->stepTime;
    update_envelope_dead(e);
    return e->initialVolume || e->direction;
}

static void reset_sweep(M4APsgSweep *s)
{
    s->step = s->time;
    s->enable = (s->step != 8) || s->shift;
    s->occurred = false;
}

static bool write_sweep(M4APsgSweep *s, uint8_t value)
{
    s->shift = value & 7;
    bool oldDirection = s->direction;
    s->direction = (value >> 3) & 1;
    bool on = true;
    if (s->occurred && oldDirection && !s->direction)
        on = false;
    s->occurred = false;
    s->time = (value >> 4) & 7;
    if (!s->time)
        s->time = 8;
    return on;
}

static void write_duty(M4APsgEnvelope *e, uint8_t value)
{
    e->length = value & 0x3F;
    e->duty = value >> 6;
}

static bool write_envelope(M4APsgEnvelope *e, uint8_t value)
{
    e->stepTime = value & 7;
    e->direction = (value >> 3) & 1;
    e->initialVolume = value >> 4;
    if (!e->stepTime) {
        /* "zombie" mode: DMG/CGB only; the GBA style leaves the volume. */
        e->currentVolume &= 0xF;
    }
    update_envelope_dead(e);
    return e->initialVolume || e->direction;
}

static void update_square_sample(M4APsgSquare *ch)
{
    ch->sample = (int8_t)(sSquareDuty[ch->envelope.duty][ch->index] * ch->envelope.currentVolume);
}

static void update_envelope(M4APsgEnvelope *e)
{
    if (e->direction)
        ++e->currentVolume;
    else
        --e->currentVolume;
    if (e->currentVolume >= 15) {
        e->currentVolume = 15;
        e->dead = 1;
    } else if (e->currentVolume <= 0) {
        e->currentVolume = 0;
        e->dead = 2;
    } else {
        e->nextStep = e->stepTime;
    }
}

static bool update_sweep(M4APsgSquare *ch, bool initial)
{
    if (initial || ch->sweep.time != 8) {
        int frequency = ch->sweep.realFrequency;
        if (ch->sweep.direction) {
            frequency -= frequency >> ch->sweep.shift;
            if (!initial && frequency >= 0) {
                ch->frequency = frequency;
                ch->sweep.realFrequency = frequency;
            }
        } else {
            frequency += frequency >> ch->sweep.shift;
            if (frequency < 2048) {
                if (!initial && ch->sweep.shift) {
                    ch->frequency = frequency;
                    ch->sweep.realFrequency = frequency;
                    if (!update_sweep(ch, true))
                        return false;
                }
            } else {
                return false;
            }
        }
        ch->sweep.occurred = true;
    }
    ch->sweep.step = ch->sweep.time;
    return true;
}

/* ---- GBAudioRun / GBAudioUpdateFrame / GBAudioSamplePSG ---- */

static int wave_volume_shift(int volume)
{
    switch (volume) {
    case 0: return 4;
    case 1: return 0;
    case 2: return 1;
    default:
    case 3: return 2;
    }
}

void m4a_psg_run(M4APsg *p, int64_t timestamp, int channels)
{
    if (!p->enable)
        return;

    M4APsgSquare *sq[2] = { &p->ch1, &p->ch2 };
    bool playing[2] = { p->playingCh1, p->playingCh2 };
    for (int i = 0; i < 2; i++) {
        int bit = 1 << i;
        M4APsgSquare *ch = sq[i];
        if ((channels & bit)
            && ((playing[i] && ch->envelope.dead != 2)
                || timestamp - ch->lastUpdate > 0x40000000
                || channels == bit)) {
            int period = 4 * (2048 - ch->frequency) * PSG_TIMING_FACTOR;
            int64_t diff = timestamp - ch->lastUpdate;
            if (diff >= period) {
                diff /= period;
                ch->index = (uint8_t)((ch->index + diff) & 7);
                ch->lastUpdate += diff * period;
                update_square_sample(ch);
            }
        }
    }

    if (p->playingCh3 && (channels & 0x4)) {
        M4APsgWave *ch = &p->ch3;
        int cycles = 2 * (2048 - ch->rate) * PSG_TIMING_FACTOR;
        int64_t diff = timestamp - ch->nextUpdate;
        if (diff >= 0) {
            diff = (diff / cycles) + 1;
            int volume = wave_volume_shift(ch->volume);
            int start = 7;
            int end = 0;
            int mask = 0x1F;
            if (ch->size) {
                mask = 0x3F;
            } else if (ch->bank) {
                end = 4;
            } else {
                start = 3;
            }
            for (int iter = 0; iter < (int)(diff & mask); ++iter) {
                uint32_t bitsCarry = ch->wavedata32[end] & 0x000000F0;
                uint32_t bits;
                for (int i = start; i >= end; --i) {
                    bits = ch->wavedata32[i] & 0x000000F0;
                    ch->wavedata32[i] = ((ch->wavedata32[i] & 0x0F0F0F0F) << 4)
                                      | ((ch->wavedata32[i] & 0xF0F0F000) >> 12);
                    ch->wavedata32[i] |= bitsCarry << 20;
                    bitsCarry = bits;
                }
                ch->sample = (int8_t)(bitsCarry >> 4);
            }
            if (ch->volume > 3)
                ch->sample += ch->sample << 1;
            ch->sample >>= volume;
            ch->nextUpdate += diff * cycles;
            ch->readable = true;
        }
    }

    if (p->playingCh4 && (channels & 0x8)) {
        M4APsgNoise *ch = &p->ch4;
        int32_t cycles = ch->ratio ? 2 * ch->ratio : 1;
        cycles <<= ch->frequency;
        cycles *= 8 * PSG_TIMING_FACTOR;

        int64_t diff = timestamp - ch->lastEvent;
        if (diff >= cycles) {
            int64_t last;
            int lsb = 0;
            int coeff = 0x60;
            if (!ch->power)
                coeff <<= 8;
            for (last = 0; last + cycles <= diff; last += cycles) {
                lsb = ch->lfsr & 1;
                ch->lfsr >>= 1;
                ch->lfsr ^= lsb * coeff;
            }
            ch->sample = (int8_t)(lsb * ch->envelope.currentVolume);
            ch->lastEvent += last;
        }
    }
}

void m4a_psg_frame(M4APsg *p, int64_t now)
{
    if (!p->enable)
        return;
    m4a_psg_run(p, now, 0x7);

    int frame = (p->frame + 1) & 7;
    p->frame = frame;

    switch (frame) {
    case 2:
    case 6:
        if (p->ch1.sweep.enable) {
            --p->ch1.sweep.step;
            if (p->ch1.sweep.step == 0) {
                if (!update_sweep(&p->ch1, false))
                    p->playingCh1 = false;
                NR52_IMAGE(p) &= ~0x0001;
                NR52_IMAGE(p) |= p->playingCh1;
            }
        }
        /* fall through */
    case 0:
    case 4:
        if (p->ch1.length && p->ch1.stop) {
            --p->ch1.length;
            if (p->ch1.length == 0) {
                p->playingCh1 = 0;
                NR52_IMAGE(p) &= ~0x0001;
            }
        }
        if (p->ch2.length && p->ch2.stop) {
            --p->ch2.length;
            if (p->ch2.length == 0) {
                p->playingCh2 = 0;
                NR52_IMAGE(p) &= ~0x0002;
            }
        }
        if (p->ch3.length && p->ch3.stop) {
            --p->ch3.length;
            if (p->ch3.length == 0) {
                p->playingCh3 = 0;
                NR52_IMAGE(p) &= ~0x0004;
            }
        }
        if (p->ch4.length && p->ch4.stop) {
            --p->ch4.length;
            if (p->ch4.length == 0) {
                p->playingCh4 = 0;
                NR52_IMAGE(p) &= ~0x0008;
            }
        }
        break;
    case 7:
        if (p->playingCh1 && !p->ch1.envelope.dead) {
            --p->ch1.envelope.nextStep;
            if (p->ch1.envelope.nextStep == 0) {
                update_envelope(&p->ch1.envelope);
                update_square_sample(&p->ch1);
            }
        }
        if (p->playingCh2 && !p->ch2.envelope.dead) {
            --p->ch2.envelope.nextStep;
            if (p->ch2.envelope.nextStep == 0) {
                update_envelope(&p->ch2.envelope);
                update_square_sample(&p->ch2);
            }
        }
        if (p->playingCh4 && !p->ch4.envelope.dead) {
            --p->ch4.envelope.nextStep;
            if (p->ch4.envelope.nextStep == 0) {
                int8_t sample = p->ch4.sample;
                update_envelope(&p->ch4.envelope);
                p->ch4.sample = (int8_t)((sample > 0) * p->ch4.envelope.currentVolume);
            }
        }
        break;
    }
}

void m4a_psg_sample(const M4APsg *p, int chMask, int *left, int *right)
{
    int sampleLeft = 0;
    int sampleRight = 0;

    if (chMask & 0x1) {
        if (p->ch1Left) sampleLeft += p->ch1.sample;
        if (p->ch1Right) sampleRight += p->ch1.sample;
    }
    if (chMask & 0x2) {
        if (p->ch2Left) sampleLeft += p->ch2.sample;
        if (p->ch2Right) sampleRight += p->ch2.sample;
    }
    if (chMask & 0x4) {
        if (p->ch3Left) sampleLeft += p->ch3.sample;
        if (p->ch3Right) sampleRight += p->ch3.sample;
    }

    sampleLeft <<= 3;
    sampleRight <<= 3;

    if (chMask & 0x8) {
        int sample = p->ch4.sample << 3;
        if (p->ch4Left) sampleLeft += sample;
        if (p->ch4Right) sampleRight += sample;
    }

    *left = sampleLeft * (1 + p->volumeLeft);
    *right = sampleRight * (1 + p->volumeRight);
}

/* ---- GBAudioWriteNRxx ---- */

static void write_nr10(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x1);
    if (!write_sweep(&p->ch1.sweep, value)) {
        p->playingCh1 = false;
        NR52_IMAGE(p) &= ~0x0001;
    }
}

static void write_nr11(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x1);
    write_duty(&p->ch1.envelope, value);
    p->ch1.length = 64 - p->ch1.envelope.length;
}

static void write_nr12(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x1);
    if (!write_envelope(&p->ch1.envelope, value)) {
        p->playingCh1 = false;
        NR52_IMAGE(p) &= ~0x0001;
    }
}

static void write_nr13(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x1);
    p->ch1.frequency &= 0x700;
    p->ch1.frequency |= value;
}

static void write_nr14(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x1);
    p->ch1.frequency &= 0xFF;
    p->ch1.frequency |= (value & 0x7) << 8;
    bool wasStop = p->ch1.stop;
    p->ch1.stop = (value >> 6) & 1;
    if (!wasStop && p->ch1.stop && p->ch1.length && !(p->frame & 1)) {
        --p->ch1.length;
        if (p->ch1.length == 0)
            p->playingCh1 = false;
    }
    if (value & 0x80) {
        p->playingCh1 = reset_envelope(&p->ch1.envelope);
        p->ch1.sweep.realFrequency = p->ch1.frequency;
        reset_sweep(&p->ch1.sweep);
        if (p->playingCh1 && p->ch1.sweep.shift)
            p->playingCh1 = update_sweep(&p->ch1, true);
        if (!p->ch1.length) {
            p->ch1.length = 64;
            if (p->ch1.stop && !(p->frame & 1))
                --p->ch1.length;
        }
        update_square_sample(&p->ch1);
    }
    NR52_IMAGE(p) &= ~0x0001;
    NR52_IMAGE(p) |= p->playingCh1;
}

static void write_nr21(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x2);
    write_duty(&p->ch2.envelope, value);
    p->ch2.length = 64 - p->ch2.envelope.length;
}

static void write_nr22(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x2);
    if (!write_envelope(&p->ch2.envelope, value)) {
        p->playingCh2 = false;
        NR52_IMAGE(p) &= ~0x0002;
    }
}

static void write_nr23(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x2);
    p->ch2.frequency &= 0x700;
    p->ch2.frequency |= value;
}

static void write_nr24(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x2);
    p->ch2.frequency &= 0xFF;
    p->ch2.frequency |= (value & 0x7) << 8;
    bool wasStop = p->ch2.stop;
    p->ch2.stop = (value >> 6) & 1;
    if (!wasStop && p->ch2.stop && p->ch2.length && !(p->frame & 1)) {
        --p->ch2.length;
        if (p->ch2.length == 0)
            p->playingCh2 = false;
    }
    if (value & 0x80) {
        p->playingCh2 = reset_envelope(&p->ch2.envelope);
        if (!p->ch2.length) {
            p->ch2.length = 64;
            if (p->ch2.stop && !(p->frame & 1))
                --p->ch2.length;
        }
        update_square_sample(&p->ch2);
    }
    NR52_IMAGE(p) &= ~0x0002;
    NR52_IMAGE(p) |= p->playingCh2 << 1;
}

static void write_nr30(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x4);
    p->ch3.enable = (value >> 7) & 1;
    if (!p->ch3.enable) {
        p->playingCh3 = false;
        NR52_IMAGE(p) &= ~0x0004;
    }
}

static void write_nr31(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x4);
    p->ch3.length = 256 - value;
}

static void write_nr33(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x4);
    p->ch3.rate &= 0x700;
    p->ch3.rate |= value;
}

static void write_nr34(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x4);
    p->ch3.rate &= 0xFF;
    p->ch3.rate |= (value & 0x7) << 8;
    bool wasStop = p->ch3.stop;
    p->ch3.stop = (value >> 6) & 1;
    if (!wasStop && p->ch3.stop && p->ch3.length && !(p->frame & 1)) {
        --p->ch3.length;
        if (p->ch3.length == 0)
            p->playingCh3 = false;
    }
    if (value & 0x80) {
        p->playingCh3 = p->ch3.enable;
        if (!p->ch3.length) {
            p->ch3.length = 256;
            if (p->ch3.stop && !(p->frame & 1))
                --p->ch3.length;
        }
        /* The DMG wave-RAM corruption and sample reset do not apply. */
        p->ch3.window = 0;
    }
    if (p->playingCh3) {
        p->ch3.readable = true;
        p->ch3.nextUpdate = now + (6 + 2 * (2048 - p->ch3.rate)) * PSG_TIMING_FACTOR;
    }
    NR52_IMAGE(p) &= ~0x0004;
    NR52_IMAGE(p) |= p->playingCh3 << 2;
}

static void write_nr41(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x8);
    write_duty(&p->ch4.envelope, value);
    p->ch4.length = 64 - p->ch4.envelope.length;
}

static void write_nr42(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x8);
    if (!write_envelope(&p->ch4.envelope, value)) {
        p->playingCh4 = false;
        NR52_IMAGE(p) &= ~0x0008;
    }
}

static void write_nr43(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x8);
    p->ch4.ratio = value & 7;
    p->ch4.frequency = value >> 4;
    p->ch4.power = (value >> 3) & 1;
}

static void write_nr44(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0x8);
    bool wasStop = p->ch4.stop;
    p->ch4.stop = (value >> 6) & 1;
    if (!wasStop && p->ch4.stop && p->ch4.length && !(p->frame & 1)) {
        --p->ch4.length;
        if (p->ch4.length == 0)
            p->playingCh4 = false;
    }
    if (value & 0x80) {
        p->playingCh4 = reset_envelope(&p->ch4.envelope);
        p->ch4.lfsr = p->ch4.power ? 0x7F : 0x7FFF;
        if (!p->ch4.length) {
            p->ch4.length = 64;
            if (p->ch4.stop && !(p->frame & 1))
                --p->ch4.length;
        }
        if (p->playingCh4)
            p->ch4.lastEvent = now;
    }
    NR52_IMAGE(p) &= ~0x0008;
    NR52_IMAGE(p) |= p->playingCh4 << 3;
}

static void write_nr50(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0xF);
    p->volumeRight = value & 7;
    p->volumeLeft = (value >> 4) & 7;
}

static void write_nr51(M4APsg *p, uint8_t value, int64_t now)
{
    m4a_psg_run(p, now, 0xF);
    p->ch1Right = (value >> 0) & 1;
    p->ch2Right = (value >> 1) & 1;
    p->ch3Right = (value >> 2) & 1;
    p->ch4Right = (value >> 3) & 1;
    p->ch1Left = (value >> 4) & 1;
    p->ch2Left = (value >> 5) & 1;
    p->ch3Left = (value >> 6) & 1;
    p->ch4Left = (value >> 7) & 1;
}

static void write_nr52(M4APsg *p, uint8_t value, int64_t now)
{
    bool wasEnable = p->enable;
    p->enable = (value >> 7) & 1;
    if (!p->enable) {
        p->playingCh1 = 0;
        p->playingCh2 = 0;
        p->playingCh3 = 0;
        p->playingCh4 = 0;
        write_nr10(p, 0, now);
        write_nr12(p, 0, now);
        write_nr13(p, 0, now);
        write_nr14(p, 0, now);
        write_nr22(p, 0, now);
        write_nr23(p, 0, now);
        write_nr24(p, 0, now);
        write_nr30(p, 0, now);
        /* GBAudioWriteNR32(0): volume 0 shifts the nibble to nothing. */
        m4a_psg_run(p, now, 0x4);
        p->ch3.volume = 0;
        p->ch3.sample = 0;
        write_nr33(p, 0, now);
        write_nr34(p, 0, now);
        write_nr42(p, 0, now);
        write_nr43(p, 0, now);
        write_nr44(p, 0, now);
        write_nr50(p, 0, now);
        write_nr51(p, 0, now);
        write_nr11(p, 0, now);
        write_nr21(p, 0, now);
        write_nr31(p, 0, now);
        write_nr41(p, 0, now);
        NR52_IMAGE(p) &= ~0x000F;
    } else if (!wasEnable) {
        p->frame = 7;
    }
}

/* ---- GBA-side glue: GBAAudioWriteSOUNDxCNT_x and GBAIOWrite/GBAIOWrite8 ---- */

static void log_write(M4APsg *p, uint32_t reg, uint32_t value)
{
    if (p->writeLog)
        p->writeLog(p->writeLogUser, reg, value);
}

/* GBAIOWrite for the sound registers: the merged 16-bit path.  Returns the
 * value stored in the I/O image. */
static void write16(M4APsg *p, uint32_t addr, uint16_t value, int64_t now)
{
    if (addr >= PSG_REG_NR10 && addr <= PSG_REG_NR50 && !p->enable)
        return; /* most audio registers ignore writes while the unit is off */

    switch (addr) {
    case 0x60:
        write_nr10(p, (uint8_t)value, now);
        value &= 0x007F;
        break;
    case 0x62:
        write_nr11(p, (uint8_t)value, now);
        write_nr12(p, (uint8_t)(value >> 8), now);
        value &= 0xFFC0;
        break;
    case 0x64:
        write_nr13(p, (uint8_t)value, now);
        write_nr14(p, (uint8_t)(value >> 8), now);
        value &= 0x4000;
        break;
    case 0x68:
        write_nr21(p, (uint8_t)value, now);
        write_nr22(p, (uint8_t)(value >> 8), now);
        value &= 0xFFC0;
        break;
    case 0x6C:
        write_nr23(p, (uint8_t)value, now);
        write_nr24(p, (uint8_t)(value >> 8), now);
        value &= 0x4000;
        break;
    case 0x70:
        p->ch3.size = (value >> 5) & 1;
        p->ch3.bank = (value >> 6) & 1;
        write_nr30(p, (uint8_t)value, now);
        value &= 0x00E0;
        break;
    case 0x72:
        write_nr31(p, (uint8_t)value, now);
        p->ch3.volume = ((value >> 8) >> 5) & 7;
        value &= 0xE000;
        break;
    case 0x74:
        write_nr33(p, (uint8_t)value, now);
        write_nr34(p, (uint8_t)(value >> 8), now);
        value &= 0x4000;
        break;
    case 0x78:
        write_nr41(p, (uint8_t)value, now);
        write_nr42(p, (uint8_t)(value >> 8), now);
        value &= 0xFF00;
        break;
    case 0x7C:
        write_nr43(p, (uint8_t)value, now);
        write_nr44(p, (uint8_t)(value >> 8), now);
        value &= 0x40FF;
        break;
    case 0x80:
        write_nr50(p, (uint8_t)value, now);
        write_nr51(p, (uint8_t)(value >> 8), now);
        value &= 0xFF77;
        break;
    case 0x84:
        /* GBAAudioWriteSOUNDCNT_X */
        write_nr52(p, (uint8_t)value, now);
        if (!p->enable) {
            for (uint32_t r = 0x60; r < 0x82; r += 2)
                p->io[IO_INDEX(r)] = 0;
            p->ch3.size = 0;
            p->ch3.bank = 0;
            p->ch3.volume = 0;
        }
        value &= 0x0080;
        value |= NR52_IMAGE(p) & 0xF;
        break;
    default:
        return;
    }
    p->io[IO_INDEX(addr)] = value;
}

void m4a_psg_write8(M4APsg *p, uint32_t reg, uint8_t value, int64_t now)
{
    log_write(p, reg, value);
    uint16_t *img;
    switch (reg) {
    case PSG_REG_NR11:
        write_nr11(p, value, now);
        img = &p->io[IO_INDEX(0x62)];
        *img = (uint16_t)((*img & 0xFF00) | (value & 0xC0));
        break;
    case PSG_REG_NR12:
        write_nr12(p, value, now);
        img = &p->io[IO_INDEX(0x62)];
        *img = (uint16_t)((*img & 0x00C0) | (value << 8));
        break;
    case PSG_REG_NR13:
        write_nr13(p, value, now);
        break;
    case PSG_REG_NR14:
        write_nr14(p, value, now);
        p->io[IO_INDEX(0x64)] = (uint16_t)((value & 0x40) << 8);
        break;
    case PSG_REG_NR21:
        write_nr21(p, value, now);
        img = &p->io[IO_INDEX(0x68)];
        *img = (uint16_t)((*img & 0xFF00) | (value & 0xC0));
        break;
    case PSG_REG_NR22:
        write_nr22(p, value, now);
        img = &p->io[IO_INDEX(0x68)];
        *img = (uint16_t)((*img & 0x00C0) | (value << 8));
        break;
    case PSG_REG_NR23:
        write_nr23(p, value, now);
        break;
    case PSG_REG_NR24:
        write_nr24(p, value, now);
        p->io[IO_INDEX(0x6C)] = (uint16_t)((value & 0x40) << 8);
        break;
    case PSG_REG_NR31:
        write_nr31(p, value, now);
        break;
    case PSG_REG_NR32:
        /* The byte path sets the GBA volume field directly: no GBAudioRun. */
        p->ch3.volume = (value >> 5) & 7;
        p->io[IO_INDEX(0x72)] = (uint16_t)((value & 0xE0) << 8);
        break;
    case PSG_REG_NR33:
        write_nr33(p, value, now);
        break;
    case PSG_REG_NR34:
        write_nr34(p, value, now);
        p->io[IO_INDEX(0x74)] = (uint16_t)((value & 0x40) << 8);
        break;
    case PSG_REG_NR41:
        write_nr41(p, value, now);
        break;
    case PSG_REG_NR42:
        write_nr42(p, value, now);
        p->io[IO_INDEX(0x78)] = (uint16_t)(value << 8);
        break;
    case PSG_REG_NR43:
        write_nr43(p, value, now);
        img = &p->io[IO_INDEX(0x7C)];
        *img = (uint16_t)((*img & 0x4000) | value);
        break;
    case PSG_REG_NR44:
        write_nr44(p, value, now);
        img = &p->io[IO_INDEX(0x7C)];
        *img = (uint16_t)((*img & 0x00FF) | ((value & 0x40) << 8));
        break;
    default: {
        /* Everything else is merged with the other byte of its register and
         * written through the 16-bit path. */
        if (reg < 0x60 || reg > 0x85)
            return;
        uint32_t addr = reg & ~1u;
        int shift = 8 * (reg & 1);
        uint16_t value16 = (uint16_t)(value << shift);
        value16 |= p->io[IO_INDEX(addr)] & (uint16_t)~(0xFF << shift);
        write16(p, addr, value16, now);
        break;
    }
    }
}

void m4a_psg_write_wave_ram(M4APsg *p, int index, uint32_t value, int64_t now)
{
    /* GBAAudioWriteWaveRAM: writes land in the bank that is NOT selected for
     * playback; with the unit off, bank 1. */
    log_write(p, PSG_REG_WAVE_RAM + 4 * (uint32_t)index, value);
    int bank = !p->ch3.bank;
    if (!p->enable)
        bank = 1;
    m4a_psg_run(p, now, 0x4);
    p->ch3.wavedata32[(index & 3) | (bank * 4)] = value;
}

uint8_t m4a_psg_read8(const M4APsg *p, uint32_t reg)
{
    if (reg < 0x60 || reg > 0x85)
        return 0;
    uint32_t addr = reg & ~1u;
    uint16_t v = p->io[IO_INDEX(addr)];
    /* GBAIORead: the channel registers read as 0 while the unit is off. */
    if (addr <= PSG_REG_NR50 && !(NR52_IMAGE(p) & 0x80))
        v = 0;
    return (uint8_t)(v >> (8 * (reg & 1)));
}

void m4a_psg_reset(M4APsg *p)
{
    M4APsgWriteLogFn log = p->writeLog;
    void *logUser = p->writeLogUser;
    memset(p, 0, sizeof(*p));
    p->writeLog = log;
    p->writeLogUser = logUser;
    /* GBAudioReset */
    p->ch1.sweep.time = 8;
    p->ch1.envelope.dead = 2;
    p->ch2.envelope.dead = 2;
    p->ch4.envelope.dead = 2;
    p->frame = 0;
    p->enable = false;
}
