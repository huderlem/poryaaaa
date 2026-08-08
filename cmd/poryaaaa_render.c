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
#include "voicegroup_loader.h"

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
        int32_t l = (int32_t)(left[i]  * 32767.0f);
        int32_t r = (int32_t)(right[i] * 32767.0f);
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
                TempoEvent te = { tick, tempo };
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
        tempoEvts[te++] = make_tempo_event(sp, 60000000.0 / (double)tempos.events[i].tempo);
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
        "\n"
        "Required:\n"
        "  <project_root>              Path to pokeemerald/pokefirered project root\n"
        "  <voicegroup>                Voicegroup name (e.g. petalburg)\n"
        "  --midi <file.mid>           MIDI input file\n"
        "\n"
        "Output (at least one required):\n"
        "  --output <file.wav>         Write rendered audio to WAV file\n"
        "  --play                      Play audio through computer speakers\n"
        "\n"
        "Audio options:\n"
        "  --song-volume <0-127>       Song master volume (default: 127)\n"
        "  --reverb <0-127>            Reverb amount (default: 0)\n"
        "  --analog-filter             Enable GBA analog low-pass filter (default: off)\n"
        "  --respect-base-midi-key     Opt-in: treat a PCM voice's key as the sample's base MIDI note (default: off)\n"
        "  --portamento                Opt-in: enable the portamento glide effect, CC 5 (default: off)\n"
        "  --pwm                       Opt-in: enable pulse-width modulation on CGB square channels, CC 0x17/0x19 (default: off)\n"
        "  --polyphony <1-15>          Max simultaneous PCM channels (default: 5)\n"
        "  --sample-rate <hz>          Sample rate in Hz (default: 44100)\n"
        "  --pcm-mix-rate <hz>         DirectSound (PCM) mix rate; 0 means same as sample-rate (default: 13379)\n"
        "  --tail <seconds>            Silence after last event, no loop markers (default: 3.0)\n"
        "\n"
        "Loop options (when MIDI contains '[' / ']' text events):\n"
        "  --loop-count <n>            Number of loop body repetitions (default: 2)\n"
        "  --extended-clocks           Song is built with mid2agb -X (48 clocks/beat),\n"
        "                              which raises the length at which a note crossing\n"
        "                              the loop end is tied (held forever, as on hardware)\n"
        "  --fadeout <seconds>         Fadeout duration after final loop (default: 5.0)\n"
        "  --total-duration-seconds <s>  Override loop-count; set exact total duration\n"
        "                                (fadeout occupies the final --fadeout seconds)\n",
        prog);
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

/* Dispatch one RenderEvent to the engine */
static void dispatch_event(M4AEngine *engine, const RenderEvent *ev,
                            int useTrackIndex)
{
    /* For Type 1 MIDIs, use the SMF track number as the engine track index
     * so that tracks sharing the same MIDI channel get separate engine tracks.
     * For Type 0, fall back to MIDI channel. */
    int trackIdx = useTrackIndex ? ev->track : ev->channel;

    switch (ev->type) {
    case RENDER_EVT_TEMPO: /* Synthetic tempo change — drives LFO/portamento rate */
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
    const char *vgName        = argv[2];
    const char *midiPath      = NULL;
    const char *outputPath    = NULL;
    bool        doPlay        = false;
    int         songVolume    = 127;
    int         reverbAmount  = 0;
    bool        analogFilter  = false;
    bool        respectBaseMidiKey = false;
    bool        portamento    = false;
    bool        pwm           = false;
    int         maxChannels   = 5;
    int         sampleRateHz  = 44100;
    float       pcmMixRate    = 13379.0f; /* GBA-accurate DirectSound mix rate; 0 = host rate */
    double      tailSeconds   = 3.0;
    int         loopCount     = 2;
    bool        extendedClocks = false; /* mid2agb -X: 48 clocks/beat */
    double      fadeoutSeconds = 5.0;
    double      totalDurSeconds = -1.0; /* -1 = not set */

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--midi") == 0 && i + 1 < argc) {
            midiPath = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (strcmp(argv[i], "--play") == 0) {
            doPlay = true;
        } else if (strcmp(argv[i], "--song-volume") == 0 && i + 1 < argc) {
            songVolume = atoi(argv[++i]);
            if (songVolume < 0)   songVolume = 0;
            if (songVolume > 127) songVolume = 127;
        } else if (strcmp(argv[i], "--reverb") == 0 && i + 1 < argc) {
            reverbAmount = atoi(argv[++i]);
            if (reverbAmount < 0)   reverbAmount = 0;
            if (reverbAmount > 127) reverbAmount = 127;
        } else if (strcmp(argv[i], "--analog-filter") == 0) {
            analogFilter = true;
        } else if (strcmp(argv[i], "--respect-base-midi-key") == 0) {
            respectBaseMidiKey = true;
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
            pcmMixRate = (float)atof(argv[++i]);
            if (pcmMixRate < 0.0f) pcmMixRate = 0.0f;
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

    if (!midiPath) {
        fprintf(stderr, "Error: --midi is required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (!outputPath && !doPlay) {
        fprintf(stderr, "Error: at least one of --output or --play is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    double sampleRate = (double)sampleRateHz;

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
            for (uint64_t off = 0; loopStartSample + off < totalSamples; off += loopDuration) {
                for (int i = 0; i < events->count; i++) {
                    uint64_t op = events->events[i].samplePos;
                    if (op < loopStartSample || op > loopEndSample) continue;
                    if (events->events[i].type == 0x9 && op == loopEndSample) continue;
                    uint64_t sp = op + off;
                    if (sp >= totalSamples) continue;
                    if (extCount >= extCap) {
                        extCap = extCap * 2 + 16;
                        RenderEvent *p = realloc(extEvts, (size_t)extCap * sizeof(RenderEvent));
                        if (!p) { free(extEvts); extEvts = NULL; goto oom; }
                        extEvts = p;
                    }
                    RenderEvent ev = events->events[i];
                    ev.samplePos = sp;
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
                            if (releasePos >= totalSamples) continue;
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
                            carryOffs[carryCount++] = offEv;
                            continue;
                        }

                        /* No note-off in the file: release at the wrap. */
                        if (wrapPos >= totalSamples) continue;
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
    engine.analogFilter = analogFilter;
    engine.maxPcmChannels = (uint8_t)maxChannels;
    engine.respectBaseMidiKey = respectBaseMidiKey;
    m4a_engine_set_portamento_enabled(&engine, portamento);
    m4a_engine_set_pwm_enabled(&engine, pwm);
    m4a_engine_set_pcm_mix_rate(&engine, pcmMixRate);

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
    for (int i = 0; i < renderEvtCount; i++) {
        const RenderEvent *ev = &renderEvts[i];

        if (ev->samplePos >= totalSamples) break; /* safety: don't write past buffer */

        /* Render audio up to this event */
        if (ev->samplePos > samplePos)
            render_frames(&engine, outL, outR, samplePos,
                          ev->samplePos - samplePos);

        samplePos = ev->samplePos;
        dispatch_event(&engine, ev, useTrackIndex);
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

    /* ---- Cleanup ---- */
    free(outL);
    free(outR);
    m4a_engine_destroy(&engine);
    voicegroup_free(vg);
    free(extEvts);
    free(events->events);
    free(events);

    return 0;
}
