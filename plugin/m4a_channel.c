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
    m4a_pcm_channel_start_at(ch, wav, type, 0);
}

void m4a_pcm_channel_start_at(M4APCMChannel *ch, WaveData *wav, uint8_t type,
                              uint32_t startOffset)
{
    ch->wav = wav;
    ch->type = type;
    /* ply_note stores track->unk_3C (XCMD 0x0D's word, 0 unless set) as the
     * channel's count (m4a_1.s _081DDCCE); SoundMainRAM's SF_START pass
     * (_081DCF6A) turns it into currentPointer = data + count and
     * count = size - count.  Nothing reads either field between the two,
     * so the port applies the pass here.  Reverse and compressed voices
     * are set up by SoundMainRAM_Unk1 on their first mix (CHN_SPECIAL):
     * a compressed voice reads the pointer back as the sample index, so an
     * offset inside a DPCM block decodes that block and starts mid-way. */
    if (startOffset > wav->size)
        startOffset = wav->size;    /* host-memory guard: the GBA would read past the
                                     * sample before its signed count check stopped it;
                                     * here the channel stops on its first step instead */
    ch->currentPointer = wav->data + startOffset;
    ch->count = (int32_t)(wav->size - startOffset);
    ch->fw = 0;
    ch->envelopeVolume = 0;
    ch->xpi = 0xFF000000u;

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
 * The PCM envelope step of SoundMainRAM (m4a_1.s _081DCF6A.._081DD006 and the
 * volume products after it), run once per VBlank frame for each channel just
 * before its mix (m4a_mixer.c).
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
        /* SF_START zeroes fw.  A Golden Sun synth (extension) keeps the
         * oscillator phase m4a_pcm_channel_start primed (the triangle's
         * 90-degree start), since this step now runs right before the
         * channel's first mix and would otherwise wipe it. */
        if (ch->synthType == M4A_SYNTH_NONE)
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
 * Every write is stamped on the CPU clock: the channel pass carries a
 * cycle cursor that starts at the channel's loop head in CgbSound and
 * advances by the cost of each segment of the C code's control flow, as
 * measured in mgba between breakpoints placed on the compiled function
 * (test/accuracy/ref/mgba_probe.c CgbSound_* labels; tables from
 * test/accuracy/tools/probe_cgb.py; docs/ACCURACY.md, appendix A).  The hardware is run up to each write (m4a_gba_run) and
 * the write is preceded by the DAC catch-up mgba performs in GBAAudioSample
 * (m4a_gba_catch_up), so a level change lands on the same 65536 Hz slot it
 * does in mgba.  The segment costs are the CGB_CYC_* constants below, named
 * after the probe's labels; a few paths the corpus never took are marked
 * est. (the neighbouring measured path plus the instruction difference).
 */

/* loop head -> active (SF_ON set) -> step2 (after the register switch) */
#define CGB_CYC_LOOP_ACTIVE        26
#define CGB_CYC_ACTIVE_STEP2_SQ1   69
#define CGB_CYC_ACTIVE_STEP2_SQ2   61
#define CGB_CYC_ACTIVE_STEP2_WAVE  75
#define CGB_CYC_ACTIVE_STEP2_NOISE 69
/* step2 -> start (SF_START) / nostart */
#define CGB_CYC_STEP2_START        24
#define CGB_CYC_STEP2_NOSTART      30
#define CGB_CYC_START_MODVOL       44    /* start -> CgbModVol */
#define CGB_CYC_START_OSCOFF       38    /* SF_START | SF_STOP -> oscillator_off (est.: 24 + the taken branch) */
/* CgbModVol -> the first start write (NR10 / NR21 / NR30 / NR41), + the pan class */
#define CGB_CYC_MODVOL_NR10        135
#define CGB_CYC_MODVOL_NR21        131
#define CGB_CYC_MODVOL_NR30_UPLOAD 147   /* NR30 = 0x40 before the wave RAM upload */
#define CGB_CYC_MODVOL_NR30        153   /* NR30 = 0 straight away */
#define CGB_CYC_MODVOL_NR41        141
/* CgbPan's four paths inside CgbModVol: right >= left and not hard right;
 * hard right; left > right and not hard left; hard left */
#define CGB_CYC_PAN_CR             0
#define CGB_CYC_PAN_R              4
#define CGB_CYC_PAN_CL             6
#define CGB_CYC_PAN_L              10
/* gaps inside the start block */
#define CGB_CYC_NR10_NR11          10
#define CGB_CYC_NR30_WAVE          26
#define CGB_CYC_WAVE_WAVE          15
#define CGB_CYC_WAVE_NR30          6
#define CGB_CYC_NR30_NR31          4
#define CGB_CYC_NR41_NR43          8
/* last start write -> envelope_step_complete: attack != 0 (square / wave,
 * noise), + attack == 0 (envelope_decay_start), + decay == 0 too
 * (envelope_sustain_start) */
#define CGB_CYC_START_STEPDONE_SQ  70
#define CGB_CYC_START_STEPDONE_WN  62
#define CGB_CYC_START_DECAY        40
#define CGB_CYC_START_SUSTAIN      76
/* nostart -> ... */
#define CGB_CYC_NOSTART_STEP       38    /* envelope running (SF_STOP clear) -> envelope_step_repeat */
#define CGB_CYC_NOSTART_STEP_STOP  46    /* SF_STOP set with the envelope already in release */
#define CGB_CYC_NOSTART_RELEASE    84    /* SF_STOP + ENV, release != 0 -> envelope_step_complete (square / noise) */
#define CGB_CYC_NOSTART_RELEASE_W  78    /* ... wave */
#define CGB_CYC_NOSTART_ECHO       114   /* ... release 0, pseudo-echo volume != 0 -> envelope_complete */
#define CGB_CYC_NOSTART_OFF        94    /* ... release 0, no echo -> oscillator_off */
#define CGB_CYC_IEC_CONTINUE       42    /* SF_IEC, length still > 0 -> envelope_complete */
#define CGB_CYC_IEC_EXPIRED        40    /* SF_IEC, expired -> oscillator_off */
/* the envelope step (envelopeCounter 0): step -> modvol -> CgbModVol, then
 * CgbModVol -> envelope_step_complete by phase and transition, + pan class */
#define CGB_CYC_STEP_MODVOL        18
#define CGB_CYC_STEP_MODVOL_W      20    /* wave sets MO_VOL first */
#define CGB_CYC_MODVOL_CALL        10
#define CGB_CYC_STEP_SUSTAIN       147   /* sustain: volume = sustainGoal */
#define CGB_CYC_STEP_RELEASE       155   /* release, volume still > 0 */
#define CGB_CYC_STEP_ATTACK        173   /* attack, goal not reached */
#define CGB_CYC_STEP_DECAY         179   /* decay, sustainGoal not reached */
#define CGB_CYC_STEP_ATTACK_DECAY  211   /* attack -> decay (envelope_decay_start) */
#define CGB_CYC_STEP_DECAY_SUSTAIN 219   /* decay -> sustain (envelope_sustain_start, square / noise) */
#define CGB_CYC_STEP_DECAY_SUSTAIN_W 213 /* ... wave (no envelopeStepTimeAndDir) */
#define CGB_CYC_STEP_ECHO          191   /* release ended -> pseudo-echo start -> envelope_complete */
#define CGB_CYC_STEP_OFF           165   /* release ended, no echo -> oscillator_off */
#define CGB_CYC_STEP_DECAY_OFF     211   /* decay reached a sustain of 0, no echo -> oscillator_off */
#define CGB_CYC_STEP_DECAY_ECHO    237   /* ... with an echo -> envelope_complete (est.: 211 + the 26 of 191 - 165) */
/* attack reached its goal with decay 0: envelope_decay_start falls through
 * to envelope_sustain_start in one pass (est.: the decay -> sustain costs
 * + 10 for the attack branch's goal test and the decay test) */
#define CGB_CYC_STEP_ATTACK_SUSTAIN 229  /* ... sustain != 0 -> envelope_sustain */
#define CGB_CYC_STEP_ATTACK_OFF    221   /* ... sustain 0, no echo -> oscillator_off */
#define CGB_CYC_STEP_ATTACK_ECHO   247   /* ... sustain 0, echo -> envelope_complete */
#define CGB_CYC_STEP_SKIP          12    /* envelopeCounter != 0: step -> envelope_step_complete */
/* envelope_step_complete -> envelope_complete, or back to the step (prevC15 == 0) */
#define CGB_CYC_STEPDONE_ENVDONE   18
#define CGB_CYC_STEPDONE_STEP      24
/* envelope_complete -> the pitch block / the volume block / channel_complete */
#define CGB_CYC_ENVDONE_PIT        10
#define CGB_CYC_ENVDONE_VOL        26
#define CGB_CYC_ENVDONE_DONE       32
#define CGB_CYC_PIT_NRX3           28
#define CGB_CYC_PIT_NR43           32    /* noise reads NR43 first */
#define CGB_CYC_PIT_FIX            40    /* TONEDATA_TYPE_FIX rounding */
#define CGB_CYC_NRX3_NRX4          32
#define CGB_CYC_NR43_NR44          24
#define CGB_CYC_NRX4_VOL           12    /* after the pitch block: -> the volume block */
#define CGB_CYC_NRX4_DONE          18    /* ... or -> channel_complete */
#define CGB_CYC_VOL_NR51           21
#define CGB_CYC_NR51_NRX2          28
#define CGB_CYC_NR51_NR32          32
#define CGB_CYC_NRX2_NRX4          10
#define CGB_CYC_NR32_NR30          16    /* wave retrigger: NR30 = 0x80 then NR34 */
#define CGB_CYC_NR30_NR34          6
#define CGB_CYC_NR32_DONE          20
#define CGB_CYC_NR34_DONE_VOL      18    /* after the retrigger: same as the pitch block's NR34 */
#define CGB_CYC_NRX4_DONE_SQ1      24    /* square 1: the sweep-direction test, bit set */
#define CGB_CYC_NR14_NR14          24    /* square 1 with the sweep direction bit clear writes NR14 again (se_faint's sweep 0x17, milestone 4 slice 1) */
#define CGB_CYC_NR14_DONE          2
#define CGB_CYC_NRX4_DONE_OTHER    12
/* oscillator_off -> CgbOscOff -> its writes -> channel_complete */
#define CGB_CYC_OSCOFF_CALL        14
#define CGB_CYC_OSC_NR12           37
#define CGB_CYC_OSC_NR22           37
#define CGB_CYC_OSC_NR30           51
#define CGB_CYC_OSC_NR42           45
#define CGB_CYC_OSC_NR12_NR14      14
#define CGB_CYC_OSC_NR22_NR24      6
#define CGB_CYC_OSC_NR42_NR44      6
#define CGB_CYC_OSC_DONE           22
/* channel_complete -> next -> the next channel's loop head */
#define CGB_CYC_DONE_NEXT          4
#define CGB_CYC_NEXT_LOOP          16

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
    /* mgba runs every event due at or before an instruction's start cycle
     * before executing it, so a store at T lands after the events due at T. */
    if (io->cursor > io->gba->now)
        m4a_gba_run(io->gba, io->cursor);
}

/* Advance the cursor by a segment's cost, plus the FIFO DMA stall when the
 * segment spans a refill. */
static void cgb_adv(CgbIo *io, int cycles)
{
    io->cursor += cycles + m4a_gba_fifo_dma_stall(io->gba, io->cursor, io->cursor + cycles);
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

/* CgbOscOff.  `io->cursor` is the CgbOscOff entry; the cursor ends at
 * channel_complete. */
static void cgb_osc_off(M4ACGBChannel *ch, CgbIo *io)
{
    switch (ch->type) {
    case 1:
        cgb_adv(io, CGB_CYC_OSC_NR12);
        cgb_write(io, PSG_REG_NR12, 8);
        cgb_adv(io, CGB_CYC_OSC_NR12_NR14);
        cgb_write(io, PSG_REG_NR14, 0x80);
        break;
    case 2:
        cgb_adv(io, CGB_CYC_OSC_NR22);
        cgb_write(io, PSG_REG_NR22, 8);
        cgb_adv(io, CGB_CYC_OSC_NR22_NR24);
        cgb_write(io, PSG_REG_NR24, 0x80);
        break;
    case 3:
        cgb_adv(io, CGB_CYC_OSC_NR30);
        cgb_write(io, PSG_REG_NR30, 0);
        break;
    default:
        cgb_adv(io, CGB_CYC_OSC_NR42);
        cgb_write(io, PSG_REG_NR42, 8);
        cgb_adv(io, CGB_CYC_OSC_NR42_NR44);
        cgb_write(io, PSG_REG_NR44, 0x80);
        break;
    }
    cgb_adv(io, CGB_CYC_OSC_DONE);
}

/* CgbOscOff from outside CgbSound (TrackStop): writes from `*cursor` on. */
void m4a_cgb_osc_off(M4ACGBChannel *ch, M4APsg *psg, M4AGbaAudio *gba, int64_t *cursor)
{
    /* The caller's cursor is the first write; CgbOscOff's own entry is the
     * first write's offset earlier. */
    int lead = ch->type == 1 ? CGB_CYC_OSC_NR12 : ch->type == 2 ? CGB_CYC_OSC_NR22
             : ch->type == 3 ? CGB_CYC_OSC_NR30 : CGB_CYC_OSC_NR42;
    CgbIo io = { psg, gba, *cursor - lead, false };
    cgb_osc_off(ch, &io);
    *cursor = io.cursor - CGB_CYC_OSC_DONE;
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

/* The cost of CgbPan's path inside CgbModVol, from the volumes it compares
 * (the comparison the C code makes; cgb_pan() has already run). */
static int cgb_pan_cost(const M4ACGBChannel *ch)
{
    unsigned rv = ch->rightVolume, lv = ch->leftVolume;
    if (rv >= lv)
        return rv / 2 >= lv ? CGB_CYC_PAN_R : CGB_CYC_PAN_CR;
    return lv / 2 >= rv ? CGB_CYC_PAN_L : CGB_CYC_PAN_CL;
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
        cgb_adv(io, CGB_CYC_NR10_NR11);
        /* fallthrough */
    case 2:
        cgb_write(io, nrx1, (uint8_t)(((uint32_t)ch->dutyCycle << 6) + ch->length));
        goto init_env_step_time_dir;
    case 3:
        if (ch->wavePointer != ch->currentWavePointer) {
            cgb_write(io, nrx0, 0x40);
            cgb_adv(io, CGB_CYC_NR30_WAVE);
            cgb_write_wave_ram(io, 0, ch->wavePointer[0]);
            cgb_adv(io, CGB_CYC_WAVE_WAVE);
            cgb_write_wave_ram(io, 1, ch->wavePointer[1]);
            cgb_adv(io, CGB_CYC_WAVE_WAVE);
            cgb_write_wave_ram(io, 2, ch->wavePointer[2]);
            cgb_adv(io, CGB_CYC_WAVE_WAVE);
            cgb_write_wave_ram(io, 3, ch->wavePointer[3]);
            cgb_adv(io, CGB_CYC_WAVE_NR30);
            ch->currentWavePointer = ch->wavePointer;
        }
        cgb_write(io, nrx0, 0);
        cgb_adv(io, CGB_CYC_NR30_NR31);
        cgb_write(io, nrx1, ch->length);
        if (ch->length)
            ch->n4 = 0xC0;
        else
            ch->n4 = 0x80;
        break;
    default:
        cgb_write(io, nrx1, ch->length);
        cgb_adv(io, CGB_CYC_NR41_NR43);
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
 * `*cursor` is the CPU cycle of the channel's loop head in CgbSound and is
 * moved to the next channel's.
 */
void m4a_cgb_channel_tick(M4ACGBChannel *ch, uint8_t c15, M4APsg *psg, M4AGbaAudio *gba,
                          int64_t *cursor)
{
    CgbIo io = { psg, gba, *cursor, false };
    if (!(ch->status & CHN_ON)) {
        cgb_adv(&io, M4A_CYC_CGB_INACTIVE);
        *cursor = io.cursor;
        return;
    }

    int chn = ch->type;
    uint32_t nrx0, nrx1, nrx2, nrx3, nrx4;
    cgb_regs(chn, &nrx0, &nrx1, &nrx2, &nrx3, &nrx4);

    cgb_adv(&io, CGB_CYC_LOOP_ACTIVE);
    cgb_adv(&io, chn == 1 ? CGB_CYC_ACTIVE_STEP2_SQ1 : chn == 2 ? CGB_CYC_ACTIVE_STEP2_SQ2
              : chn == 3 ? CGB_CYC_ACTIVE_STEP2_WAVE : CGB_CYC_ACTIVE_STEP2_NOISE);

    /* Most comparison operations that cast to s8 perform 'and' by 0xFF. */
    const int mask = 0xff;
    int prevC15 = c15;
    int envelopeStepTimeAndDir = cgb_read(&io, nrx2);
    int envelopeVolume, sustainGoal;
    int pan = 0;                 /* CgbPan's cost on the last CgbModVol */
    int pendStart = 0;           /* start block: last write -> envelope_step_complete */
    int echoFrom = 0;            /* pseudo-echo start reached from: 0 nostart, 1 release end, 2 decay end, 3 attack end (decay 0) */
    int fromAttack = 0;          /* envelope_sustain_start reached from the attack step with decay 0 */

    /* A sound moved to the polyphony-debug shadow pool mid-note: give its
     * PSG the start-frame registers it never saw, then re-apply pitch and
     * volume this frame (a trigger with the current envelope volume). */
    if (ch->hwResync && !(ch->status & CHN_START)) {
        int unused;
        ch->hwResync = false;
        cgb_adv(&io, CGB_CYC_STEP2_START + CGB_CYC_START_MODVOL + CGB_CYC_MODVOL_NR21);
        cgb_start_registers(ch, &io, chn, nrx0, nrx1, nrx3, &unused);
        if (chn == 3)
            ch->n4 |= 0x80;
        ch->modify |= MO_PIT | MO_VOL;
    }

    /* 2. calculate envelope volume */
    if (ch->status & CHN_START) {
        if (!(ch->status & CHN_STOP)) {
            cgb_adv(&io, CGB_CYC_STEP2_START);
            ch->status = CHN_ENV_ATTACK;
            ch->modify = MO_PIT | MO_VOL;
            cgb_adv(&io, CGB_CYC_START_MODVOL);
            m4a_cgb_mod_vol(ch);
            pan = cgb_pan_cost(ch);
            cgb_adv(&io, pan + (chn == 1 ? CGB_CYC_MODVOL_NR10 : chn == 2 ? CGB_CYC_MODVOL_NR21
                              : chn == 3 ? (ch->wavePointer != ch->currentWavePointer ? CGB_CYC_MODVOL_NR30_UPLOAD
                                                                                      : CGB_CYC_MODVOL_NR30)
                              : CGB_CYC_MODVOL_NR41));
            cgb_start_registers(ch, &io, chn, nrx0, nrx1, nrx3, &envelopeStepTimeAndDir);
            pendStart = chn <= 2 ? CGB_CYC_START_STEPDONE_SQ : CGB_CYC_START_STEPDONE_WN;
            ch->envelopeCounter = ch->attack;
            if ((int8_t)(ch->attack & mask)) {
                ch->envelopeVolume = 0;
                cgb_adv(&io, pendStart);
                goto envelope_step_complete;
            } else {
                /* skip attack phase if attack is instantaneous (=0) */
                goto envelope_decay_start;
            }
        } else {
            cgb_adv(&io, CGB_CYC_START_OSCOFF);
            goto oscillator_off;
        }
    } else if (ch->status & CHN_IEC) {
        cgb_adv(&io, CGB_CYC_STEP2_NOSTART);
        ch->pseudoEchoLength--;
        if ((int8_t)(ch->pseudoEchoLength & mask) <= 0) {
            cgb_adv(&io, CGB_CYC_IEC_EXPIRED);
        oscillator_off:
            cgb_adv(&io, CGB_CYC_OSCOFF_CALL);
            cgb_osc_off(ch, &io);
            ch->status = 0;
            goto channel_complete;
        }
        cgb_adv(&io, CGB_CYC_IEC_CONTINUE);
        goto envelope_complete;
    } else if ((ch->status & CHN_STOP) && (ch->status & CHN_ENV_MASK)) {
        cgb_adv(&io, CGB_CYC_STEP2_NOSTART);
        ch->status &= ~CHN_ENV_MASK;
        ch->envelopeCounter = ch->release;
        if ((int8_t)(ch->release & mask)) {
            ch->modify |= MO_VOL;
            if (chn != 3)
                envelopeStepTimeAndDir = ch->release | 0x00; /* CGB_NRx2_ENV_DIR_DEC */
            cgb_adv(&io, chn == 3 ? CGB_CYC_NOSTART_RELEASE_W : CGB_CYC_NOSTART_RELEASE);
            goto envelope_step_complete;
        } else {
            goto envelope_pseudoecho_start;
        }
    } else {
        cgb_adv(&io, CGB_CYC_STEP2_NOSTART);
        cgb_adv(&io, (ch->status & CHN_STOP) ? CGB_CYC_NOSTART_STEP_STOP : CGB_CYC_NOSTART_STEP);
    envelope_step_repeat:
        if (ch->envelopeCounter == 0) {
            if (chn == 3)
                ch->modify |= MO_VOL;
            cgb_adv(&io, (chn == 3 ? CGB_CYC_STEP_MODVOL_W : CGB_CYC_STEP_MODVOL) + CGB_CYC_MODVOL_CALL);
            m4a_cgb_mod_vol(ch);
            pan = cgb_pan_cost(ch);
            echoFrom = 1;
            if ((ch->status & CHN_ENV_MASK) == CHN_ENV_RELEASE) {
                ch->envelopeVolume--;
                if ((int8_t)(ch->envelopeVolume & mask) <= 0) {
                envelope_pseudoecho_start:
                    ch->envelopeVolume = (uint8_t)(((ch->envelopeGoal * ch->pseudoEchoVolume) + 0xFF) >> 8);
                    if (ch->envelopeVolume) {
                        ch->status |= CHN_IEC;
                        ch->modify |= MO_VOL;
                        if (chn != 3)
                            envelopeStepTimeAndDir = 0 | 0x08;
                        cgb_adv(&io, echoFrom == 3 ? CGB_CYC_STEP_ATTACK_ECHO + pan
                                   : echoFrom == 2 ? CGB_CYC_STEP_DECAY_ECHO + pan
                                   : echoFrom == 1 ? CGB_CYC_STEP_ECHO + pan : CGB_CYC_NOSTART_ECHO);
                        goto envelope_complete;
                    } else {
                        cgb_adv(&io, echoFrom == 3 ? CGB_CYC_STEP_ATTACK_OFF + pan
                                   : echoFrom == 2 ? CGB_CYC_STEP_DECAY_OFF + pan
                                   : echoFrom == 1 ? CGB_CYC_STEP_OFF + pan : CGB_CYC_NOSTART_OFF);
                        goto oscillator_off;
                    }
                } else {
                    ch->envelopeCounter = ch->release;
                    cgb_adv(&io, CGB_CYC_STEP_RELEASE + pan);
                }
            } else if ((ch->status & CHN_ENV_MASK) == CHN_ENV_SUSTAIN) {
                cgb_adv(&io, CGB_CYC_STEP_SUSTAIN + pan);
            envelope_sustain:
                ch->envelopeVolume = ch->sustainGoal;
                ch->envelopeCounter = 7;
            } else if ((ch->status & CHN_ENV_MASK) == CHN_ENV_DECAY) {
                ch->envelopeVolume--;
                envelopeVolume = (int8_t)(ch->envelopeVolume & mask);
                sustainGoal = (int8_t)(ch->sustainGoal);
                if (envelopeVolume <= sustainGoal) {
                envelope_sustain_start:
                    if (ch->sustain == 0) {
                        ch->status &= ~CHN_ENV_MASK;
                        if (pendStart) {
                            /* start frame with attack 0, decay 0, sustain 0:
                             * the start block's tail, then the pseudo-echo
                             * decision (est.: the sustain != 0 start cost) */
                            cgb_adv(&io, pendStart + CGB_CYC_START_SUSTAIN);
                            pendStart = 0;
                        }
                        if (fromAttack)
                            echoFrom = 3;
                        else if (echoFrom == 1)
                            echoFrom = 2;
                        goto envelope_pseudoecho_start;
                    } else {
                        ch->status--;
                        ch->modify |= MO_VOL;
                        if (chn != 3)
                            envelopeStepTimeAndDir = 0 | 0x08;
                        if (pendStart)
                            cgb_adv(&io, pendStart + CGB_CYC_START_SUSTAIN);
                        else if (fromAttack)
                            cgb_adv(&io, CGB_CYC_STEP_ATTACK_SUSTAIN + pan);
                        else
                            cgb_adv(&io, (chn == 3 ? CGB_CYC_STEP_DECAY_SUSTAIN_W : CGB_CYC_STEP_DECAY_SUSTAIN) + pan);
                        pendStart = 0;
                        goto envelope_sustain;
                    }
                } else {
                    ch->envelopeCounter = ch->decay;
                    cgb_adv(&io, CGB_CYC_STEP_DECAY + pan);
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
                        if (pendStart)
                            cgb_adv(&io, pendStart + CGB_CYC_START_DECAY);
                        else
                            cgb_adv(&io, CGB_CYC_STEP_ATTACK_DECAY + pan);
                        pendStart = 0;
                    } else {
                        if (!pendStart)
                            fromAttack = 1;   /* one charge at envelope_sustain_start */
                        goto envelope_sustain_start;
                    }
                } else {
                    ch->envelopeCounter = ch->attack;
                    cgb_adv(&io, CGB_CYC_STEP_ATTACK + pan);
                }
            }
        } else {
            cgb_adv(&io, CGB_CYC_STEP_SKIP);
        }
    }

envelope_step_complete:
    /* every 15 frames, envelope calculation has to be done twice
     * to keep up with the hardware envelope rate (1/64 s) */
    ch->envelopeCounter--;
    if (prevC15 == 0) {
        prevC15--;
        cgb_adv(&io, CGB_CYC_STEPDONE_STEP);
        goto envelope_step_repeat;
    }
    cgb_adv(&io, CGB_CYC_STEPDONE_ENVDONE);

envelope_complete:
    /* Opt-in pulse-width modulation (not stock CgbSound): a duty change on a
     * sounding square is written to NRx1, which the PSG applies at once. */
    if ((ch->modify & MO_DUTY) && (chn == 1 || chn == 2))
        cgb_write(&io, nrx1, (uint8_t)(((uint32_t)ch->dutyCycle << 6) + ch->length));

    /* 3. apply pitch to HW registers */
    if (ch->modify & MO_PIT) {
        cgb_adv(&io, CGB_CYC_ENVDONE_PIT);
        /* TONEDATA_TYPE_FIX rounding for the 65536 Hz DAC (SOUNDBIAS_H =
         * 0x40); the engine already applies it when computing ch->frequency
         * (m4a_cgb_fix_freq), and the rounding is idempotent. */
        if (chn < 4 && ch->fixedFreq) {
            ch->frequency = (ch->frequency + 1) & 0x7fe;
            cgb_adv(&io, CGB_CYC_PIT_FIX);
        }
        cgb_adv(&io, chn == 4 ? CGB_CYC_PIT_NR43 : CGB_CYC_PIT_NRX3);
        if (chn != 4)
            cgb_write(&io, nrx3, (uint8_t)ch->frequency);
        else
            cgb_write(&io, nrx3, (uint8_t)((cgb_read(&io, nrx3) & 0x08) | ch->frequency));
        ch->n4 = (uint8_t)((ch->n4 & 0xC0) + ((ch->frequency >> 8) & 0xFF));
        cgb_adv(&io, chn == 4 ? CGB_CYC_NR43_NR44 : CGB_CYC_NRX3_NRX4);
        cgb_write(&io, nrx4, ch->n4);
        cgb_adv(&io, (ch->modify & MO_VOL) ? CGB_CYC_NRX4_VOL : CGB_CYC_NRX4_DONE);
    } else {
        cgb_adv(&io, (ch->modify & MO_VOL) ? CGB_CYC_ENVDONE_VOL : CGB_CYC_ENVDONE_DONE);
    }

    /* 4. apply envelope & volume to HW registers */
    if (ch->modify & MO_VOL) {
        cgb_adv(&io, CGB_CYC_VOL_NR51);
        cgb_write(&io, PSG_REG_NR51,
                  (uint8_t)((cgb_read(&io, PSG_REG_NR51) & ~ch->panMask) | ch->pan));
        if (chn == 3) {
            cgb_adv(&io, CGB_CYC_NR51_NR32);
            cgb_write(&io, nrx2, cgb3_vol(ch->envelopeVolume));
            if (ch->n4 & 0x80) {
                cgb_adv(&io, CGB_CYC_NR32_NR30);
                cgb_write(&io, nrx0, 0x80);
                cgb_adv(&io, CGB_CYC_NR30_NR34);
                cgb_write(&io, nrx4, ch->n4);
                ch->n4 &= 0x7f;
                cgb_adv(&io, CGB_CYC_NR34_DONE_VOL);
            } else {
                cgb_adv(&io, CGB_CYC_NR32_DONE);
            }
        } else {
            uint32_t envMask = 0xF;
            cgb_adv(&io, CGB_CYC_NR51_NRX2);
            cgb_write(&io, nrx2, (uint8_t)((envelopeStepTimeAndDir & envMask) + (ch->envelopeVolume << 4)));
            cgb_adv(&io, CGB_CYC_NRX2_NRX4);
            cgb_write(&io, nrx4, ch->n4 | 0x80);
            if (chn == 1) {
                if (!(cgb_read(&io, nrx0) & 0x08)) {
                    cgb_adv(&io, CGB_CYC_NR14_NR14);
                    cgb_write(&io, nrx4, ch->n4 | 0x80);
                    cgb_adv(&io, CGB_CYC_NR14_DONE);
                } else {
                    cgb_adv(&io, CGB_CYC_NRX4_DONE_SQ1);
                }
            } else {
                cgb_adv(&io, CGB_CYC_NRX4_DONE_OTHER);
            }
        }
    }

channel_complete:
    ch->modify = 0;
    cgb_adv(&io, CGB_CYC_DONE_NEXT + CGB_CYC_NEXT_LOOP);
    *cursor = io.cursor;
}
