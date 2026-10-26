/*
 * gba_tempo_grid - the ROM's song-tick grid for poryaaaa_render --midi
 * (--gba-tempo-grid; docs/ACCURACY.md, the renderer's "as the game" mode).
 *
 * MPlayMain adds tempoI to tempoC once per frame and plays one tick of every
 * track while tempoC >= 150, subtracting 150 each time (m4a_1.s _081DD86C ..
 * _081DD9BC); MPlayStart seeds tempoI = 150, tempoC = 0, so the first
 * MPlayMain (frame 3's SoundMain) plays tick 0, and a TEMPO inside a tick
 * changes only the next frame's add.  mid2agb turns a MIDI tempo into
 * tempoI = 2 * floor(round(60e6 / us) * tbs / 2) and a MIDI tick into
 * 24 * tbs * tick / division clocks (one clock = one song tick).
 *
 * Grid frame 0 is frame 3, the ROM's first song tick.
 */
#ifndef GBA_TEMPO_GRID_H
#define GBA_TEMPO_GRID_H

#include <stdint.h>
#include <stdbool.h>

/* Frame 3's SoundMain: 124773 + 2 x 280896 + 160 x 1232 + 197 cycles
 * (m4a_engine.c's FRAME1_START and SOUNDMAIN_FRAME_OFFSET). */
#define ROM_FIRST_TICK_CYCLE (124773 + 2 * GBA_CYCLES_PER_FRAME \
                              + 160 * GBA_CYCLES_PER_LINE + M4A_CYC_SOUNDMAIN_ENTRY)

typedef struct {
    uint32_t division;       /* SMF ticks per quarter note */
    uint32_t clocksPerBeat;  /* 24, or 48 with mid2agb -X */
    uint32_t tempoI;         /* the next frame's add: MPlayStart's 150 until a TEMPO */
    uint32_t tempoC;         /* what is left after the last placed tick */
    int64_t  frame;          /* the frame the walker is on (-1 before frame 0) */
    int64_t  lastFrame;      /* the frame of the last placed tick */
    uint64_t nextClock;      /* the next song tick to place */
    uint64_t frameFirstClock;/* the first tick of lastFrame */
    bool     frozen;         /* tempoI 0 met with tempoC < 150: no tick ever again */
} GbaTempoGrid;

/* The frame of a tick the ROM never plays (a TEMPO byte of 0 stops the
 * accumulator; ply_tempo can store it); its sample is past any render. */
#define GBA_TEMPO_GRID_NEVER INT64_MAX

void gba_tempo_grid_init(GbaTempoGrid *g, uint32_t division, uint32_t clocksPerBeat);

/* mid2agb's clock of an SMF tick: 24 * tbs * tick / division, truncated. */
uint64_t gba_tempo_grid_clock(const GbaTempoGrid *g, uint64_t midiTick);

/* The clock of SMF tick `tick` on loop pass `pass` (0 outside a loop): the
 * GOTO at the loop end's clock jumps back to the loop start's, so a pass
 * adds clock(loopEndTick) - clock(loopStartTick), whole clocks. */
uint64_t gba_tempo_grid_pass_clock(const GbaTempoGrid *g, uint64_t tick, uint64_t pass,
                                   uint64_t loopStartTick, uint64_t loopEndTick);

/* The grid frame of song tick `clock` (0 = frame 3), or GBA_TEMPO_GRID_NEVER.
 * Queries must not go backwards; a clock already placed returns the last
 * placed tick's frame. */
int64_t gba_tempo_grid_frame(GbaTempoGrid *g, uint64_t clock);

/* The index of song tick `clock` among its frame's ticks (0 for the first),
 * for a clock gba_tempo_grid_frame just placed or queried: the LFO steps
 * MPlayMain runs before that tick's commands in the same frame (slice 7). */
uint32_t gba_tempo_grid_tick_in_frame(const GbaTempoGrid *g, uint64_t clock);

/* A TEMPO (microseconds per beat) at the tick last placed: takes effect on
 * the next frame's add. */
void gba_tempo_grid_set_tempo(GbaTempoGrid *g, uint32_t usPerBeat);

/* The tempoI the ROM runs for a MIDI tempo (mid2agb's TEMPO byte doubled by
 * ply_tempo, tempoU 0x100). */
uint32_t gba_tempo_grid_tempo_i(uint32_t usPerBeat, uint32_t clocksPerBeat);

/* The output sample an event of grid frame `frame` is dispatched at: the
 * last sample a blip frame before that frame's SoundMain (frame 0 is the
 * --gba-start-delay origin, 2522 at 48 kHz). */
uint64_t gba_tempo_grid_sample(int64_t frame, double sampleRate);

/* poryaaaa_render's dispatch sample for one event, in event order: with
 * g == NULL (no --gba-tempo-grid) the exact-tempo sample unchanged; with a
 * grid, the sample of the grid frame of song tick `clock` relative to grid
 * frame 0's (so tick 0 stays at MIDI time 0), and a tempoUs != 0 (a TEMPO
 * mid2agb keeps) changes the add from the next frame on. */
uint64_t gba_tempo_grid_dispatch_sample(GbaTempoGrid *g, uint64_t exactSample,
                                        uint64_t clock, uint32_t tempoUs, double sampleRate);

#endif
