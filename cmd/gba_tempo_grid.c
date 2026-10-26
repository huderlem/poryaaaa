/*
 * gba_tempo_grid - the ROM's song-tick grid for poryaaaa_render --midi
 * (see gba_tempo_grid.h; docs/ACCURACY.md, the renderer's "as the game" mode).
 */
#include <math.h>
#include "m4a_engine.h"
#include "gba_tempo_grid.h"

void gba_tempo_grid_init(GbaTempoGrid *g, uint32_t division, uint32_t clocksPerBeat)
{
    g->division      = division ? division : 24;
    g->clocksPerBeat = clocksPerBeat;
    g->tempoI        = 150;   /* MPlayStart: tempoD = tempoI = 150, tempoC = 0 */
    g->tempoC        = 0;
    g->frame         = -1;
    g->lastFrame     = 0;
    g->nextClock     = 0;
    g->frameFirstClock = 0;
    g->frozen        = false;
}

uint64_t gba_tempo_grid_clock(const GbaTempoGrid *g, uint64_t midiTick)
{
    /* midi.cpp ConvertTimes: (24 * g_clocksPerBeat * time) / g_midiTimeDiv */
    return (uint64_t)g->clocksPerBeat * midiTick / g->division;
}

uint64_t gba_tempo_grid_pass_clock(const GbaTempoGrid *g, uint64_t tick, uint64_t pass,
                                   uint64_t loopStartTick, uint64_t loopEndTick)
{
    uint64_t c = gba_tempo_grid_clock(g, tick);
    if (pass > 0)
        c += pass * (gba_tempo_grid_clock(g, loopEndTick) - gba_tempo_grid_clock(g, loopStartTick));
    return c;
}

int64_t gba_tempo_grid_frame(GbaTempoGrid *g, uint64_t clock)
{
    while (g->nextClock <= clock) {
        /* MPlayMain: tempoC += tempoI once per frame, then a tick per 150.
         * With tempoI 0 and tempoC short of 150 no frame plays a tick again,
         * and the TEMPO that could change it is in one of those ticks. */
        if (g->tempoI == 0 && g->tempoC < 150)
            g->frozen = true;
        if (g->frozen)
            return GBA_TEMPO_GRID_NEVER;
        if (g->tempoC < 150)
            g->frameFirstClock = g->nextClock;   /* this tick opens a new frame */
        while (g->tempoC < 150) {
            g->frame++;
            g->tempoC += g->tempoI;
        }
        g->tempoC -= 150;
        g->lastFrame = g->frame;
        g->nextClock++;
    }
    return g->lastFrame;
}

uint32_t gba_tempo_grid_tick_in_frame(const GbaTempoGrid *g, uint64_t clock)
{
    return clock > g->frameFirstClock ? (uint32_t)(clock - g->frameFirstClock) : 0;
}

void gba_tempo_grid_set_tempo(GbaTempoGrid *g, uint32_t usPerBeat)
{
    g->tempoI = gba_tempo_grid_tempo_i(usPerBeat, g->clocksPerBeat);
}

uint32_t gba_tempo_grid_tempo_i(uint32_t usPerBeat, uint32_t clocksPerBeat)
{
    if (usPerBeat == 0)
        usPerBeat = 1;
    /* agb.cpp: TEMPO , round(60000000.0f / us)*tbs/2 -- the division in
     * float, the *tbs/2 by the assembler (integer, into a .byte); ply_tempo:
     * tempoD = byte << 1, tempoI = tempoD * tempoU >> 8 with tempoU 0x100. */
    uint32_t bpm  = (uint32_t)roundf(60000000.0f / (float)usPerBeat);
    uint32_t tbs  = clocksPerBeat / 24;
    uint32_t byte = (bpm * tbs / 2) & 0xFF;
    return byte << 1;
}

uint64_t gba_tempo_grid_sample(int64_t frame, double sampleRate)
{
    if (frame == GBA_TEMPO_GRID_NEVER)
        return UINT64_MAX / 4;
    return (uint64_t)((double)(ROM_FIRST_TICK_CYCLE - GBA_BLIP_FRAME
                               + frame * (int64_t)GBA_CYCLES_PER_FRAME)
                      * sampleRate / (double)GBA_CPU_HZ);
}

uint64_t gba_tempo_grid_dispatch_sample(GbaTempoGrid *g, uint64_t exactSample,
                                        uint64_t clock, uint32_t tempoUs, double sampleRate)
{
    if (!g)
        return exactSample;
    int64_t frame = gba_tempo_grid_frame(g, clock);
    uint64_t s = gba_tempo_grid_sample(frame, sampleRate) - gba_tempo_grid_sample(0, sampleRate);
    if (tempoUs != 0 && !g->frozen)
        gba_tempo_grid_set_tempo(g, tempoUs);
    return s;
}
