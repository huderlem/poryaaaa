/*
 * poryaaaa_render - Standalone M4A MIDI renderer
 *
 * Usage: poryaaaa_render <project_root> <voicegroup> --midi <file.mid> [options]
 *
 * Parses a Standard MIDI File (Type 0 or Type 1), renders it through the
 * M4A engine using a specified voicegroup, and writes a WAV file and/or
 * plays audio through the computer's speakers via miniaudio.
 *
 * Loop support: MIDI text events (Meta 0x01) or marker events (Meta 0x06)
 * containing exactly '[' mark the loop start, and ']' mark the loop end.
 * When both are found the song loops with a configurable count and fadeout.
 */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#ifdef __linux__
#include <dlfcn.h>
#endif
#include "m4a_engine.h"
#include "m4a_reverb.h"
#include "m4a_song.h"
#include "m4a_cry.h"
#include "m4a_sequencer.h"
#include "voicegroup_loader.h"
#include "gba_tempo_grid.h"

/* ========================================================================
 * WAV writing helpers (matching test_wav_export.c)
 * ======================================================================== */

static void write_u16_le(FILE *f, uint16_t val)
{
    uint8_t buf[2] = { val & 0xFF, (val >> 8) & 0xFF };
    fwrite(buf, 1, 2, f);
}

static void write_u32_le(FILE *f, uint32_t val)
{
    uint8_t buf[4] = { val & 0xFF, (val >> 8) & 0xFF,
                       (val >> 16) & 0xFF, (val >> 24) & 0xFF };
    fwrite(buf, 1, 4, f);
}

static int write_wav(const char *path, const float *left, const float *right,
                     uint64_t numSamples, int sampleRate)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open %s for writing\n", path);
        return -1;
    }

    uint16_t numChannels   = 2;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate   = (uint32_t)sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t dataSize   = (uint32_t)(numSamples * numChannels * bitsPerSample / 8);

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    write_u32_le(f, 36 + dataSize);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    write_u32_le(f, 16);
    write_u16_le(f, 1);              /* PCM */
    write_u16_le(f, numChannels);
    write_u32_le(f, (uint32_t)sampleRate);
    write_u32_le(f, byteRate);
    write_u16_le(f, blockAlign);
    write_u16_le(f, bitsPerSample);

    /* data chunk */
    fwrite("data", 1, 4, f);
    write_u32_le(f, dataSize);

    for (uint64_t i = 0; i < numSamples; i++) {
        /* The engine's output is int16 / 32768, so this is exact. */
        int32_t l = (int32_t)lrintf(left[i]  * 32768.0f);
        int32_t r = (int32_t)lrintf(right[i] * 32768.0f);
        if (l >  32767)  l =  32767;
        if (l < -32768)  l = -32768;
        if (r >  32767)  r =  32767;
        if (r < -32768)  r = -32768;
        write_u16_le(f, (uint16_t)(int16_t)l);
        write_u16_le(f, (uint16_t)(int16_t)r);
    }

    fclose(f);
    return 0;
}

/* ========================================================================
 * MIDI Parser (SMF Type 0 and Type 1)
 * ======================================================================== */

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
} MidiReader;

static int mr_read_byte(MidiReader *r, uint8_t *out)
{
    if (r->pos >= r->size) return -1;
    *out = r->data[r->pos++];
    return 0;
}

static int mr_read_u16_be(MidiReader *r, uint16_t *out)
{
    if (r->pos + 2 > r->size) return -1;
    *out = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    return 0;
}

static int mr_read_u32_be(MidiReader *r, uint32_t *out)
{
    if (r->pos + 4 > r->size) return -1;
    *out = ((uint32_t)r->data[r->pos]     << 24)
         | ((uint32_t)r->data[r->pos + 1] << 16)
         | ((uint32_t)r->data[r->pos + 2] <<  8)
         |  (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    return 0;
}

static int mr_skip(MidiReader *r, uint32_t n)
{
    if (r->pos + n > r->size) return -1;
    r->pos += n;
    return 0;
}

/* Read a variable-length quantity (up to 4 bytes) */
static int mr_read_vlq(MidiReader *r, uint32_t *out)
{
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (mr_read_byte(r, &b) < 0) return -1;
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) { *out = val; return 0; }
    }
    return -1; /* VLQ too long */
}

/* A tempo change in the MIDI file */
typedef struct {
    uint64_t tick;
    uint32_t tempo; /* microseconds per quarter note */
    uint8_t  track; /* SMF track index (mid2agb reads tempo from the first only) */
} TempoEvent;

/* A raw MIDI channel event collected during parsing */
typedef struct {
    uint64_t tick;
    uint8_t  channel;
    uint8_t  track;    /* SMF track index (MTrk chunk number) */
    uint8_t  type;   /* nibble: 0x8=off, 0x9=on, 0xB=cc, 0xC=pc, 0xE=pb */
    uint8_t  data0;
    uint8_t  data1;
    int      origIndex; /* insertion order — used to make sort stable */
} RawMidiEvent;

/*
 * Synthetic RenderEvent type for a tempo change.  Real MIDI channel-voice
 * events use status nibbles 0x8..0xE, so 0x1 is free.  The target BPM is
 * carried as a 14-bit value across data0 (low 7 bits) and data1 (high 7).
 */
#define RENDER_EVT_TEMPO 0x1

/* A rendered event with its absolute sample position */
typedef struct {
    uint64_t samplePos;
    uint64_t tick;     /* absolute SMF tick (0 for synthetic events) */
    uint8_t  channel;
    uint8_t  track;    /* SMF track index */
    uint8_t  type;
    uint8_t  data0;
    uint8_t  data1;
    /* --gba-tempo-grid: the event's song tick (SMF ticks within its loop
     * pass), the pass (0 outside the loop body) and, for a tempo change
     * mid2agb keeps (first MTrk), its microseconds per beat (0: not a TEMPO
     * on the ROM's grid). */
    uint64_t songTick;
    uint32_t loopPass;
    uint32_t gridTempoUs;
} RenderEvent;

/* Dynamic array helpers */
typedef struct { RawMidiEvent *events; int count, capacity; } RawEventArray;
typedef struct { TempoEvent   *events; int count, capacity; } TempoArray;

static int raw_push(RawEventArray *a, RawMidiEvent ev)
{
    if (a->count >= a->capacity) {
        int nc = a->capacity ? a->capacity * 2 : 512;
        RawMidiEvent *p = realloc(a->events, (size_t)nc * sizeof(*p));
        if (!p) return -1;
        a->events = p;  a->capacity = nc;
    }
    ev.origIndex = a->count;
    a->events[a->count++] = ev;
    return 0;
}

static int tempo_push(TempoArray *a, TempoEvent ev)
{
    if (a->count >= a->capacity) {
        int nc = a->capacity ? a->capacity * 2 : 16;
        TempoEvent *p = realloc(a->events, (size_t)nc * sizeof(*p));
        if (!p) return -1;
        a->events = p;  a->capacity = nc;
    }
    a->events[a->count++] = ev;
    return 0;
}

/*
 * Check whether the (possibly whitespace-padded) text in buf[0..len) is
 * exactly the single character `marker` after stripping leading and trailing
 * ASCII whitespace.
 */
static bool text_is_loop_marker(const uint8_t *buf, uint32_t len, char marker)
{
    uint32_t s = 0, e = len;
    while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\r' || buf[s] == '\n'))
        s++;
    while (e > s && (buf[e-1] == ' ' || buf[e-1] == '\t' || buf[e-1] == '\r' || buf[e-1] == '\n'))
        e--;
    return (e - s == 1) && ((char)buf[s] == marker);
}

/*
 * Parse one MTrk chunk.  trackLen is the number of bytes in the chunk body.
 * Collected channel events are appended to rawEvents and tempo changes to tempos.
 * Loop markers ('[' and ']') found in text/marker meta events are written to
 * *loopStartTick / *loopEndTick (first occurrence wins; UINT64_MAX = not found).
 */
static void parse_track(MidiReader *r, uint32_t trackLen, uint8_t trackIndex,
                         RawEventArray *rawEvents, TempoArray *tempos,
                         uint64_t *loopStartTick, uint64_t *loopEndTick)
{
    size_t   end           = r->pos + trackLen;
    uint64_t tick          = 0;
    uint8_t  runningStatus = 0;

    while (r->pos < end) {
        uint32_t delta;
        if (mr_read_vlq(r, &delta) < 0) break;
        tick += delta;

        uint8_t b;
        if (mr_read_byte(r, &b) < 0) break;

        if (b == 0xFF) {
            /* Meta event — clear running status */
            runningStatus = 0;
            uint8_t  metaType;
            uint32_t metaLen;
            if (mr_read_byte(r, &metaType) < 0) break;
            if (mr_read_vlq(r, &metaLen)   < 0) break;

            if (metaType == 0x51 && metaLen == 3) {
                /* Tempo change */
                uint8_t t0, t1, t2;
                if (mr_read_byte(r, &t0) < 0 ||
                    mr_read_byte(r, &t1) < 0 ||
                    mr_read_byte(r, &t2) < 0) break;
                uint32_t tempo = ((uint32_t)t0 << 16) |
                                 ((uint32_t)t1 <<  8) | t2;
                TempoEvent te = { tick, tempo, trackIndex };
                tempo_push(tempos, te);
            } else if (metaType >= 0x01 && metaType <= 0x07) {
                /* Text-type meta event: check for loop markers.
                 * Meta types 0x01 (Text) and 0x06 (Marker) are most common. */
                uint32_t readLen = metaLen < 32 ? metaLen : 32;
                uint8_t  textBuf[32];
                for (uint32_t j = 0; j < readLen; j++) {
                    if (mr_read_byte(r, &textBuf[j]) < 0) goto track_done;
                }
                if (metaLen > readLen) {
                    if (mr_skip(r, metaLen - readLen) < 0) goto track_done;
                }
                if (text_is_loop_marker(textBuf, readLen, '[') &&
                        *loopStartTick == UINT64_MAX)
                    *loopStartTick = tick;
                else if (text_is_loop_marker(textBuf, readLen, ']') &&
                        *loopEndTick == UINT64_MAX)
                    *loopEndTick = tick;
            } else {
                if (mr_skip(r, metaLen) < 0) break;
            }
        } else if (b == 0xF0 || b == 0xF7) {
            /* SysEx — clear running status, skip body */
            runningStatus = 0;
            uint32_t sysexLen;
            if (mr_read_vlq(r, &sysexLen) < 0) break;
            if (mr_skip(r, sysexLen)       < 0) break;
        } else {
            /* Channel message */
            uint8_t status, data0;
            if (b & 0x80) {
                status = b;
                runningStatus = b;
                if (mr_read_byte(r, &data0) < 0) break;
            } else {
                /* Running status — b is first data byte */
                if (!runningStatus) break;
                status = runningStatus;
                data0  = b;
            }

            uint8_t type = (status >> 4) & 0x0F;
            uint8_t chan =  status       & 0x0F;

            switch (type) {
            case 0x8: { /* Note Off */
                uint8_t vel;
                if (mr_read_byte(r, &vel) < 0) goto track_done;
                RawMidiEvent ev = { tick, chan, trackIndex, 0x8, data0, vel, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            case 0x9: { /* Note On (vel=0 → note off) */
                uint8_t vel;
                if (mr_read_byte(r, &vel) < 0) goto track_done;
                RawMidiEvent ev = { tick, chan, trackIndex, vel ? (uint8_t)0x9 : (uint8_t)0x8, data0, vel, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            case 0xA: { /* Polyphonic aftertouch — skip */
                uint8_t dummy;
                if (mr_read_byte(r, &dummy) < 0) goto track_done;
                break;
            }
            case 0xB: { /* Control Change */
                uint8_t val;
                if (mr_read_byte(r, &val) < 0) goto track_done;
                RawMidiEvent ev = { tick, chan, trackIndex, 0xB, data0, val, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            case 0xC: { /* Program Change (1 data byte) */
                RawMidiEvent ev = { tick, chan, trackIndex, 0xC, data0, 0, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            case 0xD: { /* Channel Pressure — skip (already read data0) */
                break;
            }
            case 0xE: { /* Pitch Bend (2 data bytes: LSB already in data0) */
                uint8_t msb;
                if (mr_read_byte(r, &msb) < 0) goto track_done;
                RawMidiEvent ev = { tick, chan, trackIndex, 0xE, data0, msb, 0 };
                raw_push(rawEvents, ev);
                break;
            }
            default:
                goto track_done; /* unknown status byte, bail */
            }
        }
    }
track_done:
    r->pos = end;
}

static int cmp_raw_events(const void *a, const void *b)
{
    const RawMidiEvent *ea = (const RawMidiEvent *)a;
    const RawMidiEvent *eb = (const RawMidiEvent *)b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return  1;
    /* Stable tiebreaker: preserve original file order so that setup events
     * (Program Change, CC) that precede Note Ons at the same tick are always
     * dispatched first, regardless of whether the platform's qsort is stable. */
    if (ea->origIndex < eb->origIndex) return -1;
    if (ea->origIndex > eb->origIndex) return  1;
    return 0;
}

static int cmp_tempo_events(const void *a, const void *b)
{
    uint64_t ta = ((const TempoEvent *)a)->tick;
    uint64_t tb = ((const TempoEvent *)b)->tick;
    if (ta < tb) return -1;
    if (ta > tb) return  1;
    return 0;
}

/*
 * Convert an absolute tick position to an absolute sample index using the
 * tempo map.  Default tempo: 500000 µs/beat (= 120 BPM).
 */
static uint64_t tick_to_sample(uint64_t tick,
                                const TempoEvent *tempos, int tempoCount,
                                uint32_t tpqn, double sampleRate)
{
    double   samples   = 0.0;
    uint64_t prevTick  = 0;
    double   prevTempo = 500000.0; /* default 120 BPM */

    for (int i = 0; i < tempoCount; i++) {
        if (tempos[i].tick >= tick) break;
        uint64_t segTicks = tempos[i].tick - prevTick;
        samples   += (double)segTicks * prevTempo / (double)tpqn / 1000000.0 * sampleRate;
        prevTick   = tempos[i].tick;
        prevTempo  = (double)tempos[i].tempo;
    }
    uint64_t remTicks = tick - prevTick;
    samples += (double)remTicks * prevTempo / (double)tpqn / 1000000.0 * sampleRate;

    return (uint64_t)(samples + 0.5);
}

/* Build a synthetic tempo-change RenderEvent (BPM encoded as 14-bit). */
static RenderEvent make_tempo_event(uint64_t samplePos, double bpm)
{
    int b = (int)(bpm + 0.5);
    if (b < 1)      b = 1;
    if (b > 0x3FFF) b = 0x3FFF;
    RenderEvent ev;
    ev.samplePos = samplePos;
    ev.tick      = 0;
    ev.channel   = 0;
    ev.track     = 0;
    ev.type      = RENDER_EVT_TEMPO;
    ev.data0     = (uint8_t)(b & 0x7F);
    ev.data1     = (uint8_t)((b >> 7) & 0x7F);
    ev.songTick  = 0;
    ev.loopPass  = 0;
    ev.gridTempoUs = 0;
    return ev;
}

typedef struct {
    RenderEvent *events;
    int          count;
} RenderEventArray;

/*
 * Load and parse a Standard MIDI File.
 *
 * On success returns a heap-allocated RenderEventArray (caller must free
 * both ->events and the struct itself).  Writes the sample index of the last
 * MIDI event into *totalMidiSamples.  If '[' / ']' loop-marker text events
 * are found in any track, their sample and tick positions are written to
 * *loopStartSampleOut / *loopEndSampleOut / *loopStartTickOut /
 * *loopEndTickOut; otherwise UINT64_MAX is written.  The file's ticks per
 * quarter note go to *tpqnOut, and the tempo map to *temposOut (ownership
 * passes to the caller, who must free ->events).
 *
 * Returns NULL on error.
 */
static RenderEventArray *parse_midi(const char *path, double sampleRate,
                                     uint64_t *totalMidiSamples,
                                     uint64_t *loopStartSampleOut,
                                     uint64_t *loopEndSampleOut,
                                     uint64_t *loopStartTickOut,
                                     uint64_t *loopEndTickOut,
                                     uint32_t *tpqnOut,
                                     TempoArray *temposOut,
                                     uint16_t *midiFormatOut)
{
    *loopStartSampleOut = UINT64_MAX;
    *loopEndSampleOut   = UINT64_MAX;
    *loopStartTickOut   = UINT64_MAX;
    *loopEndTickOut     = UINT64_MAX;
    temposOut->events   = NULL;
    temposOut->count    = 0;
    temposOut->capacity = 0;

    /* ---- Load file into memory ---- */
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open MIDI file: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize <= 0) { fprintf(stderr, "Empty MIDI file\n"); fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)fsize);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "Failed to read MIDI file: %s\n", path);
        free(buf); fclose(f); return NULL;
    }
    fclose(f);

    MidiReader r = { buf, (size_t)fsize, 0 };

    /* ---- MThd header ---- */
    if (r.size < 14 || memcmp(r.data, "MThd", 4) != 0) {
        fprintf(stderr, "Not a Standard MIDI File: %s\n", path);
        free(buf); return NULL;
    }
    r.pos = 4;

    uint32_t hdrLen;
    uint16_t format, numTracks, division;
    if (mr_read_u32_be(&r, &hdrLen)    < 0 ||
        mr_read_u16_be(&r, &format)    < 0 ||
        mr_read_u16_be(&r, &numTracks) < 0 ||
        mr_read_u16_be(&r, &division)  < 0) {
        fprintf(stderr, "Invalid MIDI header\n");
        free(buf); return NULL;
    }
    /* Skip any extra header bytes */
    if (hdrLen > 6) r.pos += hdrLen - 6;

    if (format > 1) {
        fprintf(stderr, "Unsupported MIDI format %u (only 0 and 1 supported)\n", format);
        free(buf); return NULL;
    }
    if (division & 0x8000) {
        fprintf(stderr, "SMPTE time codes not supported\n");
        free(buf); return NULL;
    }

    *midiFormatOut = format;
    uint32_t tpqn = division; /* ticks per quarter note */

    RawEventArray rawEvents    = { NULL, 0, 0 };
    TempoArray    tempos       = { NULL, 0, 0 };
    uint64_t      loopStartTick = UINT64_MAX;
    uint64_t      loopEndTick   = UINT64_MAX;

    /* ---- Parse all tracks ---- */
    for (int t = 0; t < (int)numTracks; t++) {
        if (r.pos + 8 > r.size) break;
        if (memcmp(r.data + r.pos, "MTrk", 4) != 0) {
            fprintf(stderr, "Expected MTrk chunk (track %d)\n", t);
            break;
        }
        r.pos += 4;
        uint32_t trackLen;
        if (mr_read_u32_be(&r, &trackLen) < 0) break;
        size_t trackEnd = r.pos + trackLen;
        parse_track(&r, trackLen, (uint8_t)t, &rawEvents, &tempos, &loopStartTick, &loopEndTick);
        r.pos = trackEnd;
    }

    free(buf);

    /* ---- Sort ---- */
    if (rawEvents.count > 0)
        qsort(rawEvents.events, (size_t)rawEvents.count,
              sizeof(RawMidiEvent), cmp_raw_events);
    if (tempos.count > 0)
        qsort(tempos.events, (size_t)tempos.count,
              sizeof(TempoEvent), cmp_tempo_events);

    /* ---- Compute last tick's sample position ---- */
    uint64_t lastTick = 0;
    for (int i = 0; i < rawEvents.count; i++)
        if (rawEvents.events[i].tick > lastTick)
            lastTick = rawEvents.events[i].tick;
    *totalMidiSamples = tick_to_sample(lastTick,
                                        tempos.events, tempos.count,
                                        tpqn, sampleRate);

    /* ---- Convert loop marker ticks to sample positions ---- */
    if (loopStartTick != UINT64_MAX)
        *loopStartSampleOut = tick_to_sample(loopStartTick,
                                              tempos.events, tempos.count,
                                              tpqn, sampleRate);
    if (loopEndTick != UINT64_MAX)
        *loopEndSampleOut = tick_to_sample(loopEndTick,
                                            tempos.events, tempos.count,
                                            tpqn, sampleRate);

    /* ---- Build RenderEventArray ----
     *
     * Note/CC/PC/PB events and tempo changes are merged into one
     * sample-ordered stream.  Tempo changes become synthetic
     * RENDER_EVT_TEMPO events so the render loop updates the engine's tempo
     * at the right moment.  The engine's tempo drives LFO/vibrato (and
     * portamento) speed; without this the engine would keep its init-default
     * tempo and the LFO would run at the wrong rate (e.g. ~2x too fast for a
     * 76 BPM song), diverging from the CLAP plugin which tracks host tempo. */

    /* Note/CC/PC/PB events, already in tick (hence sample) order. */
    RenderEvent *noteEvts = malloc((size_t)rawEvents.count * sizeof(RenderEvent));
    if (rawEvents.count > 0 && !noteEvts) {
        free(rawEvents.events); free(tempos.events);
        return NULL;
    }
    for (int i = 0; i < rawEvents.count; i++) {
        const RawMidiEvent *re = &rawEvents.events[i];
        noteEvts[i].samplePos = tick_to_sample(re->tick,
                                               tempos.events, tempos.count,
                                               tpqn, sampleRate);
        noteEvts[i].tick      = re->tick;
        noteEvts[i].channel   = re->channel;
        noteEvts[i].track     = re->track;
        noteEvts[i].type      = re->type;
        noteEvts[i].data0     = re->data0;
        noteEvts[i].data1     = re->data1;
        noteEvts[i].songTick  = re->tick;
        noteEvts[i].loopPass  = 0;
        noteEvts[i].gridTempoUs = 0;
    }

    /* Tempo-change events, also in tick order.  Prepend the SMF default of
     * 120 BPM at sample 0 when the song does not set a tempo at its very
     * start, so the engine never runs on its (unrelated) init default. */
    bool needDefaultTempo = (tempos.count == 0 || tempos.events[0].tick != 0);
    int  tempoEvtCount    = tempos.count + (needDefaultTempo ? 1 : 0);
    RenderEvent *tempoEvts = malloc((size_t)(tempoEvtCount > 0 ? tempoEvtCount : 1)
                                    * sizeof(RenderEvent));
    if (!tempoEvts) {
        free(noteEvts); free(rawEvents.events); free(tempos.events);
        return NULL;
    }
    int te = 0;
    if (needDefaultTempo)
        tempoEvts[te++] = make_tempo_event(0, 120.0);
    for (int i = 0; i < tempos.count; i++) {
        uint64_t sp = tick_to_sample(tempos.events[i].tick,
                                     tempos.events, tempos.count,
                                     tpqn, sampleRate);
        tempoEvts[te] = make_tempo_event(sp, 60000000.0 / (double)tempos.events[i].tempo);
        tempoEvts[te].songTick = tempos.events[i].tick;
        if (tempos.events[i].track == 0)
            tempoEvts[te].gridTempoUs = tempos.events[i].tempo;
        te++;
    }

    /* Merge the two sample-ordered lists.  On equal sample positions the
     * tempo change is emitted first so it takes effect before the notes at
     * that instant. */
    RenderEventArray *result = malloc(sizeof(*result));
    if (!result) {
        free(tempoEvts); free(noteEvts); free(rawEvents.events); free(tempos.events);
        return NULL;
    }
    int total = rawEvents.count + tempoEvtCount;
    result->count  = total;
    result->events = malloc((size_t)(total > 0 ? total : 1) * sizeof(RenderEvent));
    if (!result->events) {
        free(result); free(tempoEvts); free(noteEvts);
        free(rawEvents.events); free(tempos.events);
        return NULL;
    }
    {
        int ni = 0, ti = 0, oi = 0;
        while (ni < rawEvents.count || ti < tempoEvtCount) {
            bool takeTempo;
            if (ni >= rawEvents.count)        takeTempo = true;
            else if (ti >= tempoEvtCount)     takeTempo = false;
            else takeTempo = (tempoEvts[ti].samplePos <= noteEvts[ni].samplePos);
            result->events[oi++] = takeTempo ? tempoEvts[ti++] : noteEvts[ni++];
        }
    }

    free(tempoEvts);
    free(noteEvts);
    free(rawEvents.events);
    *loopStartTickOut = loopStartTick;
    *loopEndTickOut   = loopEndTick;
    *tpqnOut          = tpqn;
    *temposOut        = tempos; /* caller owns the tempo map now */
    return result;
}

/* ========================================================================
 * Miniaudio playback
 * ======================================================================== */

#ifdef __linux__
/*
 * Install a no-op error handler into libasound to suppress the wall of
 * "cannot find card '0'" messages that ALSA prints on WSL and other
 * systems without hardware audio.  Uses dlopen so we don't need to link
 * against libasound explicitly.
 */
static void alsa_error_noop(const char *file, int line, const char *func,
                              int err, const char *fmt, ...)
{
    (void)file; (void)line; (void)func; (void)err; (void)fmt;
}

static void suppress_alsa_errors(void)
{
    /* If libasound is not present this is a no-op */
    void *lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib) return;

    /* void snd_lib_error_set_handler(snd_lib_error_handler_t handler) */
    typedef void (*ErrFn)(const char*, int, const char*, int, const char*, ...);
    typedef void (*SetFn)(ErrFn);
    SetFn setfn;
    *(void **)(&setfn) = dlsym(lib, "snd_lib_error_set_handler");
    if (setfn)
        setfn(alsa_error_noop);
    /* Leave the handle open so the handler stays installed when miniaudio
     * later opens libasound itself (same shared library instance). */
}
#endif /* __linux__ */

typedef struct {
    const float *bufL;
    const float *bufR;
    uint64_t     total;
    uint64_t     pos;
} PlaybackCtx;

static void playback_callback(ma_device *dev, void *out,
                               const void *in, ma_uint32 frameCount)
{
    (void)in;
    PlaybackCtx *ctx = (PlaybackCtx *)dev->pUserData;
    float *dst = (float *)out;
    for (ma_uint32 i = 0; i < frameCount; i++) {
        if (ctx->pos >= ctx->total) {
            dst[i * 2]     = 0.0f;
            dst[i * 2 + 1] = 0.0f;
        } else {
            dst[i * 2]     = ctx->bufL[ctx->pos];
            dst[i * 2 + 1] = ctx->bufR[ctx->pos];
            ctx->pos++;
        }
    }
}

/* ========================================================================
 * CLI
 * ======================================================================== */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <project_root> <voicegroup> --midi <file.mid> [options]\n"
        "       %s <project_root> [<voicegroup>] --song <file.s> [options]\n"
        "       %s <project_root> --cry <id> [options]\n"
        "\n"
        "Required:\n"
        "  <project_root>              Path to pokeemerald/pokefirered project root\n"
        "  <voicegroup>                Voicegroup name (e.g. petalburg); with --song it\n"
        "                              is optional and overrides the song's _grp symbol\n"
        "  --midi <file.mid>           MIDI input file, or\n"
        "  --song <file.s>             compiled song (mid2agb output) played through the\n"
        "                              MPlayMain sequencer port.  Master volume, reverb\n"
        "                              and the voicegroup are baked into the .s file, so\n"
        "                              --song-volume / --reverb are ignored on this path.\n"
        "  --cry <id>                  a Pokemon cry: gCryTable[id] (sound/cry_tables.inc; the cry_reverse\n"
        "                              entries continue the count after the cry lines: 388 + the forward index)\n"
        "                              played as the game does (PlayCryInternal's\n"
        "                              CRY_MODE_NORMAL defaults: volume 120, pan 0, pitch\n"
        "                              15360, length 140, priority 10) on cry player 0\n"
        "  --cry-pitch <-32768..32767> SetPokemonCryPitch's value (15360 = key 60; the game's\n"
        "                              modes use 14440..15800)\n"
        "  --cry-pan <-128..127>       SetPokemonCryPanpot's value (0 = centre; -64 left, 63 right)\n"
        "  --cry-volume <0..255>       SetPokemonCryVolume's value (masked to 0..127; the game\n"
        "                              uses 120, 90, 70)\n"
        "  --cry-length <0..65535>     SetPokemonCryLength's value: the ticks the TIE holds before\n"
        "                              the EOT (XCMD xWAIT; the game's modes use 10..140)\n"
        "  --cry-release <0..255>      SetPokemonCryRelease's value: the track tone's release at\n"
        "                              the EOT (XCMD xRELE; 0 cuts, the game's modes use 100..225)\n"
        "  --cry-progress <0..2^32-1>  SetPokemonCryProgress: the sample offset the cry starts\n"
        "                              at (XCMD 0x0D; every game mode passes 0)\n"
        "  --cry-chorus <-128..255>    SetPokemonCryChorus's s8: 0 = one track; else the cry song\n"
        "                              is two tracks, the second's TUNE = (chorus + TUNE) & 0x7F\n"
        "                              (the game's modes pass 20 and 192; 128..255 wrap to the s8\n"
        "                              the game's u32 becomes, 192 = -64)\n"
        "  --cry2 <id> --cry2-start-frame <1..255>\n"
        "                              a second cry while the first plays: gCryTable[id] at\n"
        "                              CRY_MODE_NORMAL's defaults (volume 120, pan 0, pitch 15360,\n"
        "                              length 140, release 0, chorus 0), started on VBlank F as the\n"
        "                              accuracy ROM's StartCry does (mgba_render --id2 N --start-frame2 F;\n"
        "                              its first tick is on engine frame F + 2).  It takes the first\n"
        "                              free cry player (SetPokemonCryTone): cry player 1 while the\n"
        "                              first cry plays, whose MPlayMain body runs after cry 0's on\n"
        "                              the same frame; both busy is refused.  Needs --cry on cry0,\n"
        "                              or --song: a cry over a sound effect or a song (the song on\n"
        "                              bgm, se1, se2 or se3), cry player 0 from silence, its body\n"
        "                              after the song's on the same frame (mgba_render --type song\n"
        "                              --id N --id2 M --start-frame2 F)\n"
        "\n"
        "Output (at least one required):\n"
        "  --output <file.wav>         Write rendered audio to WAV file\n"
        "  --play                      Play audio through computer speakers\n"
        "\n"
        "Audio options:\n"
        "  --song-volume <0-127>       Song master volume (default: 127)\n"
        "  --reverb <0-127>            Reverb amount (default: 0)\n"
        "  --respect-base-midi-key     Opt-in: treat a PCM voice's key as the sample's base MIDI note (default: off)\n"
        "  --portamento                Opt-in: enable the portamento glide effect, CC 5 (default: off)\n"
        "  --pwm                       Opt-in: enable pulse-width modulation on CGB square channels, CC 0x17/0x19 (default: off)\n"
        "  --quantize                  --midi only: latch every MIDI event to the next VBlank tick\n"
        "                              (note on / off, CC, program, bend applied where MPlayMain\n"
        "                              runs a track's commands, in arrival order; a note shorter\n"
        "                              than a frame plays one tick).  Default: events apply at\n"
        "                              their sample position.  (--song / --cry already run on the tick.)\n"
        "  --gba-start-delay           --midi only: start the MIDI where the game would, after the\n"
        "                              engine's start-up frames (frame 3's SoundMain, about 52.5 ms\n"
        "                              of leading silence), as --song renders already do.  For\n"
        "                              comparing with an emulator recording; default: the first\n"
        "                              note plays at the start of the output\n"
        "  --gba-tempo-grid            --midi only: play each MIDI tick on the frame the game\n"
        "                              would (the song tempo as mid2agb writes it, a tick each\n"
        "                              time the tempo accumulator reaches 150, MPlayStart's 150\n"
        "                              before the first TEMPO) instead of at the MIDI's exact\n"
        "                              tempo (at an even BPM about 0.46 %% faster; mid2agb drops\n"
        "                              an odd BPM by one), the LFO stepping on the same ticks\n"
        "                              (MPlayStart's tempo at the song start).  With\n"
        "                              --gba-start-delay each tick and LFO step lands on the\n"
        "                              ROM's frame (without it the ticks of the first frames\n"
        "                              collapse onto frame 3 and the LFO runs 3 frames behind).\n"
        "                              Default: the exact tempo map\n"
        "  --polyphony <1-15>          Max simultaneous PCM channels (default: 5)\n"
        "  --sample-rate <hz>          Sample rate in Hz (default: 44100)\n"
        "  --pcm-mix-rate <hz>         DirectSound (PCM) mix rate: one of the m4a sound-mode rates\n"
        "                              5734 7884 10512 13379 15768 18157 21024 26758 31536 36314\n"
        "                              40137 42048 (nearest is used; default: 13379)\n"
        "  --tail <seconds>            Silence after last event, no loop markers (default: 3.0)\n"
        "\n"
        "Compiled-song options (--song):\n"
        "  --player <bgm|se1|se2|se3|cry0|cry1>\n"
        "                              the music player the song plays on, as the game's\n"
        "                              song table names it (default: bgm; a sound\n"
        "                              effect's player has 3 / 9 / 1 tracks and its own\n"
        "                              position in SoundMain's MPlayMain chain; --cry\n"
        "                              plays on cry0 unless told otherwise)\n"
        "  --psg-log <file>            log every PSG register write with its CPU cycle\n"
        "  --seq-log <file>            log each frame's modeled CgbSound entry cycle\n"
        "\n"
        "Loop options (when MIDI contains '[' / ']' text events):\n"
        "  --loop-count <n>            Number of loop body repetitions (default: 2)\n"
        "  --extended-clocks           Song is built with mid2agb -X (48 clocks/beat),\n"
        "                              which raises the length at which a note crossing\n"
        "                              the loop end is tied (held forever, as on hardware)\n"
        "  --fadeout <seconds>         Fadeout duration after final loop (default: 5.0)\n"
        "  --total-duration-seconds <s>  Override loop-count; set exact total duration\n"
        "                                (fadeout occupies the final --fadeout seconds)\n"
        "  (with --song, loop-count counts passes through the song's GOTO; the song's\n"
        "   FINE ends the render after --tail seconds)\n",
        prog, prog, prog);
}

/* ========================================================================
 * Compiled-song path: play a mid2agb .s file through the sequencer port.
 * Produces the rendered buffers; the caller handles output.
 * ======================================================================== */

typedef struct {
    const char *projectRoot;
    const char *voicegroup;      /* NULL: use the song header's */
    const char *songPath;        /* --song, or NULL with --cry */
    int cry;                     /* --cry: gCryTable index, or -1 */
    M4ACryParams cryParams;      /* --cry-pitch / --cry-pan / --cry-volume / --cry-length / --cry-release / --cry-progress / --cry-chorus over PlayCryInternal's defaults */
    int cry2;                    /* --cry2: a second cry's gCryTable index, or -1 */
    int cry2StartFrame;          /* --cry2-start-frame: the VBlank it starts on (its tick on engine frame + 2) */
    bool respectBaseMidiKey, portamento, pwm;
    int maxChannels;
    double sampleRate;
    int pcmFreqMode;
    double tailSeconds, fadeoutSeconds, totalDurSeconds;
    int loopCount;
    const char *psgLogPath;      /* --psg-log: every PSG register write with its CPU cycle */
    const char *seqLogPath;      /* --seq-log: the sequencer's per-frame CgbSound entry cycle */
    int player;                  /* --player: M4A_PLAYER_* the song plays on */
} SongRenderOpts;

/* --player bgm|se1|se2|se3|cry0|cry1 -> M4A_PLAYER_*, or -1 */
static const char *const kPlayerNames[M4A_PLAYER_COUNT] = { "bgm", "se1", "se2", "se3", "cry0", "cry1" };
static int parse_player(const char *name)
{
    const char *const *names = kPlayerNames;
    for (int i = 0; i < M4A_PLAYER_COUNT; i++)
        if (strcmp(name, names[i]) == 0)
            return i;
    return -1;
}

/* --psg-log: one line per PSG register write, "cycle reg value", on the
 * same cycle clock as test/accuracy/ref/mgba_probe.c logs mgba's writes. */
static struct { FILE *f; const M4AEngine *engine; } sPsgLog;
static void psg_log_cb(void *user, uint32_t reg, uint32_t value)
{
    (void)user;
    if (sPsgLog.f)
        fprintf(sPsgLog.f, "%lld %02X %02X\n", (long long)sPsgLog.engine->gba.now, reg, value);
}

static int grow_buffers(float **l, float **r, uint64_t *cap, uint64_t need, uint64_t limit)
{
    if (need <= *cap)
        return 0;
    uint64_t cap2 = *cap ? *cap : (1u << 20);
    while (cap2 < need)
        cap2 *= 2;
    if (cap2 > limit)
        cap2 = limit; /* never allocate past the render limit */
    float *nl = realloc(*l, cap2 * sizeof(float));
    float *nr = realloc(*r, cap2 * sizeof(float));
    if (!nl || !nr) {
        free(nl ? nl : *l);
        free(nr ? nr : *r);
        *l = *r = NULL;
        return -1;
    }
    *l = nl; *r = nr; *cap = cap2;
    return 0;
}

/* --cry2: the second cry's StartCry, issued between two SoundMains as the
 * ROM's StartTest issues it on VBlank startFrame2 -- at the top of the
 * frame hook of engine frame startFrame2 + 2, before either player's
 * MPlayMain body (a hook's entry is engine frame frameCount + 2: boot runs
 * frame 2's).  SetPokemonCryTone's rule picks the player then: the first
 * free cry player, cry 1 while the first cry plays; neither free is
 * refused (the game's maxClock replacement, out of scope). */
static struct {
    M4AFrameHook hook;          /* the sequencer chain's hook, run after the check (with the engine's own user) */
    M4ASequencer *players[2];   /* cry player 0, cry player 1 */
    M4ASong *song;              /* pending; NULL once started or refused */
    const ToneData *voices;
    uint32_t startFrame;
    int startedOn;              /* the player it took, -1 before */
    bool refused;
} sCry2;
static void cry2_hook(M4AEngine *engine, void *user)
{
    if (sCry2.song && sCry2.players[0]->frameCount == sCry2.startFrame) {
        int p = m4a_cry_choose_player(sCry2.players);
        if (p < 0) {
            sCry2.refused = true;
        } else {
            m4a_sequencer_set_voicegroup(sCry2.players[p], sCry2.voices);
            m4a_sequencer_start(sCry2.players[p], sCry2.song);
            sCry2.startedOn = p;
        }
        sCry2.song = NULL;
    }
    sCry2.hook(engine, user);
}

/* Play a loaded song through the sequencer on the chosen player.  Takes
 * ownership of `song` and `vg`, and of `song2` / `vg2` (a second cry at
 * CRY_MODE_NORMAL's defaults started on VBlank startFrame2, or NULL). */
static int render_sequenced(const SongRenderOpts *o, M4ASong *song, LoadedVoiceGroup *vg,
                            M4ASong *song2, LoadedVoiceGroup *vg2, int startFrame2,
                            float **outL, float **outR, uint64_t *outTotal, uint64_t *outFadeStart)
{
    M4AEngine engine;
    m4a_engine_init(&engine, (float)o->sampleRate);
    m4a_engine_set_voicegroup(&engine, vg->voices);
    m4a_engine_set_song_volume(&engine, MAX_SONG_VOLUME); /* baked into the song's VOL bytes */
    engine.maxPcmChannels = (uint8_t)o->maxChannels;
    engine.respectBaseMidiKey = o->respectBaseMidiKey;
    m4a_engine_set_portamento_enabled(&engine, o->portamento);
    m4a_engine_set_pwm_enabled(&engine, o->pwm);
    m4a_engine_set_pcm_freq_mode(&engine, o->pcmFreqMode);

    M4ASequencer seq, seq2;
    m4a_sequencer_init(&seq, &engine);
    if (m4a_sequencer_set_player(&seq, o->player) != 0) {
        fprintf(stderr, "Error: no music player %d\n", o->player);
        m4a_engine_destroy(&engine);
        voicegroup_free(vg);
        m4a_song_free(song);
        if (song2) m4a_song_free(song2);
        if (vg2) voicegroup_free(vg2);
        return -1;
    }
    memset(&sCry2, 0, sizeof(sCry2));
    sCry2.startedOn = -1;
    if (song2 && o->cry >= 0) {
        /* The two cry players, in SoundMain's chain: cry 1's body after cry
         * 0's on the same frame, on engine tracks 2..3 above cry 0's 0..1. */
        if (o->player != M4A_PLAYER_CRY0
            || m4a_sequencer_init_chained(&seq2, &seq, seq.trackBase + seq.maxTracks) != 0
            || m4a_sequencer_set_player(&seq2, M4A_PLAYER_CRY1) != 0) {
            fprintf(stderr, "Error: --cry2 needs the first cry on cry player 0 (--player cry0, the default)\n");
            m4a_sequencer_detach(&seq);
            m4a_engine_destroy(&engine);
            voicegroup_free(vg);
            m4a_song_free(song);
            m4a_song_free(song2);
            voicegroup_free(vg2);
            return -1;
        }
        sCry2.players[0] = &seq;
        sCry2.players[1] = &seq2;
        printf("  Second cry: StartCry on VBlank %d (its first tick on engine frame %d), cry player 1 if the first still plays\n",
               startFrame2, startFrame2 + 2);
    } else if (song2) {
        /* A cry over a sound effect or a song (plan, step 12): cry player 0
         * chained after the song's player -- BGM (10 tracks) -> cry 0 on
         * engine tracks 10..11, SE1 (3) -> cry 0 on 3..4 -- its body after
         * the song's on the same frame, the paused players between them
         * charged by the chain.  SetPokemonCryTone from silence takes cry 0. */
        if (o->player > M4A_PLAYER_SE3
            || m4a_sequencer_init_chained_player(&seq2, &seq, seq.trackBase + seq.maxTracks, M4A_PLAYER_CRY0) != 0) {
            fprintf(stderr, "Error: --cry2 over --song needs the song on bgm, se1, se2 or se3 (--player; got %s)\n",
                    kPlayerNames[o->player]);
            m4a_sequencer_detach(&seq);
            m4a_engine_destroy(&engine);
            voicegroup_free(vg);
            m4a_song_free(song);
            m4a_song_free(song2);
            voicegroup_free(vg2);
            return -1;
        }
        sCry2.players[0] = &seq2;      /* cry player 0; cry player 1 is not opened in this form */
        sCry2.players[1] = NULL;
        printf("  Cry over the song: StartCry on VBlank %d (its first tick on engine frame %d), cry player 0 chained after %s on engine tracks %u..%u\n",
               startFrame2, startFrame2 + 2, kPlayerNames[o->player], seq2.trackBase, seq2.trackBase + seq2.maxTracks - 1);
    }
    if (song2) {
        sCry2.song = song2;
        sCry2.voices = vg2->voices;
        sCry2.startFrame = (uint32_t)startFrame2;
        sCry2.hook = engine.frameHook;
        engine.frameHook = cry2_hook;
    }
    if (o->player != M4A_PLAYER_BGM)
        printf("  Music player: %s (%u tracks)\n", kPlayerNames[o->player], seq.maxTracks);
    if (song->trackCount > seq.maxTracks)
        printf("  Note: the player has %u tracks; the song's other %u are not played\n",
               seq.maxTracks, song->trackCount - seq.maxTracks);
    if (o->psgLogPath) {
        sPsgLog.f = fopen(o->psgLogPath, "w");
        sPsgLog.engine = &engine;
        engine.psg[0].writeLog = psg_log_cb;
    }
    FILE *seqLog = o->seqLogPath ? fopen(o->seqLogPath, "w") : NULL;
    seq.traceFile = seqLog;
    if (song2)
        seq2.traceFile = seqLog;    /* one log: both players' lines, the frame line by the last */
    /* Replay the hardware boot with its output kept, so the render lines up
     * with an emulator recording from reset: the song's first frame is the
     * first SoundMain after the one the game catches on boot. */
    m4a_engine_boot(&engine, true);
    m4a_sequencer_start(&seq, song);

    /* Render plan.  A fixed --total-duration-seconds renders exactly that
     * long (fadeout in the final --fadeout seconds).  Otherwise the song
     * decides: FINE on every track ends it after --tail seconds, and the
     * --loop-count'th pass through its GOTO starts the fadeout. */
    const double rate = o->sampleRate;
    uint64_t fadeSamps = (uint64_t)(o->fadeoutSeconds * rate + 0.5);
    uint64_t tailSamps = (uint64_t)(o->tailSeconds * rate + 0.5);
    uint64_t total = 0, fadeStart = UINT64_MAX, cap = 0;
    float *l = NULL, *r = NULL;
    bool fixed = o->totalDurSeconds >= 0.0;
    uint64_t limit = fixed ? (uint64_t)(o->totalDurSeconds * rate + 0.5)
                           : (uint64_t)(10.0 * 60.0 * rate); /* safety cap (~230 MB at 48 kHz) */
    if (fixed) {
        fadeStart = limit > fadeSamps ? limit - fadeSamps : 0;
        printf("  Total render: %.2f s (fadeout starts %.3f s)\n",
               (double)limit / rate, (double)fadeStart / rate);
    }
    uint64_t endAt = UINT64_MAX;  /* dynamic: set when FINE / loop count reached */

    printf("Rendering...\n");
    fflush(stdout);
    const int block = 256;
    while (total < limit && total < endAt) {
        uint64_t n = block;
        if (total + n > limit) n = limit - total;
        if (total + n > endAt) n = endAt - total;
        if (grow_buffers(&l, &r, &cap, total + n, limit) != 0) {
            fprintf(stderr, "Out of memory allocating audio buffers\n");
            if (seqLog) fclose(seqLog);
            if (sPsgLog.f) { fclose(sPsgLog.f); sPsgLog.f = NULL; }
            if (song2) { m4a_sequencer_detach(&seq2); voicegroup_free(vg2); m4a_song_free(song2); }
            m4a_sequencer_detach(&seq);
            m4a_engine_destroy(&engine);
            voicegroup_free(vg);
            m4a_song_free(song);
            return -1;
        }
        m4a_engine_process(&engine, l + total, r + total, (int)n);
        total += n;
        if (sCry2.refused) {
            fprintf(stderr, "Error: --cry2 at %.3f s: neither cry player is free (both busy: the game replaces the "
                            "longer-playing one, which this renderer does not do)\n", (double)total / rate);
            if (seqLog) fclose(seqLog);
            if (sPsgLog.f) { fclose(sPsgLog.f); sPsgLog.f = NULL; }
            m4a_sequencer_detach(&seq2);
            m4a_sequencer_detach(&seq);
            m4a_engine_destroy(&engine);
            voicegroup_free(vg);
            voicegroup_free(vg2);
            m4a_song_free(song);
            m4a_song_free(song2);
            free(l);
            free(r);
            return -1;
        }

        if (!fixed && endAt == UINT64_MAX) {
            bool done = m4a_sequencer_finished(&seq)
                     && (!song2 || (sCry2.song == NULL && m4a_sequencer_finished(&seq2)));
            if (done) {
                endAt = total + tailSamps;
                printf("  Song ended (FINE) at %.3f s; %.2f s tail\n",
                       (double)total / rate, o->tailSeconds);
            } else if (seq.loopPasses >= (uint32_t)o->loopCount) {
                fadeStart = total;
                endAt = total + fadeSamps;
                printf("  Loop pass %u reached at %.3f s; fading out over %.2f s\n",
                       seq.loopPasses, (double)total / rate, o->fadeoutSeconds);
            }
        }
    }
    if (!fixed && endAt == UINT64_MAX)
        fprintf(stderr, "Warning: stopped at the %.0f s safety limit (no FINE or GOTO reached)\n",
                (double)limit / rate);

    printf("  %u notes, %u song ticks\n", seq.noteCount, seq.clock);
    if (song2 && o->cry < 0) {
        if (sCry2.startedOn == 0)
            printf("  Cry on cry player 0 over the song: %u notes, %u song ticks\n", seq2.noteCount, seq2.clock);
        else
            printf("  The cry never started (the render ended before VBlank %d)\n", startFrame2);
    } else if (song2) {
        if (sCry2.startedOn == 1)
            printf("  Second cry on cry player 1: %u notes, %u song ticks\n", seq2.noteCount, seq2.clock);
        else if (sCry2.startedOn == 0)
            printf("  Second cry on cry player 0 (the first cry had finished before VBlank %d; the counts above are its restart)\n", startFrame2);
        else
            printf("  Second cry never started (the render ended before VBlank %d)\n", startFrame2);
    }
    if (fadeStart != UINT64_MAX && fadeStart < total) {
        uint64_t fs = total - fadeStart;
        for (uint64_t i = 0; i < fs; i++) {
            float gain = 1.0f - (float)i / (float)fs;
            l[fadeStart + i] *= gain;
            r[fadeStart + i] *= gain;
        }
    }

    if (seqLog)
        fclose(seqLog);
    if (sPsgLog.f) {
        fclose(sPsgLog.f);
        sPsgLog.f = NULL;
    }
    if (song2) {
        m4a_sequencer_detach(&seq2);
        voicegroup_free(vg2);
        m4a_song_free(song2);
    }
    m4a_sequencer_detach(&seq);
    m4a_engine_destroy(&engine);
    voicegroup_free(vg);
    m4a_song_free(song);
    *outL = l;
    *outR = r;
    *outTotal = total;
    *outFadeStart = fadeStart;
    return 0;
}

static int load_cry2(const SongRenderOpts *o, M4ASong **outSong, LoadedVoiceGroup **outVg);

static int render_song(const SongRenderOpts *o, float **outL, float **outR,
                       uint64_t *outTotal, uint64_t *outFadeStart)
{
    char err[256];
    printf("Parsing song file: %s\n", o->songPath);
    M4ASong *song = m4a_song_load(o->songPath, err, sizeof(err));
    if (!song) {
        fprintf(stderr, "Failed to parse %s: %s\n", o->songPath, err);
        return -1;
    }
    printf("  %s: %u tracks, %u bytes, priority %u, reverb %s%u, voicegroup %s\n",
           song->name, song->trackCount, song->size, song->priority,
           (song->reverb & 0x80) ? "" : "(unset) ", song->reverb & 0x7F, song->voicegroup);

    const char *vgName = o->voicegroup ? o->voicegroup : song->voicegroup;
    if (o->voicegroup && strcmp(o->voicegroup, song->voicegroup) != 0)
        printf("  Voicegroup override: %s (song says %s)\n", o->voicegroup, song->voicegroup);

    printf("Loading voicegroup '%s' from %s...\n", vgName, o->projectRoot);
    fflush(stdout);
    LoadedVoiceGroup *vg = voicegroup_load(o->projectRoot, vgName, NULL);
    if (!vg) {
        fprintf(stderr, "Failed to load voicegroup '%s'\n", vgName);
        m4a_song_free(song);
        return -1;
    }
    printf("Voicegroup loaded successfully.\n");
    if (o->cry2 < 0)
        return render_sequenced(o, song, vg, NULL, NULL, 0, outL, outR, outTotal, outFadeStart);
    /* --cry2 over the song: a cry on cry player 0 chained after the song's player */
    M4ASong *song2;
    LoadedVoiceGroup *vg2;
    if (load_cry2(o, &song2, &vg2) != 0) {
        voicegroup_free(vg);
        m4a_song_free(song);
        return -1;
    }
    return render_sequenced(o, song, vg, song2, vg2, o->cry2StartFrame, outL, outR, outTotal, outFadeStart);
}

/* --cry2: the second cry's sample and its own template copy (the ROM's
 * gPokemonCrySongs[1], or [0] over a song) at CRY_MODE_NORMAL's defaults. */
static int load_cry2(const SongRenderOpts *o, M4ASong **outSong, LoadedVoiceGroup **outVg)
{
    char symbol2[128];
    uint8_t type2 = VOICE_CRY;
    if (voicegroup_cry_table_entry(o->projectRoot, o->cry2, symbol2, sizeof(symbol2), &type2) != 0) {
        fprintf(stderr, "Error: no gCryTable entry %d in %s/sound/cry_tables.inc (--cry2)\n", o->cry2, o->projectRoot);
        return -1;
    }
    printf("Cry %d (%s, --cry2): %s%s\n", o->cry2, o->cry >= 0 ? "second" : "over the song", symbol2,
           (type2 & VOICE_TYPE_REV) ? " (reversed: gCryTable_Reverse's entry)" : "");
    fflush(stdout);
    LoadedVoiceGroup *vg2 = voicegroup_load_cry_typed(o->projectRoot, symbol2, type2, NULL);
    if (!vg2) {
        fprintf(stderr, "Failed to load cry sample '%s'\n", symbol2);
        return -1;
    }
    const WaveData *wd2 = vg2->voices[0].wav;
    printf("  %s: %u samples at %u Hz, %s%s%s\n", symbol2, wd2->size, wd2->freq >> 10,
           (vg2->voices[0].type & VOICE_TYPE_CMP) ? "DPCM" : "uncompressed (no built .bin: plays as DirectSound)",
           (vg2->voices[0].type & VOICE_TYPE_REV) ? ", played in reverse" : "",
           (wd2->status & 0x4000) ? ", loops" : "");
    M4ACryParams cp2;
    m4a_cry_params_default(&cp2);
    M4ASong *song2 = m4a_cry_song_new(&cp2);
    if (!song2) {
        voicegroup_free(vg2);
        return -1;
    }
    printf("  %s (%s): the defaults, %u track, StartCry on VBlank %d\n", song2->name,
           o->cry >= 0 ? "second" : "over the song", song2->trackCount, o->cry2StartFrame);
    *outSong = song2;
    *outVg = vg2;
    return 0;
}

/* --cry: gCryTable[id] through the cry song template (m4a_cry.h), as the
 * ROM's StartCry / the game's PlayCryInternal play it. */
static int render_cry(const SongRenderOpts *o, float **outL, float **outR,
                      uint64_t *outTotal, uint64_t *outFadeStart)
{
    char symbol[128];
    uint8_t type = VOICE_CRY;
    if (voicegroup_cry_table_entry(o->projectRoot, o->cry, symbol, sizeof(symbol), &type) != 0) {
        fprintf(stderr, "Error: no gCryTable entry %d in %s/sound/cry_tables.inc\n", o->cry, o->projectRoot);
        return -1;
    }
    printf("Cry %d: %s%s\n", o->cry, symbol, (type & VOICE_TYPE_REV) ? " (reversed: gCryTable_Reverse's entry)" : "");
    fflush(stdout);
    LoadedVoiceGroup *vg = voicegroup_load_cry_typed(o->projectRoot, symbol, type, NULL);
    if (!vg) {
        fprintf(stderr, "Failed to load cry sample '%s'\n", symbol);
        return -1;
    }
    const WaveData *wd = vg->voices[0].wav;
    printf("  %s: %u samples at %u Hz, %s%s%s\n", symbol, wd->size, wd->freq >> 10,
           (vg->voices[0].type & VOICE_TYPE_CMP) ? "DPCM" : "uncompressed (no built .bin: plays as DirectSound)",
           (vg->voices[0].type & VOICE_TYPE_REV) ? ", played in reverse" : "",
           (wd->status & 0x4000) ? ", loops" : "");
    M4ACryParams cp = o->cryParams;
    M4ASong *song = m4a_cry_song_new(&cp);
    if (!song) {
        voicegroup_free(vg);
        return -1;
    }
    printf("  %s: volume %u, pan %d, pitch %d, length %u, release %u, progress %u, chorus %d, priority %u, %u track%s\n",
           song->name, cp.volume, cp.pan, cp.pitch, cp.length, cp.release, cp.progress, cp.chorus, cp.priority,
           song->trackCount, song->trackCount == 1 ? "" : "s");
    if (o->cry2 < 0)
        return render_sequenced(o, song, vg, NULL, NULL, 0, outL, outR, outTotal, outFadeStart);
    M4ASong *song2;
    LoadedVoiceGroup *vg2;
    if (load_cry2(o, &song2, &vg2) != 0) {
        voicegroup_free(vg);
        m4a_song_free(song);
        return -1;
    }
    return render_sequenced(o, song, vg, song2, vg2, o->cry2StartFrame, outL, outR, outTotal, outFadeStart);
}

/* Loop-expansion bookkeeping: which notes are keyed on (note-on emitted, no
 * note-off yet), per engine track and key, so each loop wrap can release the
 * ones whose note-off lies beyond the loop end.  keyedEv keeps the note-on
 * event so a synthetic note-off dispatches to the same engine track. */
static void track_keyed_note(bool keyed[MAX_TRACKS][128],
                             RenderEvent keyedEv[MAX_TRACKS][128],
                             const RenderEvent *ev, int useTrackIndex)
{
    int idx = useTrackIndex ? ev->track : ev->channel;
    if (idx >= MAX_TRACKS || ev->data0 >= 128)
        return;
    if (ev->type == 0x9) {
        keyed[idx][ev->data0] = true;
        keyedEv[idx][ev->data0] = *ev;
    } else if (ev->type == 0x8) {
        keyed[idx][ev->data0] = false;
    }
}

/* Order RenderEvents by sample position (for the gate-carry merge). */
static int cmp_render_event_pos(const void *a, const void *b)
{
    uint64_t pa = ((const RenderEvent *)a)->samplePos;
    uint64_t pb = ((const RenderEvent *)b)->samplePos;
    return (pa > pb) - (pa < pb);
}

/*
 * --gba-start-delay: the output sample MIDI time 0 is dispatched at.  The
 * ROM's first song tick is frame 3's SoundMain (MPlayStart runs on frame 2):
 * 124773 + 2 x 280896 + 160 x 1232 + 197 = 883882 cycles, m4a_engine.c's
 * FRAME1_START and SOUNDMAIN_FRAME_OFFSET.  An event dispatched after s
 * output samples finds the machine up to one blip frame (GBA_BLIP_FRAME)
 * past sample s's cycle, since m4a_engine_process runs it until the blip
 * holds the samples; an event at MIDI time 0 must still precede that
 * SoundMain, so the origin is the last sample a blip frame before it
 * (2522 at 48 kHz, dispatched at cycle 882538; plan, milestone 5 slice 2a).
 * ROM_FIRST_TICK_CYCLE is in gba_tempo_grid.h.
 */
static uint64_t gba_start_delay_samples(double sampleRate)
{
    return (uint64_t)((double)(ROM_FIRST_TICK_CYCLE - GBA_BLIP_FRAME) * sampleRate
                      / (double)GBA_CPU_HZ);
}

/*
 * --gba-tempo-grid with a loop: the exact-time bound the loop expansion must
 * reach so that every event the grid places before `totalSamples` exists.
 * An event's grid time is at most (largest grid / smallest exact time per
 * tick) of its exact time, an event at grid time g at most (largest exact /
 * smallest grid) x g in exact time.  A loop pass is whole clocks
 * (passRatio = its clocks / its ticks in clocks, which scales the grid's
 * time per tick inside the loop); the frame rounding and the dispatch
 * guard add under two frames and a tick's truncation to its clock up to one
 * clock: four frames and two of the longest clocks are allowed.
 */
static uint64_t grid_expand_limit(uint64_t totalSamples, const TempoEvent *tempos, int tempoCount,
                                  uint32_t tpqn, uint32_t clocksPerBeat, double passRatio,
                                  double sampleRate)
{
    /* Seconds per SMF tick: exact = us / 1e6 / tpqn; on the grid
     * 150 / tempoI frames per clock, clocksPerBeat / tpqn clocks per tick. */
    double exMin = 1e300, exMax = 0.0, gMin = 1e300, gMax = 0.0;
    const double frameSec = (double)GBA_CYCLES_PER_FRAME / (double)GBA_CPU_HZ;
    bool exDefault = (tempoCount == 0 || tempos[0].tick != 0);
    bool gDefault  = true;  /* MPlayStart's 150 until a first-MTrk TEMPO at tick 0 */
    for (int i = 0; i < tempoCount; i++)
        if (tempos[i].track == 0 && tempos[i].tick == 0) gDefault = false;
    for (int i = -1; i < tempoCount; i++) {
        if (i < 0 ? exDefault : true) {
            double us = i < 0 ? 500000.0 : (double)tempos[i].tempo;
            double s = us / 1e6 / tpqn;
            if (s < exMin) exMin = s;
            if (s > exMax) exMax = s;
        }
        if (i < 0 ? gDefault : tempos[i].track == 0) {
            uint32_t ti = i < 0 ? 150 : gba_tempo_grid_tempo_i(tempos[i].tempo, clocksPerBeat);
            if (ti == 0)
                continue;   /* the ROM stops ticking: nothing after it is placed */
            double s = 150.0 / ti * frameSec * clocksPerBeat / tpqn;
            if (s < gMin) gMin = s;
            if (s > gMax) gMax = s;
        }
    }
    if (passRatio < 1.0) gMin *= passRatio;
    if (passRatio > 1.0) gMax *= passRatio;
    double kx = exMax / gMin, kg = gMax / exMin;
    double clockSec = gMax * tpqn / clocksPerBeat;
    double k = kx * kg;
    if (kx > k) k = kx;
    if (k < 1.0) k = 1.0;
    return (uint64_t)(k * ((double)totalSamples + (4.0 * frameSec + 2.0 * clockSec) * sampleRate)) + 1;
}

/* Dispatch one RenderEvent to the engine.  gridClocksPerBeat != 0
 * (--gba-tempo-grid): a TEMPO the ROM plays reaches the engine as the
 * grid's tempoI from the next frame's add, the others not at all. */
static void dispatch_event(M4AEngine *engine, const RenderEvent *ev,
                            int useTrackIndex, uint32_t gridClocksPerBeat)
{
    /* For Type 1 MIDIs, use the SMF track number as the engine track index
     * so that tracks sharing the same MIDI channel get separate engine tracks.
     * For Type 0, fall back to MIDI channel. */
    int trackIdx = useTrackIndex ? ev->track : ev->channel;

    switch (ev->type) {
    case RENDER_EVT_TEMPO: /* Synthetic tempo change — drives LFO/portamento rate */
        if (gridClocksPerBeat) {
            if (ev->gridTempoUs != 0)
                m4a_engine_set_tick_tempo(engine,
                                          (uint16_t)gba_tempo_grid_tempo_i(ev->gridTempoUs, gridClocksPerBeat));
            break;
        }
        m4a_engine_set_tempo_bpm(engine,
                                 (double)(((int)ev->data1 << 7) | ev->data0));
        break;
    case 0x8: /* Note Off */
        m4a_engine_note_off(engine, trackIdx, ev->data0);
        break;
    case 0x9: /* Note On */
        m4a_engine_note_on(engine, trackIdx, ev->data0, ev->data1);
        break;
    case 0xB: /* Control Change */
        m4a_engine_cc(engine, trackIdx, ev->data0, ev->data1);
        break;
    case 0xC: /* Program Change */
        m4a_engine_program_change(engine, trackIdx, ev->data0);
        break;
    case 0xE: /* Pitch Bend — convert MIDI 14-bit unsigned to signed -8192..+8191 */
    {
        int16_t bend = (int16_t)(((int)(ev->data1 << 7) | ev->data0) - 8192);
        m4a_engine_pitch_bend(engine, trackIdx, bend);
        break;
    }
    }
}

/* Render a block of frames, chunked to fit in int */
static void render_frames(M4AEngine *engine, float *outL, float *outR,
                           uint64_t startSample, uint64_t frameCount)
{
    uint64_t remaining = frameCount;
    uint64_t pos = startSample;
    while (remaining > 0) {
        int chunk = (remaining > 0x7FFFFFFF) ? 0x7FFFFFFF : (int)remaining;
        m4a_engine_process(engine, outL + pos, outR + pos, chunk);
        pos       += (uint64_t)chunk;
        remaining -= (uint64_t)chunk;
    }
}

static int output_audio(const float *outL, const float *outR, uint64_t totalSamples,
                        int sampleRateHz, const char *outputPath, bool doPlay);

int main(int argc, char *argv[])
{
    /* Line-buffer stdout even when piped so progress prints stay visible
     * (diagnosing a hang otherwise requires stdbuf). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const char *projectRoot   = argv[1];
    /* The voicegroup positional is optional with --song (the header names it). */
    const char *vgName        = (strncmp(argv[2], "--", 2) == 0) ? NULL : argv[2];
    int         firstOpt      = vgName ? 3 : 2;
    const char *midiPath      = NULL;
    const char *songPath      = NULL;
    int         cryId         = -1;
    int         cry2Id        = -1;
    int         cry2Start     = 0;
    M4ACryParams cryParams;
    bool        cryParamsSet  = false;
    m4a_cry_params_default(&cryParams);
    bool        songVolumeSet = false;
    bool        reverbSet     = false;
    const char *outputPath    = NULL;
    bool        doPlay        = false;
    int         songVolume    = 127;
    int         reverbAmount  = 0;
    bool        respectBaseMidiKey = false;
    bool        portamento    = false;
    bool        pwm           = false;
    bool        quantize      = false;  /* --quantize: the MIDI event latch */
    bool        gbaStartDelay = false;  /* --gba-start-delay: MIDI time 0 at the first song tick */
    bool        gbaTempoGrid  = false;  /* --gba-tempo-grid: events on MPlayMain's tempoC / 150 ticks */
    int         maxChannels   = 5;
    int         sampleRateHz  = 44100;
    int         pcmFreqMode   = M4A_PCM_FREQ_MODE_DEFAULT; /* SOUND_MODE_FREQ_13379 */
    double      tailSeconds   = 3.0;
    int         loopCount     = 2;
    bool        extendedClocks = false; /* mid2agb -X: 48 clocks/beat */
    double      fadeoutSeconds = 5.0;
    double      totalDurSeconds = -1.0; /* -1 = not set */
    const char *psgLogPath    = NULL;
    const char *seqLogPath    = NULL;
    int         player        = M4A_PLAYER_BGM;
    bool        playerSet     = false;

    for (int i = firstOpt; i < argc; i++) {
        if (strcmp(argv[i], "--midi") == 0 && i + 1 < argc) {
            midiPath = argv[++i];
        } else if (strcmp(argv[i], "--song") == 0 && i + 1 < argc) {
            songPath = argv[++i];
        } else if (strcmp(argv[i], "--cry") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 0 || v > 0xFFFF) {
                fprintf(stderr, "Error: --cry takes a gCryTable index (got '%s')\n", argv[i]);
                return 1;
            }
            cryId = (int)v;
        } else if (strcmp(argv[i], "--cry2") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 0 || v > 0xFFFF) {
                fprintf(stderr, "Error: --cry2 takes a gCryTable index (got '%s')\n", argv[i]);
                return 1;
            }
            cry2Id = (int)v;
        } else if (strcmp(argv[i], "--cry2-start-frame") == 0 && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 1 || v > 255) {
                fprintf(stderr, "Error: --cry2-start-frame takes a VBlank count 1..255 (the ROM's u8 startFrame2; got '%s')\n", argv[i]);
                return 1;
            }
            cry2Start = (int)v;
        } else if ((strcmp(argv[i], "--cry-pitch") == 0 || strcmp(argv[i], "--cry-pan") == 0
                    || strcmp(argv[i], "--cry-volume") == 0 || strcmp(argv[i], "--cry-length") == 0
                    || strcmp(argv[i], "--cry-release") == 0 || strcmp(argv[i], "--cry-progress") == 0
                    || strcmp(argv[i], "--cry-chorus") == 0)
                   && i + 1 < argc) {
            /* The setters' argument types: s16, s8, u8, u16, u8, u32, s8 (chorus:
             * PlayCryInternal's u32 192 becomes the s8 -64, so 128..255 are taken and wrapped). */
            const char *flag = argv[i];
            char *end = NULL;
            long long v = strtoll(argv[++i], &end, 10);
            long long lo = 0, hi = 0;
            if (strcmp(flag, "--cry-pitch") == 0)         { lo = -32768; hi = 32767; }
            else if (strcmp(flag, "--cry-pan") == 0)      { lo = -128; hi = 127; }
            else if (strcmp(flag, "--cry-volume") == 0)   { lo = 0; hi = 255; }
            else if (strcmp(flag, "--cry-length") == 0)   { lo = 0; hi = 65535; }
            else if (strcmp(flag, "--cry-release") == 0)  { lo = 0; hi = 255; }
            else if (strcmp(flag, "--cry-chorus") == 0)   { lo = -128; hi = 255; }
            else                                          { lo = 0; hi = 4294967295LL; }
            if (end == argv[i] || *end != '\0' || v < lo || v > hi) {
                fprintf(stderr, "Error: %s takes %lld..%lld (got '%s')\n", flag, lo, hi, argv[i]);
                return 1;
            }
            cryParamsSet = true;
            if (strcmp(flag, "--cry-pitch") == 0)         cryParams.pitch = (int16_t)v;
            else if (strcmp(flag, "--cry-pan") == 0)      cryParams.pan = (int8_t)v;
            else if (strcmp(flag, "--cry-volume") == 0)   cryParams.volume = (uint8_t)v;
            else if (strcmp(flag, "--cry-length") == 0)   cryParams.length = (uint16_t)v;
            else if (strcmp(flag, "--cry-release") == 0)  cryParams.release = (uint8_t)v;
            else if (strcmp(flag, "--cry-chorus") == 0)   cryParams.chorus = (int8_t)(uint8_t)v;
            else                                          cryParams.progress = (uint32_t)v;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (strcmp(argv[i], "--play") == 0) {
            doPlay = true;
        } else if (strcmp(argv[i], "--song-volume") == 0 && i + 1 < argc) {
            songVolume = atoi(argv[++i]);
            songVolumeSet = true;
            if (songVolume < 0)   songVolume = 0;
            if (songVolume > 127) songVolume = 127;
        } else if (strcmp(argv[i], "--reverb") == 0 && i + 1 < argc) {
            reverbAmount = atoi(argv[++i]);
            reverbSet = true;
            if (reverbAmount < 0)   reverbAmount = 0;
            if (reverbAmount > 127) reverbAmount = 127;
        } else if (strcmp(argv[i], "--respect-base-midi-key") == 0) {
            respectBaseMidiKey = true;
        } else if (strcmp(argv[i], "--quantize") == 0) {
            quantize = true;
        } else if (strcmp(argv[i], "--gba-start-delay") == 0) {
            gbaStartDelay = true;
        } else if (strcmp(argv[i], "--gba-tempo-grid") == 0) {
            gbaTempoGrid = true;
        } else if (strcmp(argv[i], "--portamento") == 0) {
            portamento = true;
        } else if (strcmp(argv[i], "--pwm") == 0) {
            pwm = true;
        } else if (strcmp(argv[i], "--polyphony") == 0 && i + 1 < argc) {
            maxChannels = atoi(argv[++i]);
            if (maxChannels < 1) maxChannels = 1;
            if (maxChannels > MAX_PCM_CHANNELS) maxChannels = MAX_PCM_CHANNELS;
        } else if (strcmp(argv[i], "--sample-rate") == 0 && i + 1 < argc) {
            sampleRateHz = atoi(argv[++i]);
            if (sampleRateHz < 8000) sampleRateHz = 8000;
        } else if (strcmp(argv[i], "--pcm-mix-rate") == 0 && i + 1 < argc) {
            double hz = atof(argv[++i]);
            pcmFreqMode = m4a_pcm_freq_mode_from_hz(hz);
            if ((int)hz != m4a_pcm_freq_mode_hz(pcmFreqMode))
                fprintf(stderr, "Note: --pcm-mix-rate %g is not an m4a sound-mode rate; using %d Hz\n",
                        hz, m4a_pcm_freq_mode_hz(pcmFreqMode));
        } else if (strcmp(argv[i], "--psg-log") == 0 && i + 1 < argc) {
            psgLogPath = argv[++i];
        } else if (strcmp(argv[i], "--seq-log") == 0 && i + 1 < argc) {
            seqLogPath = argv[++i];
        } else if (strcmp(argv[i], "--player") == 0 && i + 1 < argc) {
            player = parse_player(argv[++i]);
            if (player < 0) {
                fprintf(stderr, "Error: --player must be bgm, se1, se2, se3, cry0 or cry1\n");
                return 1;
            }
            playerSet = true;
        } else if (strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            tailSeconds = atof(argv[++i]);
            if (tailSeconds < 0.0) tailSeconds = 0.0;
        } else if (strcmp(argv[i], "--loop-count") == 0 && i + 1 < argc) {
            loopCount = atoi(argv[++i]);
            if (loopCount < 1) loopCount = 1;
        } else if (strcmp(argv[i], "--extended-clocks") == 0) {
            extendedClocks = true;
        } else if (strcmp(argv[i], "--fadeout") == 0 && i + 1 < argc) {
            fadeoutSeconds = atof(argv[++i]);
            if (fadeoutSeconds < 0.0) fadeoutSeconds = 0.0;
        } else if (strcmp(argv[i], "--total-duration-seconds") == 0 && i + 1 < argc) {
            totalDurSeconds = atof(argv[++i]);
            if (totalDurSeconds < 0.0) totalDurSeconds = 0.0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if ((midiPath != NULL) + (songPath != NULL) + (cryId >= 0) != 1) {
        fprintf(stderr, "Error: exactly one of --midi, --song or --cry is required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (cryId >= 0 && vgName)
        fprintf(stderr, "Note: --cry ignores the <voicegroup> argument (the cry's sample is its voicegroup)\n");
    if (cryId < 0 && cryParamsSet)
        fprintf(stderr, "Note: --cry-pitch / --cry-pan / --cry-volume / --cry-length / --cry-release / --cry-progress / --cry-chorus only apply to --cry\n");
    if ((cry2Id >= 0) != (cry2Start > 0)) {
        fprintf(stderr, "Error: --cry2 <id> and --cry2-start-frame <F> go together\n");
        return 1;
    }
    if (cry2Id >= 0 && cryId < 0 && !songPath) {
        fprintf(stderr, "Error: --cry2 needs --cry (the first cry) or --song (the sound effect or song it plays over)\n");
        return 1;
    }
    if (cry2Id >= 0 && songPath && player > M4A_PLAYER_SE3) {
        fprintf(stderr, "Error: --cry2 over --song needs the song on bgm, se1, se2 or se3 (--player; got %s)\n",
                kPlayerNames[player]);
        return 1;
    }
    if (midiPath && !vgName) {
        fprintf(stderr, "Error: --midi needs a <voicegroup> argument\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (!outputPath && !doPlay) {
        fprintf(stderr, "Error: at least one of --output or --play is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    double sampleRate = (double)sampleRateHz;

    if (midiPath && player != M4A_PLAYER_BGM)
        fprintf(stderr, "Note: --player only applies to --song (the MIDI path has no music player)\n");

    if (songPath || cryId >= 0) {
        if (quantize)
            fprintf(stderr, "Note: --quantize only applies to --midi (the sequencer already runs on the tick)\n");
        if (gbaStartDelay)
            fprintf(stderr, "Note: --gba-start-delay only applies to --midi (--song / --cry already start on the game's frame)\n");
        if (gbaTempoGrid)
            fprintf(stderr, "Note: --gba-tempo-grid only applies to --midi (--song / --cry already run on the game's ticks)\n");
        if (songVolumeSet || reverbSet)
            fprintf(stderr, "Note: --song-volume / --reverb are ignored with --song / --cry "
                            "(the song data carries its own volume and reverb)\n");
        if (cryId >= 0 && !playerSet)
            player = M4A_PLAYER_CRY0;      /* SetPokemonCryTone from silence */
        SongRenderOpts o = {
            .projectRoot = projectRoot, .voicegroup = vgName, .songPath = songPath, .cry = cryId,
            .cryParams = cryParams, .cry2 = cry2Id, .cry2StartFrame = cry2Start,
            .respectBaseMidiKey = respectBaseMidiKey,
            .portamento = portamento, .pwm = pwm, .maxChannels = maxChannels,
            .sampleRate = sampleRate, .pcmFreqMode = pcmFreqMode,
            .tailSeconds = tailSeconds, .fadeoutSeconds = fadeoutSeconds,
            .totalDurSeconds = totalDurSeconds, .loopCount = loopCount,
            .psgLogPath = psgLogPath,
            .seqLogPath = seqLogPath,
            .player = player,
        };
        float *sl, *sr;
        uint64_t sTotal, sFade;
        if ((cryId >= 0 ? render_cry(&o, &sl, &sr, &sTotal, &sFade)
                        : render_song(&o, &sl, &sr, &sTotal, &sFade)) != 0)
            return 1;
        printf("Rendering complete.\n");
        int rc = output_audio(sl, sr, sTotal, sampleRateHz, outputPath, doPlay);
        free(sl);
        free(sr);
        return rc;
    }

    /* ---- Parse MIDI ---- */
    printf("Parsing MIDI file: %s\n", midiPath);
    fflush(stdout);

    uint64_t totalMidiSamples = 0;
    uint64_t loopStartSample  = UINT64_MAX;
    uint64_t loopEndSample    = UINT64_MAX;
    uint64_t loopStartTick    = UINT64_MAX;
    uint64_t loopEndTick      = UINT64_MAX;
    uint32_t tpqn             = 24;
    TempoArray tempoMap       = { NULL, 0, 0 };
    uint16_t midiFormat       = 0;
    RenderEventArray *events = parse_midi(midiPath, sampleRate, &totalMidiSamples,
                                           &loopStartSample, &loopEndSample,
                                           &loopStartTick, &loopEndTick,
                                           &tpqn, &tempoMap, &midiFormat);
    if (!events) return 1;

    /* For Type 1 MIDI files, use SMF track numbers as engine track indices.
     * This handles files where multiple tracks share the same MIDI channel
     * (each track gets its own program/voice in the M4A engine). */
    int useTrackIndex = (midiFormat == 1);

    printf("  %d MIDI events, raw duration: %.2f s\n",
           events->count, (double)totalMidiSamples / sampleRate);

    /* ---- Determine render plan ---- */
    bool hasLoop = (loopStartSample != UINT64_MAX &&
                    loopEndSample   != UINT64_MAX &&
                    loopEndSample   >  loopStartSample);

    if (!hasLoop && (loopStartSample != UINT64_MAX || loopEndSample != UINT64_MAX))
        fprintf(stderr, "Warning: incomplete loop markers (need both '[' and ']' "
                        "text events with loop end > loop start)\n");

    uint64_t      totalSamples;
    uint64_t      fadeStartSample = UINT64_MAX; /* UINT64_MAX = no fadeout */
    const RenderEvent *renderEvts;
    int            renderEvtCount;
    RenderEvent   *extEvts = NULL; /* allocated when loop is active */

    if (hasLoop) {
        uint64_t loopDuration  = loopEndSample - loopStartSample;
        uint64_t fadeoutSamps  = (uint64_t)(fadeoutSeconds * sampleRate + 0.5);

        if (totalDurSeconds >= 0.0) {
            totalSamples    = (uint64_t)(totalDurSeconds * sampleRate + 0.5);
            fadeStartSample = totalSamples > fadeoutSamps
                              ? totalSamples - fadeoutSamps : 0;
        } else {
            fadeStartSample = loopStartSample + (uint64_t)loopCount * loopDuration;
            totalSamples    = fadeStartSample + fadeoutSamps;
        }

        printf("  Loop region: [%.3f s, %.3f s] (%.3f s body)\n",
               (double)loopStartSample / sampleRate,
               (double)loopEndSample   / sampleRate,
               (double)loopDuration    / sampleRate);
        printf("  Fadeout: starts %.3f s, duration %.2f s\n",
               (double)fadeStartSample / sampleRate, fadeoutSeconds);

        /* Build extended event list:
         *   1. Pre-loop events (samplePos < loopStartSample) — played once.
         *   2. Loop body events (loopStartSample <= samplePos <= loopEndSample),
         *      repeated with increasing sample offsets until totalSamples.
         *
         * Iteration k has offset = k * loopDuration:
         *   iter 0 starts at loopStartSample  (original positions)
         *   iter 1 starts at loopEndSample     (seamless continuation)
         *   etc.
         *
         * Within each iteration events are added in original sorted order, so
         * note-offs at the loop boundary naturally precede the note-ons of the
         * next iteration at the same sample position.
         *
         * Notes get extra rules, matching the GBA (mid2agb orders the
         * loop-end GOTO after same-tick note-ends but before note-starts,
         * and the GOTO touches no sound-channel state):
         *   - A note-on exactly at the loop end is dropped — its note-off
         *     lies beyond the loop end, which looping playback never reaches,
         *     so it would be held (and re-triggered) forever.
         *   - A note still keyed on at a wrap crosses the loop end. If it is
         *     <= 96 mid2agb clocks, mid2agb emits a direct note command whose
         *     channel gate keeps counting through the GOTO: the note releases
         *     at its written duration, so its note-off is rescheduled at the
         *     wrapped position (gate-carry).
         *   - A longer crossing note becomes TIE + EOT with the EOT beyond
         *     the loop end, unreachable: no note-off is emitted at all, so it
         *     is held forever, stacking a fresh instance every pass — exactly
         *     as on hardware.
         */
        int extCap   = events->count + 256;
        int extCount = 0;
        extEvts = malloc((size_t)extCap * sizeof(RenderEvent));
        if (!extEvts) {
            fprintf(stderr, "Out of memory building event list\n");
            free(events->events); free(events);
            return 1;
        }

        bool keyed[MAX_TRACKS][128] = {{false}};
        RenderEvent keyedEv[MAX_TRACKS][128];
        memset(keyedEv, 0, sizeof(keyedEv));

        /* Gate-carried note-offs land inside a later pass, out of append
         * order, so they collect here and merge in at the end. */
        RenderEvent *carryOffs = NULL;
        int carryCount = 0, carryCap = 0;
        const uint64_t loopLenTicks = loopEndTick - loopStartTick;
        const uint64_t clocksPerBeat = extendedClocks ? 48 : 24;

        /* The passes are expanded in exact MIDI time up to expandLimit.  On
         * the tempo grid an event keeps its song tick but moves to the ROM's
         * frame, so the bound widens by the largest ratio of grid to exact
         * time the tempo map allows (a slower grid must not run out of
         * expanded events); what lands past the end is dropped below. */
        uint64_t expandLimit = totalSamples;
        if (gbaTempoGrid) {
            GbaTempoGrid gc;
            gba_tempo_grid_init(&gc, tpqn, (uint32_t)clocksPerBeat);
            double passClocks = (double)gba_tempo_grid_pass_clock(&gc, loopStartTick, 1, loopStartTick, loopEndTick)
                              - (double)gba_tempo_grid_clock(&gc, loopStartTick);
            double tickClocks = (double)loopLenTicks * clocksPerBeat / tpqn;
            expandLimit = grid_expand_limit(totalSamples, tempoMap.events, tempoMap.count,
                                            tpqn, (uint32_t)clocksPerBeat,
                                            tickClocks > 0.0 ? passClocks / tickClocks : 1.0, sampleRate);
        }

        /* Pre-loop */
        for (int i = 0; i < events->count; i++) {
            if (events->events[i].samplePos < loopStartSample) {
                if (extCount >= extCap) {
                    extCap = extCap * 2 + 16;
                    RenderEvent *p = realloc(extEvts, (size_t)extCap * sizeof(RenderEvent));
                    if (!p) { free(extEvts); extEvts = NULL; goto oom; }
                    extEvts = p;
                }
                extEvts[extCount++] = events->events[i];
                track_keyed_note(keyed, keyedEv, &events->events[i], useTrackIndex);
            }
        }

        /* Loop body iterations */
        if (loopDuration > 0) {
            for (uint64_t off = 0, pass = 0; loopStartSample + off < expandLimit; off += loopDuration, pass++) {
                for (int i = 0; i < events->count; i++) {
                    uint64_t op = events->events[i].samplePos;
                    if (op < loopStartSample || op > loopEndSample) continue;
                    if (events->events[i].type == 0x9 && op == loopEndSample) continue;
                    uint64_t sp = op + off;
                    if (sp >= expandLimit) continue;
                    if (extCount >= extCap) {
                        extCap = extCap * 2 + 16;
                        RenderEvent *p = realloc(extEvts, (size_t)extCap * sizeof(RenderEvent));
                        if (!p) { free(extEvts); extEvts = NULL; goto oom; }
                        extEvts = p;
                    }
                    RenderEvent ev = events->events[i];
                    ev.samplePos = sp;
                    ev.loopPass = (uint32_t)pass;
                    /* mid2agb sorts the loop end's GOTO (LoopEnd 0x12) before a
                     * TEMPO on the same clock (0x19; midi.cpp EventCompare), so
                     * that TEMPO is never played: no grid tempo change. */
                    if (ev.type == RENDER_EVT_TEMPO && op == loopEndSample)
                        ev.gridTempoUs = 0;
                    extEvts[extCount++] = ev;
                    track_keyed_note(keyed, keyedEv, &ev, useTrackIndex);
                }

                /* Notes still keyed on cross the loop end (their note-off
                 * lies beyond it): gate-carry the short ones, hold the tied
                 * ones forever — see the rules above. */
                uint64_t wrapPos = loopEndSample + off;
                for (int t = 0; t < MAX_TRACKS; t++) {
                    for (int k = 0; k < 128; k++) {
                        if (!keyed[t][k]) continue;
                        keyed[t][k] = false;

                        /* The note's off event: first matching off past the
                         * note-on, which is also how mid2agb pairs notes. */
                        const RenderEvent *noteOff = NULL;
                        for (int i = 0; i < events->count; i++) {
                            const RenderEvent *e = &events->events[i];
                            if (e->type != 0x8 || e->data0 != k) continue;
                            if ((useTrackIndex ? e->track : e->channel) != t) continue;
                            if (e->tick <= keyedEv[t][k].tick) continue;
                            noteOff = e;
                            break;
                        }
                        if (noteOff) {
                            uint64_t durTicks = noteOff->tick - keyedEv[t][k].tick;
                            uint64_t clocks = clocksPerBeat * durTicks / tpqn;
                            if (clocks == 0) clocks = 1;
                            if (clocks > 96)
                                continue; /* TIE + EOT: unreachable — held forever */

                            /* Gate-carry: the release lands rr ticks into a
                             * later pass (usually the next one).  rr ends up
                             * in (0, loopLenTicks] with passDelta >= 1.
                             *
                             * A note can also still be keyed on at the wrap
                             * with its off tick NOT past the loop end: the
                             * off event was clipped by the render-length
                             * limit (sp >= totalSamples) rather than by the
                             * loop end, or boundary rounding put it just
                             * outside the sample-position filter.  Then the
                             * release belongs inside the *current* pass at
                             * its own tick (passDelta 0) — computing it that
                             * way lands it at the off's original position,
                             * which the totalSamples check below drops in
                             * the clipped case, i.e. the note simply holds
                             * to the end of the render as it would on
                             * hardware.  (The old repeated-subtraction loop
                             * underflowed here and spun ~2^64 iterations.) */
                            uint64_t rr, passDelta;
                            if (noteOff->tick > loopEndTick && loopLenTicks > 0) {
                                uint64_t past = noteOff->tick - loopEndTick;
                                passDelta = 1 + (past - 1) / loopLenTicks;
                                rr = past - (passDelta - 1) * loopLenTicks;
                            } else {
                                passDelta = 0;
                                rr = noteOff->tick > loopStartTick
                                         ? noteOff->tick - loopStartTick
                                         : 0;
                            }
                            uint64_t releasePos =
                                tick_to_sample(loopStartTick + rr, tempoMap.events,
                                               tempoMap.count, tpqn, sampleRate)
                                + off + passDelta * loopDuration;
                            if (releasePos >= expandLimit) continue;
                            if (carryCount >= carryCap) {
                                carryCap = carryCap * 2 + 64;
                                RenderEvent *p = realloc(carryOffs,
                                                         (size_t)carryCap * sizeof(RenderEvent));
                                if (!p) {
                                    free(carryOffs); free(extEvts); extEvts = NULL;
                                    goto oom;
                                }
                                carryOffs = p;
                            }
                            RenderEvent offEv = keyedEv[t][k];
                            offEv.type = 0x8;
                            offEv.data1 = 0;
                            offEv.samplePos = releasePos;
                            offEv.songTick = loopStartTick + rr;
                            offEv.loopPass = (uint32_t)(pass + passDelta);
                            carryOffs[carryCount++] = offEv;
                            continue;
                        }

                        /* No note-off in the file: release at the wrap. */
                        if (wrapPos >= expandLimit) continue;
                        if (extCount >= extCap) {
                            extCap = extCap * 2 + 16;
                            RenderEvent *p = realloc(extEvts, (size_t)extCap * sizeof(RenderEvent));
                            if (!p) { free(carryOffs); free(extEvts); extEvts = NULL; goto oom; }
                            extEvts = p;
                        }
                        RenderEvent offEv = keyedEv[t][k];
                        offEv.type = 0x8;
                        offEv.data1 = 0;
                        offEv.samplePos = wrapPos;
                        offEv.songTick = loopEndTick;
                        offEv.loopPass = (uint32_t)pass;
                        extEvts[extCount++] = offEv;
                    }
                }
            }
        }

        /* Merge the gate-carried releases into the (already ordered)
         * expanded list.  At equal positions the release goes first: on
         * hardware the channel gate hits zero as the clock advances, before
         * that clock's track commands run. */
        if (carryCount > 0) {
            qsort(carryOffs, (size_t)carryCount, sizeof(RenderEvent),
                  cmp_render_event_pos);
            RenderEvent *merged = malloc((size_t)(extCount + carryCount)
                                         * sizeof(RenderEvent));
            if (!merged) { free(carryOffs); free(extEvts); extEvts = NULL; goto oom; }
            int a = 0, b = 0, o = 0;
            while (a < extCount || b < carryCount) {
                bool takeCarry;
                if (a >= extCount)          takeCarry = true;
                else if (b >= carryCount)   takeCarry = false;
                else takeCarry = (carryOffs[b].samplePos <= extEvts[a].samplePos);
                merged[o++] = takeCarry ? carryOffs[b++] : extEvts[a++];
            }
            free(extEvts);
            extEvts  = merged;
            extCount = o;
        }
        free(carryOffs);

        renderEvts      = extEvts;
        renderEvtCount  = extCount;
    } else {
        /* No loop: use original events + tail silence.  An explicit
         * --total-duration-seconds overrides the natural length exactly:
         * events beyond it are dropped (the render loop stops at
         * totalSamples), a longer duration pads with silence, and the
         * fadeout occupies the final --fadeout seconds just as in the
         * looped path. */
        if (totalDurSeconds >= 0.0) {
            uint64_t fadeoutSamps = (uint64_t)(fadeoutSeconds * sampleRate + 0.5);
            totalSamples    = (uint64_t)(totalDurSeconds * sampleRate + 0.5);
            fadeStartSample = totalSamples > fadeoutSamps
                              ? totalSamples - fadeoutSamps : 0;
            printf("  Fadeout: starts %.3f s, duration %.2f s\n",
                   (double)fadeStartSample / sampleRate, fadeoutSeconds);
        } else {
            uint64_t tailSamps = (uint64_t)(tailSeconds * sampleRate + 0.5);
            totalSamples       = totalMidiSamples + tailSamps;
        }
        renderEvts     = events->events;
        renderEvtCount = events->count;
    }

    /* --gba-tempo-grid: every event moves from its exact-tempo sample to the
     * ROM's frame for its song tick (gba_tempo_grid.h): the sample a blip
     * frame before that frame's SoundMain, relative to the origin's, so
     * grid frame 0 is MIDI time 0 and --gba-start-delay below still adds O.
     * Several ticks in one frame share one sample, in order.  The natural
     * tail and the loop-count fade start move onto the grid with them; an
     * explicit --total-duration-seconds is output time and stays.  Without
     * the flag every event keeps its exact-tempo sample (gridp NULL). */
    GbaTempoGrid grid;
    gba_tempo_grid_init(&grid, tpqn, extendedClocks ? 48 : 24);
    GbaTempoGrid *gridp = gbaTempoGrid ? &grid : NULL;  /* NULL: the exact samples, unchanged */
    {
        RenderEvent *evs = (RenderEvent *)renderEvts;  /* extEvts or events->events */
        const bool fadeOnGrid = gbaTempoGrid && hasLoop && totalDurSeconds < 0.0;
        const uint64_t fadeClock = fadeOnGrid
            ? gba_tempo_grid_pass_clock(&grid, loopStartTick, (uint64_t)loopCount, loopStartTick, loopEndTick)
            : 0;
        int64_t fadeFrame = -1;
        for (int i = 0; i < renderEvtCount; i++) {
            uint64_t clock = gba_tempo_grid_pass_clock(&grid, evs[i].songTick, evs[i].loopPass,
                                                       loopStartTick, loopEndTick);
            if (fadeOnGrid && fadeFrame < 0 && fadeClock <= clock)
                fadeFrame = gba_tempo_grid_frame(&grid, fadeClock);
            evs[i].samplePos = gba_tempo_grid_dispatch_sample(gridp, evs[i].samplePos, clock,
                                                              evs[i].type == RENDER_EVT_TEMPO ? evs[i].gridTempoUs : 0,
                                                              sampleRate);
        }
        const int64_t lastEventFrame = grid.lastFrame;  /* before a fade query past the events */
        if (fadeOnGrid && fadeFrame < 0)
            fadeFrame = gba_tempo_grid_frame(&grid, fadeClock);
        if (fadeOnGrid && fadeFrame == GBA_TEMPO_GRID_NEVER) {
            /* A TEMPO byte of 0 stopped the song before the fade: keep the
             * exact-tempo fade, the events after the stop are never played. */
            printf("  Note: a TEMPO of 0 stops the game's song before the fade; fade kept at %.3f s\n",
                   (double)fadeStartSample / sampleRate);
        } else if (fadeOnGrid) {
            uint64_t fadeoutSamps = (uint64_t)(fadeoutSeconds * sampleRate + 0.5);
            fadeStartSample = gba_tempo_grid_sample(fadeFrame, sampleRate) - gba_tempo_grid_sample(0, sampleRate);
            totalSamples    = fadeStartSample + fadeoutSamps;
            printf("  Fadeout: moved to %.3f s on the tempo grid\n",
                   (double)fadeStartSample / sampleRate);
        } else if (gbaTempoGrid && !hasLoop && totalDurSeconds < 0.0) {
            uint64_t tailSamps = (uint64_t)(tailSeconds * sampleRate + 0.5);
            const uint64_t never = gba_tempo_grid_sample(GBA_TEMPO_GRID_NEVER, sampleRate);
            uint64_t last = 0;
            for (int i = 0; i < renderEvtCount; i++)
                if (evs[i].type != RENDER_EVT_TEMPO   /* the last channel event, as totalMidiSamples */
                    && evs[i].samplePos < never - gba_tempo_grid_sample(0, sampleRate))
                    last = evs[i].samplePos;   /* the ticks a TEMPO of 0 never reaches are no part of it */
            totalSamples = last + tailSamps;
        }
        if (gbaTempoGrid && grid.frozen)
            printf("  MIDI ticks on the game's tempo grid (--gba-tempo-grid): a TEMPO of 0 stops the song,"
                   " its last tick on ROM frame %lld\n", (long long)(lastEventFrame + 3));
        else if (gbaTempoGrid)
            printf("  MIDI ticks on the game's tempo grid (--gba-tempo-grid): the last MIDI event's tick on"
                   " ROM frame %lld (events past the render end are dropped)\n", (long long)(lastEventFrame + 3));
    }

    /* --gba-start-delay: every event is dispatched O samples later.  The
     * render length and the fadeout move with it where they are MIDI time
     * (the tail after the last event, the loop-count fade); an explicit
     * --total-duration-seconds is output time and stays.  The loop
     * expansion above works in MIDI time, unchanged. */
    uint64_t midiOrigin = gbaStartDelay ? gba_start_delay_samples(sampleRate) : 0;
    if (gbaStartDelay) {
        if (totalDurSeconds < 0.0) {
            totalSamples += midiOrigin;
            if (fadeStartSample != UINT64_MAX) {
                fadeStartSample += midiOrigin;
                printf("  Fadeout: moved to %.3f s with the origin\n",
                       (double)fadeStartSample / sampleRate);
            }
        }
        printf("  MIDI time 0 at output sample %llu (--gba-start-delay)\n",
               (unsigned long long)midiOrigin);
    }

    printf("  Total render: %.2f s (%llu samples)\n",
           (double)totalSamples / sampleRate,
           (unsigned long long)totalSamples);

    free(tempoMap.events); /* only needed while expanding the loop */
    tempoMap.events = NULL;

    if (0) {
oom:
        fprintf(stderr, "Out of memory building event list\n");
        free(events->events); free(events); free(tempoMap.events);
        return 1;
    }

    /* ---- Load voicegroup ---- */
    printf("Loading voicegroup '%s' from %s...\n", vgName, projectRoot);
    fflush(stdout);

    LoadedVoiceGroup *vg = voicegroup_load(projectRoot, vgName, NULL);
    if (!vg) {
        fprintf(stderr, "Failed to load voicegroup '%s'\n", vgName);
        free(extEvts);
        free(events->events);
        free(events);
        return 1;
    }
    printf("Voicegroup loaded successfully.\n");

    /* ---- Initialize engine ---- */
    M4AEngine engine;
    m4a_engine_init(&engine, (float)sampleRate);
    m4a_engine_set_voicegroup(&engine, vg->voices);
    m4a_engine_set_song_volume(&engine, (uint8_t)songVolume);
    m4a_reverb_set_amount(&engine.reverb, (uint8_t)reverbAmount);
    engine.maxPcmChannels = (uint8_t)maxChannels;
    engine.respectBaseMidiKey = respectBaseMidiKey;
    m4a_engine_set_portamento_enabled(&engine, portamento);
    m4a_engine_set_pwm_enabled(&engine, pwm);
    m4a_engine_set_pcm_freq_mode(&engine, pcmFreqMode);
    /* --psg-log on the MIDI path too, opened before the boot as the song
     * path does, so the two logs compare line for line (probe_diff.py). */
    if (psgLogPath) {
        sPsgLog.f = fopen(psgLogPath, "w");
        sPsgLog.engine = &engine;
        engine.psg[0].writeLog = psg_log_cb;
    }
    /* Keep the boot's output so MIDI and compiled-song renders of the same
     * material line up with each other and with an emulator recording. */
    m4a_engine_boot(&engine, true);
    /* The frame latch (plan, milestone 5): events wait for the next tick. */
    m4a_engine_set_quantize_events(&engine, quantize);
    if (quantize)
        printf("MIDI events latched to the VBlank tick (--quantize)\n");

    /* ---- Allocate output buffers ---- */
    float *outL = calloc(totalSamples, sizeof(float));
    float *outR = calloc(totalSamples, sizeof(float));
    if (!outL || !outR) {
        fprintf(stderr, "Out of memory allocating audio buffers (%llu samples)\n",
                (unsigned long long)totalSamples);
        free(outL); free(outR);
        m4a_engine_destroy(&engine);
        voicegroup_free(vg);
        free(extEvts);
        free(events->events);
        free(events);
        return 1;
    }

    /* ---- Rendering loop ---- */
    printf("Rendering...\n");
    fflush(stdout);

    uint64_t samplePos = 0;
    /* --gba-tempo-grid: the engine's LFO clock starts with the song, as
     * MPlayStart seeds it, at the origin (before frame 3's SoundMain).  The
     * boot's frame-2 tick already leaves that state (tempoI 150, tempoC 0),
     * so this only states it; the held TEMPOs are what move the steps. */
    const uint32_t gridClocksPerBeat = gbaTempoGrid ? (extendedClocks ? 48 : 24) : 0;
    bool songTempoStarted = !gbaTempoGrid;
    for (int i = 0; i < renderEvtCount; i++) {
        const RenderEvent *ev = &renderEvts[i];

        uint64_t evPos = ev->samplePos + midiOrigin;
        if (evPos >= totalSamples) break; /* safety: don't write past buffer */

        if (!songTempoStarted) {
            if (midiOrigin > samplePos) {
                render_frames(&engine, outL, outR, samplePos, midiOrigin - samplePos);
                samplePos = midiOrigin;
            }
            m4a_engine_start_song_tempo(&engine);
            songTempoStarted = true;
        }

        /* Render audio up to this event */
        if (evPos > samplePos)
            render_frames(&engine, outL, outR, samplePos, evPos - samplePos);

        samplePos = evPos;
        dispatch_event(&engine, ev, useTrackIndex, gridClocksPerBeat);
    }

    /* Render remaining frames (tail / fadeout section) */
    if (samplePos < totalSamples)
        render_frames(&engine, outL, outR, samplePos, totalSamples - samplePos);

    /* ---- Apply fadeout envelope ---- */
    if (fadeStartSample != UINT64_MAX && fadeStartSample < totalSamples) {
        uint64_t fadeSamps = totalSamples - fadeStartSample;
        for (uint64_t i = 0; i < fadeSamps; i++) {
            float gain = 1.0f - (float)i / (float)fadeSamps;
            outL[fadeStartSample + i] *= gain;
            outR[fadeStartSample + i] *= gain;
        }
    }

    printf("Rendering complete.\n");

    int rc = output_audio(outL, outR, totalSamples, sampleRateHz, outputPath, doPlay);

    /* ---- Cleanup ---- */
    if (sPsgLog.f) {
        fclose(sPsgLog.f);
        sPsgLog.f = NULL;
    }
    free(outL);
    free(outR);
    m4a_engine_destroy(&engine);
    voicegroup_free(vg);
    free(extEvts);
    free(events->events);
    free(events);

    return rc;
}

/* Write and/or play the rendered buffers. */
static int output_audio(const float *outL, const float *outR, uint64_t totalSamples,
                        int sampleRateHz, const char *outputPath, bool doPlay)
{
    /* ---- WAV output ---- */
    if (outputPath) {
        printf("Writing %s...\n", outputPath);
        if (write_wav(outputPath, outL, outR, totalSamples, sampleRateHz) == 0)
            printf("Done: %s\n", outputPath);
    }

    /* ---- Speaker playback via miniaudio ---- */
    if (doPlay) {
        printf("Playing audio...\n");
        fflush(stdout);

#ifdef __linux__
        /* Suppress ALSA's verbose "cannot find card" error spam.  On WSL
         * there is no ALSA hardware, so miniaudio will fall back to
         * PulseAudio (provided by WSLg on Windows 11). */
        suppress_alsa_errors();

        /* Try PulseAudio before ALSA so that WSLg's PulseAudio server is
         * found without probing ALSA at all. */
        ma_backend linuxBackends[] = { ma_backend_pulseaudio, ma_backend_alsa };
        ma_context context;
        bool hasContext = (ma_context_init(linuxBackends, 2, NULL, &context) == MA_SUCCESS);
#endif

        PlaybackCtx ctx = { outL, outR, totalSamples, 0 };

        ma_device_config cfg  = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format   = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate        = (ma_uint32)sampleRateHz;
        cfg.dataCallback      = playback_callback;
        cfg.pUserData         = &ctx;

        ma_device device;
#ifdef __linux__
        ma_result initResult = ma_device_init(hasContext ? &context : NULL, &cfg, &device);
#else
        ma_result initResult = ma_device_init(NULL, &cfg, &device);
#endif
        if (initResult != MA_SUCCESS) {
            fprintf(stderr, "Failed to initialize audio playback device.\n");
#ifdef __linux__
            fprintf(stderr, "On WSL, audio requires PulseAudio (WSLg on Windows 11 provides this).\n");
#endif
        } else {
            if (ma_device_start(&device) != MA_SUCCESS) {
                fprintf(stderr, "Failed to start audio playback device\n");
            } else {
                while (ctx.pos < ctx.total)
                    ma_sleep(100);
            }
            ma_device_uninit(&device);
        }

#ifdef __linux__
        if (hasContext)
            ma_context_uninit(&context);
#endif

        printf("Playback complete.\n");
    }

    return 0;
}
