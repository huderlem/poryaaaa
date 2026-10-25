/*
 * mgba timing probe: runs the accuracy ROM under libmgba's debugger and logs
 * every sound/DMA/timer IO write and the entry of the m4a driver functions
 * with the CPU cycle, mgba frame counter and VCOUNT.  This is how the cycle
 * offsets in plugin/m4a_engine.c (boot table, VSync/SoundMain/CgbSound
 * offsets) were measured.  Not built by CMake; compile by hand:
 *
 *   gcc -O1 -o mgba_probe test/accuracy/ref/mgba_probe.c -I$MGBA/include \
 *       -I$MGBA/build/include $MGBA/build/libmgba-static.a -lz -lpng -lm \
 *       -lpthread -lrt -ldl /usr/lib/x86_64-linux-gnu/libEGL.so /usr/lib/x86_64-linux-gnu/libGL.so
 *   ./mgba_probe build/accuracy/rom/accuracy_test.gba out.txt <frames> <type> <id> \
 *       build/accuracy/rom/accuracy_test.map [player [pitch [pan [volume [length [release [chorus [id2 [startFrame2]]]]]]]]]
 *
 * type/id as in mgba_render (0 = song, 1 = cry, 2 = micro, id = line in
 * micro.cfg from 0, the song id or the gCryTable index).  The ninth to
 * fourteenth arguments are the cry parameters StartCry passes to
 * SetPokemonCryPitch / Panpot / Volume / Length / Release / Chorus
 * (mgba_render's --pitch / --pan / --volume / --length / --release /
 * --chorus; defaults 15360, 0, 120, 140, 0, 0 as mgba_render's; a nonzero
 * chorus makes the cry song two tracks), ignored by the other types.  The
 * fifteenth and sixteenth arguments start a second cry (gCryTable index,
 * CRY_MODE_NORMAL's defaults) on the given VBlank while the first test plays
 * (mgba_render's --id2 / --start-frame2; start frame 0 = none).  Filter FIFO
 * writes (040000A0/A4) out of the log to
 * see the rest.  `player` (eighth argument, default gMPlayInfo_BGM) names
 * the MusicPlayerInfo symbol whose track flags and tempoC the `.frames`
 * summary reads: gMPlayInfo_SE1 / SE2 / SE3 for a sound effect (the song
 * table's `ms` field); an array element is `symbol[N]`, N * 0x40 (one
 * MusicPlayerInfo) past the symbol, e.g. gPokemonCryMusicPlayers[1] for cry
 * player 1.  The breakpoint log itself covers every player (r7
 * at MPlayMain's labels is the MusicPlayerInfo).
 *
 * Besides the raw log, `<out>.frames` gets one line per engine frame k
 * (frame k >= 1 starts at cycle 124773 + (k - 1) * 280896; cycles are given
 * relative to the start of VBlank line 160 of that frame):
 *
 *   entry cycles of m4aSoundMain, SoundMain, MPlayMain, CgbSound and
 *   SoundMainRAM_Buffer; MPlayMain calls; ply_note calls; all ply_* handler
 *   calls; tracks with MPT_FLG_EXIST and MPT_FLG_START in gMPlayInfo_BGM at
 *   MPlayMain entry; tempoC at entry; SoundInfo.c15 before and after
 *   CgbSound; the m4aSoundVSync entry relative to line 150; and every PSG
 *   register write (address low byte = value @ cycle), in order.
 *
 * With an `nm` listing of the ROM's ELF as the symbol file (seventh
 * argument; `arm-none-eabi-nm build/accuracy_test.elf`, which also holds the
 * local labels the map lacks), breakpoints are set inside MPlayMain's track
 * loop, ply_note's channel search and the VOLCHG/PITCHG pass (the `_081DD*`
 * labels of m4a_1.s) and each one logs r3-r5 and r7, so a per-track, per-note
 * and per-channel cost can be read from the raw log (tools/probe_mplay.py).
 * The map still works and gives the function entries only.  Lines of the
 * form "<hex addr> t CgbSound_<name>" appended to the nm listing add
 * breakpoints inside CgbSound (compiled C, no labels of its own):
 * CgbSound_loop 0x80010ac (per-channel loop head), _active 0x80010c2,
 * _step2 0x800113c, _start 0x8001154, _nostart 0x8001232, _oscoff
 * 0x8001256, _step 0x80012a4 (envelope_step_repeat), _modvol 0x80012b8,
 * _stepdone 0x8001398, _envdone 0x80013aa, _pit 0x80013b4, _vol
 * 0x800142c, _done 0x80014a8, _next 0x80014ac in the 2026-10-25 ROM with
 * the second-cry parameters (build/accuracy/rom/accuracy_test.elf, the
 * CMake build; CgbSound at 0x800107c -- every ROM symbol from AgbMain on
 * moved +0x40 when main.c's StartTest grew, the body is unchanged; the
 * 2026-10-25 ROM with the cry chorus parameter had CgbSound at 0x800103c
 * and the labels 0x40 lower, the 2026-10-24 ROM with the length / release
 * parameters 0x8001020, the one before it 0x8000ff0; re-derive from
 * `objdump -d` if m4a.c, main.c or the toolchain changes).
 *
 * `<out>.events` summarizes mgba's own scheduler lateness: for the timer-0
 * overflow, the audio sample event and the PSG frame-sequencer event, the
 * histogram of cyclesLate and the number of times a 65536 Hz DAC slot
 * boundary fell between the event's due time and the time it was processed
 * (the "deferred PCM floor" from the output-stage review).  Those hooks
 * replace the mTimingEvent callbacks in place after reset and forward to the
 * originals; the emulation is unaffected.
 */
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/debugger/debugger.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/timer.h>
#include <mgba/internal/gba/video.h>
#include <mgba/internal/arm/arm.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME1_START 124773
#define CYCLES_PER_FRAME 280896
#define CYCLES_PER_LINE 1232
#define LINE160 (160 * CYCLES_PER_LINE)
#define LINE150 (150 * CYCLES_PER_LINE)

/* IWRAM symbols read at breakpoints, taken from the map (fallbacks are the
 * 2026-10-24 build's addresses) */
static uint32_t ADDR_gSoundInfo = 0x03000f70;
static uint32_t ADDR_gMPlayInfo_BGM = 0x03002008;   /* the player the .frames summary reads (see main) */
#define O_SoundInfo_c15    10
#define O_MPlayInfo_trackCount 8
#define O_MPlayInfo_tempoC 0x22
#define O_MPlayInfo_tracks 0x2C
#define SIZEOF_MusicPlayerTrack 0x50
#define MPT_FLG_START 0x40
#define MPT_FLG_EXIST 0x80

static struct GBA* gGba;
static struct mCore* gCore;
static FILE* gOut;
static long gFrames;
struct Sym { uint32_t addr; const char* name; };
static struct Sym gSyms[256]; static int gNumSyms;

#define MAX_FRAMES 4096
#define MAX_WRITES 96
struct FrameRec {
	int used;
	int32_t m4aSoundMain, soundMain, mplayMain, cgbSound, soundMainRAM, vsync;
	int mplayMainCalls, plyNote, plyAll, tracksExist, tracksStart, trackCount, tempoC;
	int c15Before, c15After, cgbSoundCalls;
	int numWrites;
	struct { uint8_t reg; uint8_t value; int32_t cycle; } writes[MAX_WRITES];
	int plyCount[256];
};
static struct FrameRec* gFrameRecs;

/* ---- mgba scheduler lateness hooks ---- */
struct LateStats {
	const char* name;
	long events;
	long late[8];        /* histogram: 0, 1-7, 8-15, 16-31, 32-63, 64-127, 128-255, 256+ */
	long slotCrossed;    /* a 256-cycle DAC slot boundary between due and processing time */
	long maxLate;
	long slotCrossedFirst; /* cycle of the first crossing, -1 if none */
	long untilShifted;   /* timer0 only: mgba's own `until` arithmetic changed (misses window ends) */
	long phaseCrossed;   /* psgframe only: a square's duty step fell between due and processing time */
};
static struct LateStats gTimer0 = { "timer0", 0, {0}, 0, 0, -1, 0, 0 };
static struct LateStats gSample = { "sample", 0, {0}, 0, 0, -1, 0, 0 };
static struct LateStats gPsgFrame = { "psgframe", 0, {0}, 0, 0, -1, 0, 0 };
static void (*gOrigTimer0)(struct mTiming*, void*, uint32_t);
static void (*gOrigSample)(struct mTiming*, void*, uint32_t);
static void (*gOrigPsgFrame)(struct mTiming*, void*, uint32_t);

static void lateRecord(struct LateStats* s, uint32_t late, int crossed) {
	s->events++;
	int bin = 0;
	if (late >= 256) bin = 7; else if (late >= 128) bin = 6; else if (late >= 64) bin = 5; else if (late >= 32) bin = 4;
	else if (late >= 16) bin = 3; else if (late >= 8) bin = 2; else if (late >= 1) bin = 1;
	s->late[bin]++;
	if (late > (uint32_t) s->maxLate) s->maxLate = late;
	if (crossed) {
		s->slotCrossed++;
		if (s->slotCrossedFirst < 0) s->slotCrossedFirst = mTimingCurrentTime(&gGba->timing);
	}
}

/* Does a DAC slot boundary (lastSample + k * sampleInterval) fall in (due, now]? */
static int slotCrossed(int32_t now, uint32_t late) {
	struct GBAAudio* audio = &gGba->audio;
	int32_t interval = audio->sampleInterval;
	if (interval <= 0) return 0;
	int32_t due = now - (int32_t) late;
	int32_t a = due - audio->lastSample, b = now - audio->lastSample;
	if (a < 0) a -= interval - 1; /* floor for negatives */
	if (b < 0) b -= interval - 1;
	return (a / interval) != (b / interval);
}

/* mgba's slot choice in GBAAudioSampleFIFO uses mTimingUntil at the processing
 * time; this reproduces that arithmetic with and without the lateness. */
static int fifoSlotShifted(uint32_t late) {
	struct GBAAudio* audio = &gGba->audio;
	int res = GBARegisterSOUNDBIASGetResolution(audio->soundbias);
	int32_t untilNow = mTimingUntil(&gGba->timing, &audio->sampleEvent) - 1;
	int32_t untilDue = untilNow + (int32_t) late;
	int bits = 2 << res;
	int32_t u1 = (untilNow + (1 << (9 - res))) >> (9 - res);
	int32_t u2 = (untilDue + (1 << (9 - res))) >> (9 - res);
	if (u1 > bits) u1 = bits;
	if (u2 > bits) u2 = bits;
	return u1 != u2;
}

static void hookTimer0(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	int32_t now = mTimingCurrentTime(timing);
	/* A boundary of the 256-cycle DAC grid in (due, now] moves the FIFO
	 * sample one slot later (GBAAudioSampleFIFO picks the slot from the
	 * processing time), including at a 1024-cycle window end, which the
	 * `until` arithmetic alone cannot see once the sample event has run. */
	int crossed = slotCrossed(now, cyclesLate);
	if (fifoSlotShifted(cyclesLate)) gTimer0.untilShifted++;
	lateRecord(&gTimer0, cyclesLate, crossed);
	if (crossed && gOut) fprintf(gOut, "%10d f%u v%3d L timer0 late=%u due=%d (FIFO sample lands one DAC slot later)\n", now, gGba->video.frameCounter, gGba->video.vcount, cyclesLate, now - (int32_t) cyclesLate);
	gOrigTimer0(timing, context, cyclesLate);
}
static void hookSample(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	/* _sample passes the corrected time to GBAAudioSample; lateness only
	 * matters for the FIFO/PSG events that interleave with it. */
	lateRecord(&gSample, cyclesLate, slotCrossed(mTimingCurrentTime(timing), cyclesLate));
	gOrigSample(timing, context, cyclesLate);
}
/* GBAudioUpdateFrame runs the PSG to the processing time, not the due time:
 * a square whose duty step falls in (due, now] has its pending DAC slot
 * sampled after that step instead of before it. */
static int squarePhaseCrossed(int32_t now, uint32_t late) {
	struct GBAudio* psg = &gGba->audio.psg;
	int32_t due = now - (int32_t) late;
	int n = 0;
	if (psg->playingCh1) {
		int32_t period = 4 * (2048 - psg->ch1.control.frequency) * psg->timingFactor;
		if (period > 0 && (now - psg->ch1.lastUpdate) / period != (due - psg->ch1.lastUpdate) / period) n++;
	}
	if (psg->playingCh2) {
		int32_t period = 4 * (2048 - psg->ch2.control.frequency) * psg->timingFactor;
		if (period > 0 && (now - psg->ch2.lastUpdate) / period != (due - psg->ch2.lastUpdate) / period) n++;
	}
	return n;
}
static void hookPsgFrame(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	int32_t now = mTimingCurrentTime(timing);
	int crossed = slotCrossed(now, cyclesLate);
	int phase = cyclesLate ? squarePhaseCrossed(now, cyclesLate) : 0;
	gPsgFrame.phaseCrossed += phase;
	if (phase && gOut) fprintf(gOut, "%10d f%u v%3d L psgframe late=%u square duty step between due and processing (%d ch)\n", now, gGba->video.frameCounter, gGba->video.vcount, cyclesLate, phase);
	lateRecord(&gPsgFrame, cyclesLate, crossed);
	if (crossed && gOut) fprintf(gOut, "%10d f%u v%3d L psgframe late=%u due=%d (slot sampled before the frame step)\n", now, gGba->video.frameCounter, gGba->video.vcount, cyclesLate, now - (int32_t) cyclesLate);
	gOrigPsgFrame(timing, context, cyclesLate);
}
static void installHooks(void) {
	if (gGba->timers[0].event.callback != hookTimer0) {
		gOrigTimer0 = gGba->timers[0].event.callback;
		gGba->timers[0].event.callback = hookTimer0;
	}
	if (gGba->audio.sampleEvent.callback != hookSample) {
		gOrigSample = gGba->audio.sampleEvent.callback;
		gGba->audio.sampleEvent.callback = hookSample;
	}
	if (gGba->audio.psg.frameEvent.callback != hookPsgFrame) {
		gOrigPsgFrame = gGba->audio.psg.frameEvent.callback;
		gGba->audio.psg.frameEvent.callback = hookPsgFrame;
	}
}

static void quietLog(struct mLogger* l, int c, enum mLogLevel lv, const char* f, va_list a) { (void)l;(void)c;(void)lv;(void)f;(void)a; }
static struct mLogger sLogger = { .log = quietLog };

static int symIndex(uint32_t a) {
	for (int i = 0; i < gNumSyms; ++i) if ((gSyms[i].addr & ~1u) == (a & ~1u)) return i;
	return -1;
}
static const char* symName(uint32_t a) { int i = symIndex(a); return i < 0 ? "?" : gSyms[i].name; }

static struct FrameRec* frameFor(int32_t t, int32_t* rel) {
	if (t < FRAME1_START) { *rel = t; return NULL; }
	int k = (t - FRAME1_START) / CYCLES_PER_FRAME + 1;
	*rel = (t - FRAME1_START) % CYCLES_PER_FRAME - LINE160;
	if (k >= MAX_FRAMES) return NULL;
	struct FrameRec* fr = &gFrameRecs[k];
	if (!fr->used) {
		fr->used = 1;
		fr->m4aSoundMain = fr->soundMain = fr->mplayMain = fr->cgbSound = fr->soundMainRAM = fr->vsync = -999999;
		fr->c15Before = fr->c15After = -1;
		fr->tempoC = -1;
	}
	return fr;
}

static uint8_t rd8(uint32_t a) { return (uint8_t) gCore->busRead8(gCore, a); }
static uint16_t rd16(uint32_t a) { return (uint16_t) gCore->busRead16(gCore, a); }
static uint32_t rd32(uint32_t a) { return gCore->busRead32(gCore, a); }

static void entered(struct mDebugger* d, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
	int32_t t = mTimingCurrentTime(&gGba->timing);
	unsigned fc = gGba->video.frameCounter;
	int vc = gGba->video.vcount;
	int32_t rel;
	struct FrameRec* fr = frameFor(t, &rel);
	if (reason == DEBUGGER_ENTER_WATCHPOINT) {
		fprintf(gOut, "%10d f%u v%3d W %08X <- %08X (old %08X)\n", t, fc, vc, info->address, info->type.wp.newValue, info->type.wp.oldValue);
		if (fr && info->address >= 0x04000060 && info->address < 0x040000A0 && fr->numWrites < MAX_WRITES) {
			fr->writes[fr->numWrites].reg = (uint8_t) (info->address & 0xFF);
			fr->writes[fr->numWrites].value = (uint8_t) info->type.wp.newValue;
			fr->writes[fr->numWrites].cycle = rel;
			fr->numWrites++;
		}
	} else if (reason == DEBUGGER_ENTER_BREAKPOINT) {
		int si = symIndex(info->address);
		const char* name = si < 0 ? "?" : gSyms[si].name;
		fprintf(gOut, "%10d f%u v%3d B %08X %s", t, fc, vc, info->address, name);
		if (!strcmp(name, "CgbSound")) {
			int c15 = rd8(ADDR_gSoundInfo + O_SoundInfo_c15);
			fprintf(gOut, " c15=%d", c15);
			if (fr) { fr->cgbSound = rel; fr->c15Before = c15; fr->cgbSoundCalls++; }
		} else if (!strcmp(name, "SoundMainRAM_Buffer")) {
			int c15 = rd8(ADDR_gSoundInfo + O_SoundInfo_c15);
			fprintf(gOut, " c15=%d", c15);
			if (fr) { fr->soundMainRAM = rel; fr->c15After = c15; }
		} else if (!strcmp(name, "MPlayMain")) {
			int n = rd8(ADDR_gMPlayInfo_BGM + O_MPlayInfo_trackCount);
			int tempoC = rd16(ADDR_gMPlayInfo_BGM + O_MPlayInfo_tempoC);
			uint32_t tracks = rd32(ADDR_gMPlayInfo_BGM + O_MPlayInfo_tracks);
			int exist = 0, start = 0;
			for (int i = 0; i < n && i < 16; ++i) {
				int flags = rd8(tracks + i * SIZEOF_MusicPlayerTrack);
				if (flags & MPT_FLG_EXIST) exist++;
				if (flags & MPT_FLG_START) start++;
			}
			fprintf(gOut, " trackCount=%d exist=%d start=%d tempoC=%d", n, exist, start, tempoC);
			if (fr) {
				if (fr->mplayMainCalls == 0) { fr->mplayMain = rel; fr->trackCount = n; fr->tracksExist = exist; fr->tracksStart = start; fr->tempoC = tempoC; }
				fr->mplayMainCalls++;
			}
		} else if (!strcmp(name, "m4aSoundMain")) { if (fr) fr->m4aSoundMain = rel; }
		else if (!strcmp(name, "SoundMain")) { if (fr) fr->soundMain = rel; }
		else if (!strcmp(name, "m4aSoundVSync")) { if (fr) fr->vsync = rel + LINE160 - LINE150; }
		else if (!strncmp(name, "ply_", 4)) {
			if (fr) { fr->plyAll++; if (si >= 0 && si < 256) fr->plyCount[si]++; if (!strcmp(name, "ply_note")) fr->plyNote++; }
		}
		if (!strncmp(name, "ply_", 4) && strcmp(name, "ply_note")) {
			/* (mplayInfo, track): the track's pending flags decide the handler's path */
			struct ARMCore* cpu = gGba->cpu;
			fprintf(gOut, " flg=%02X", rd8(cpu->gprs[1]));
		} else if (!strcmp(name, "ply_note")) {
			/* (cmd, mplayInfo, track): flags, voice type and the lfoDelay that picks the clear_modM path */
			struct ARMCore* cpu = gGba->cpu;
			fprintf(gOut, " flg=%02X vt=%02X lfodl=%02X", rd8(cpu->gprs[2]), rd8(cpu->gprs[2] + 0x24), rd8(cpu->gprs[2] + 0x1B));
		}
		if (name[0] == '_' || !strncmp(name, "TrackStop_", 10) || !strncmp(name, "CgbSound_", 9) || !strcmp(name, "CgbModVol") || !strcmp(name, "ply_note") || !strcmp(name, "TrackStop") || !strcmp(name, "ClearChain") || !strcmp(name, "RealClearChain") || !strcmp(name, "TrkVolPitSet")) {
			/* r5 = track (MPlayMain, ply_note), r4 = channel, r7 = player
			 * (MPlayMain) / chosen track (ply_note scan), r3 = loop count */
			struct ARMCore* cpu = gGba->cpu;
			fprintf(gOut, " r0=%08X r3=%08X r4=%08X r5=%08X r7=%08X r6=%08X", cpu->gprs[0], cpu->gprs[3], cpu->gprs[4], cpu->gprs[5], cpu->gprs[7], cpu->gprs[6]);
			if (!strcmp(name, "CgbModVol"))        /* r0 = CgbChannel: CgbPan's and the clamp's inputs */
				fprintf(gOut, " rv=%02X lv=%02X sus=%02X", rd8(cpu->gprs[0] + 2), rd8(cpu->gprs[0] + 3), rd8(cpu->gprs[0] + 6));
			if (!strncmp(name, "CgbSound_", 9))   /* r4 = CgbChannel, r6 = ch: the state its branches test */
				fprintf(gOut, " ch%d st=%02X mod=%02X envc=%02X envv=%02X goal=%02X iec=%02X n4=%02X swp=%02X", cpu->gprs[6], rd8(cpu->gprs[4]), rd8(cpu->gprs[4] + 0x1D), rd8(cpu->gprs[4] + 0x0B), rd8(cpu->gprs[4] + 0x09), rd8(cpu->gprs[4] + 0x0A), rd8(cpu->gprs[4] + 0x0D), rd8(cpu->gprs[4] + 0x1A), rd8(cpu->gprs[4] + 0x1F));
			if (!strcmp(name, "RealClearChain"))   /* r0 = channel: its track / prev / next decide the unlink path */
				fprintf(gOut, " trk=%08X prev=%08X next=%08X", rd32(cpu->gprs[0] + 0x2C), rd32(cpu->gprs[0] + 0x30), rd32(cpu->gprs[0] + 0x34));
			else if (!strcmp(name, "_081DD938"))   /* r5 = track: wait and the LFO fields the tick tests */
				fprintf(gOut, " wait=%02X lfos=%02X mod=%02X lfodlc=%02X", rd8(cpu->gprs[5] + 1), rd8(cpu->gprs[5] + 0x19), rd8(cpu->gprs[5] + 0x17), rd8(cpu->gprs[5] + 0x1C));
			else if (!strcmp(name, "_081DDBC4"))   /* r4 = cgbChans, r6 = type: the target channel's status and priority */
				fprintf(gOut, " cgb%d st=%02X prio=%02X", cpu->gprs[6], rd8(cpu->gprs[4] + (cpu->gprs[6] - 1) * 0x40), rd8(cpu->gprs[4] + (cpu->gprs[6] - 1) * 0x40 + 0x13));
			else if (!strcmp(name, "_081DDC40"))   /* r5 = track: whether its chain is empty (the beq at _081DDC4C) */
				fprintf(gOut, " chan=%08X", rd32(cpu->gprs[5] + 0x20));
			else if (!strcmp(name, "TrkVolPitSet"))   /* r1 = track: the fields its C code branches on */
				fprintf(gOut, " flg=%02X vol=%02X volX=%02X pan=%02X panX=%02X modT=%02X modM=%02X", rd8(cpu->gprs[1]), rd8(cpu->gprs[1] + 0x12), rd8(cpu->gprs[1] + 0x13), rd8(cpu->gprs[1] + 0x14), rd8(cpu->gprs[1] + 0x15), rd8(cpu->gprs[1] + 0x18), rd8(cpu->gprs[1] + 0x16));
		}
		fputc('\n', gOut);
	}
	d->state = DEBUGGER_RUNNING;
}
static void paused(struct mDebugger* d) { d->state = DEBUGGER_RUNNING; }

/* The ROM's parameter block (test/accuracy/rom/main.c struct AccParams,
 * as mgba_render patches it): type, volume, id, pitch (s16), pan (s8),
 * start frame 1, cry length (u16), cry release (u8), cry chorus (s8). */
static int patchParams(uint8_t* rom, size_t size, int type, int id, int pitch, int pan, int volume, int length, int release, int chorus, int id2, int startFrame2) {
	for (size_t off = 0; off + 32 <= size; off += 4) {
		if (memcmp(rom + off, "PORYACC1", 8) == 0) {
			uint8_t* b = rom + off + 8;
			b[0] = type; b[1] = (uint8_t) volume; b[2] = id & 0xFF; b[3] = id >> 8;
			b[4] = (uint8_t) (pitch & 0xFF); b[5] = (uint8_t) ((pitch >> 8) & 0xFF); b[6] = (uint8_t) (int8_t) pan; b[7] = 1;
			b[8] = (uint8_t) (length & 0xFF); b[9] = (uint8_t) ((length >> 8) & 0xFF); b[10] = (uint8_t) release;
			b[11] = (uint8_t) (int8_t) chorus;
			b[12] = id2 & 0xFF; b[13] = (id2 >> 8) & 0xFF; b[14] = (uint8_t) startFrame2;
			return 1;
		}
	}
	return 0;
}

static const char* regName(uint8_t lo) {
	switch (lo) {
	case 0x60: return "NR10"; case 0x62: return "NR11"; case 0x63: return "NR12"; case 0x64: return "NR13"; case 0x65: return "NR14";
	case 0x68: return "NR21"; case 0x69: return "NR22"; case 0x6C: return "NR23"; case 0x6D: return "NR24";
	case 0x70: return "NR30"; case 0x72: return "NR31"; case 0x73: return "NR32"; case 0x74: return "NR33"; case 0x75: return "NR34";
	case 0x78: return "NR41"; case 0x79: return "NR42"; case 0x7C: return "NR43"; case 0x7D: return "NR44";
	case 0x80: return "NR50"; case 0x81: return "NR51"; case 0x84: return "NR52"; case 0x82: return "SNDH_L"; case 0x83: return "SNDH_H";
	case 0x88: return "BIAS_L"; case 0x89: return "BIAS_H";
	default: return NULL;
	}
}

static void writeStats(FILE* f, struct LateStats* s) {
	fprintf(f, "%-9s events=%ld max_late=%ld slot_crossed=%ld (first at cycle %ld) until_shifted=%ld square_phase_crossed=%ld\n  late histogram: 0:%ld 1-7:%ld 8-15:%ld 16-31:%ld 32-63:%ld 64-127:%ld 128-255:%ld 256+:%ld\n",
	        s->name, s->events, s->maxLate, s->slotCrossed, s->slotCrossedFirst, s->untilShifted, s->phaseCrossed,
	        s->late[0], s->late[1], s->late[2], s->late[3], s->late[4], s->late[5], s->late[6], s->late[7]);
}

int main(int argc, char** argv) {
	if (argc < 6) { fprintf(stderr, "probe rom out.txt frames type id [map [player-symbol or symbol[N] [pitch [pan [volume [length [release [chorus [id2 [startFrame2]]]]]]]]]]\n"); return 1; }
	char playerSym[128]; strncpy(playerSym, argc > 7 ? argv[7] : "gMPlayInfo_BGM", sizeof playerSym - 1); playerSym[sizeof playerSym - 1] = 0;
	unsigned long playerIndex = 0;   /* "symbol[N]": the N-th MusicPlayerInfo (0x40 bytes each) from the symbol */
	{ char* br = strchr(playerSym, '['); if (br) { playerIndex = strtoul(br + 1, NULL, 10); *br = 0; } }
	int cryPitch = argc > 8 ? atoi(argv[8]) : 15360;   /* StartCry's parameters, mgba_render's defaults */
	int cryPan = argc > 9 ? atoi(argv[9]) : 0;
	int cryVolume = argc > 10 ? atoi(argv[10]) : 120;
	int cryLength = argc > 11 ? atoi(argv[11]) : 140;
	int cryRelease = argc > 12 ? atoi(argv[12]) : 0;
	int cryChorus = argc > 13 ? atoi(argv[13]) : 0;
	int cryId2 = argc > 14 ? atoi(argv[14]) : 0;          /* a second cry (mgba_render's --id2 / --start-frame2) */
	int cryStartFrame2 = argc > 15 ? atoi(argv[15]) : 0;
	int playerFound = 0;
	gFrames = atol(argv[3]);
	int type = atoi(argv[4]), id = atoi(argv[5]);
	FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t* rom = malloc(n); if (fread(rom, 1, n, f) != (size_t) n) return 1; fclose(f);
	patchParams(rom, n, type, id, cryPitch, cryPan, cryVolume, cryLength, cryRelease, cryChorus, cryId2, cryStartFrame2);
	gOut = fopen(argv[2], "w");
	gFrameRecs = calloc(MAX_FRAMES, sizeof *gFrameRecs);
	if (argc > 6) {
		FILE* m = fopen(argv[6], "r"); char line[512];
		const char* want[] = { "VBlankIntr", "VCountIntr", "IntrMain", "m4aSoundMain", "m4aSoundVSync", "SoundMain", "MPlayMain", "CgbSound", "MPlayStart", "SampleFreqSet", "m4aSoundInit", "AgbMain", "SoundMainRAM_Buffer", "SoundMainBTM", "StartTest", "SoundMainRAM", "TrackStop", "CgbModVol", "CgbOscOff", "MidiKeyToCgbFreq", "ChnVolSetAsm", "ClearChain",
		                       /* internal labels (nm listing only): MPlayMain's tick and apply loops, ply_note's search, TrackStop's loop */
		                       "RealClearChain", "TrkVolPitSet", "Clear64byte", "clear_modM", "MidiKeyToFreq", "FadeOutBody",
		                       "_081DD874", "_081DD87C", "_081DD886", "_081DD892", "_081DD8AE", "_081DD8B4", "_081DD8BA", "_081DD8E0", "_081DD8EC", "_081DD8F6", "_081DD90C", "_081DD92E", "_081DD938", "_081DD95A", "_081DD972", "_081DD994", "_081DD998", "_081DD9A4", "_081DD9BC",
		                       "_081DD9C4", "_081DD9C8", "_081DD9E6", "_081DD9F6", "_081DDA14", "_081DDA28", "_081DDA46", "_081DDA52", "_081DDA58", "_081DDA62", "_081DDA6C",
		                       "_081DDB46", "_081DDB66", "_081DDB68", "_081DDB80", "_081DDB94", "_081DDB98", "_081DDB9C", "_081DDBC4", "_081DDBEC", "_081DDBFA", "_081DDC14", "_081DDC18", "_081DDC24", "_081DDC32", "_081DDC34", "_081DDC40", "_081DDC54", "_081DDC66", "_081DDCA0", "_081DDCBC", "_081DDCCE", "_081DDCDC", "_081DDCEA",
		                       "TrackStop_Loop", "TrackStop_1", "TrackStop_2", "TrackStop_3", "TrackStop_Done",
		                       /* SoundMainRAM: the store that ends a DirectSound channel (statusFlags = 0 at m4a_1.s
		                        * _081DCFB0: a release or pseudo-echo tail reaching 0, or SF_START with SF_STOP), so a
		                        * channel's death frame can be read from the raw log after its track is gone */
		                       "_081DCFB0", NULL };
		while (m && fgets(line, sizeof line, m)) {
			unsigned long a; char name[128]; char kind;
			if (sscanf(line, " 0x%lx %127s", &a, name) == 2 || sscanf(line, "%lx %c %127s", &a, &kind, name) == 3) {
				int take = !strncmp(name, "ply_", 4) || !strncmp(name, "CgbSound_", 9);
				for (int i = 0; !take && want[i]; ++i) if (!strcmp(name, want[i])) take = 1;
				if (!strcmp(name, "gSoundInfo")) ADDR_gSoundInfo = (uint32_t) a;
				if (!strcmp(name, playerSym)) { ADDR_gMPlayInfo_BGM = (uint32_t) (a + playerIndex * 0x40); playerFound = 1; }
				if (take && gNumSyms < 256) {
					int dup = 0;
					for (int i = 0; i < gNumSyms; ++i) if (!strcmp(gSyms[i].name, name)) dup = 1;
					if (!dup) { gSyms[gNumSyms].addr = a; gSyms[gNumSyms].name = strdup(name); gNumSyms++; }
				}
			}
		}
		if (m) fclose(m);
		/* SoundMainRAM runs from its IWRAM copy (m4aSoundInit's CpuCopy32 of
		 * SoundMainRAM to SoundMainRAM_Buffer), so its labels break at the
		 * copy's address, not the ROM's. */
		{
			unsigned long romBase = 0, ramBase = 0;
			for (int i = 0; i < gNumSyms; ++i) {
				if (!strcmp(gSyms[i].name, "SoundMainRAM")) romBase = gSyms[i].addr & ~1ul;
				if (!strcmp(gSyms[i].name, "SoundMainRAM_Buffer")) ramBase = gSyms[i].addr;
			}
			for (int i = 0; romBase && ramBase && i < gNumSyms; ++i)
				if (!strcmp(gSyms[i].name, "_081DCFB0")) gSyms[i].addr = gSyms[i].addr - romBase + ramBase;
		}
		if (!playerFound && argc > 7) { fprintf(stderr, "player symbol %s not in %s\n", argv[7], argv[6]); return 1; }
	}
	mLogSetDefaultLogger(&sLogger);
	struct VFile* vf = VFileMemChunk(rom, n); free(rom);
	struct mCore* core = mCoreFindVF(vf);
	gCore = core;
	core->init(core);
	mCoreConfigInit(&core->config, NULL);
	mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "ignore");
	mCoreConfigSetDefaultIntValue(&core->config, "skipBios", 0);
	mCoreConfigSetDefaultIntValue(&core->config, "audioBuffers", 8192);
	mCoreConfigSetDefaultIntValue(&core->config, "sampleRate", 48000);
	mCoreLoadConfig(core);
	unsigned w, h; core->desiredVideoDimensions(core, &w, &h);
	color_t* video = calloc(w * h, sizeof(color_t)); core->setVideoBuffer(core, video, w);
	core->setAudioBufferSize(core, 8192);
	core->loadROM(core, vf);
	gGba = core->board;

	struct mDebugger dbg; memset(&dbg, 0, sizeof dbg);
	dbg.entered = entered; dbg.paused = paused; dbg.type = DEBUGGER_CUSTOM;
	mDebuggerAttach(&dbg, core);
	core->reset(core);
	installHooks();
	dbg.state = DEBUGGER_RUNNING;
	for (uint32_t a = 0x04000060; a < 0x040000A8; a += 1) {
		struct mWatchpoint wp = { .id = -1, .address = a, .segment = -1, .type = WATCHPOINT_WRITE, .condition = NULL };
		dbg.platform->setWatchpoint(dbg.platform, &wp);
	}
	uint32_t extra[] = { 0x04000004, 0x040000BC, 0x040000C0, 0x040000C4, 0x040000C6, 0x040000C8, 0x040000CC, 0x040000D0, 0x040000D2, 0x04000100, 0x04000102, 0x04000200, 0x04000208 };
	for (size_t i = 0; i < sizeof extra / sizeof *extra; ++i) {
		struct mWatchpoint wp = { .id = -1, .address = extra[i], .segment = -1, .type = WATCHPOINT_WRITE, .condition = NULL };
		dbg.platform->setWatchpoint(dbg.platform, &wp);
	}
	for (int i = 0; i < gNumSyms; ++i) {
		struct mBreakpoint bp = { .id = -1, .address = gSyms[i].addr & ~1u, .segment = -1, .type = BREAKPOINT_HARDWARE, .condition = NULL };
		dbg.platform->setBreakpoint(dbg.platform, &bp);
	}
	fprintf(gOut, "reset: t=%d vcount=%d video.event.when=%d\n", mTimingCurrentTime(&gGba->timing), gGba->video.vcount, gGba->video.event.when);
	for (long fr = 0; fr < gFrames; ++fr) {
		int32_t t0 = mTimingCurrentTime(&gGba->timing);
		mDebuggerRunFrame(&dbg);
		installHooks();
		fprintf(gOut, "---- runFrame %ld done: t=%d (frame start t=%d) vcount=%d\n", fr, mTimingCurrentTime(&gGba->timing), t0, gGba->video.vcount);
	}
	fclose(gOut);

	char path[1024];
	snprintf(path, sizeof path, "%s.frames", argv[2]);
	FILE* ff = fopen(path, "w");
	fprintf(ff, "# frame | m4aSoundMain SoundMain MPlayMain CgbSound SoundMainRAM (cycles from line 160 start) | vsync (from line 150) | mplay calls, ply_note, ply_all, tracks exist/start/count, tempoC (%s) | c15 before->after | writes\n", playerSym);
	for (int k = 1; k < MAX_FRAMES; ++k) {
		struct FrameRec* r = &gFrameRecs[k];
		if (!r->used) continue;
		fprintf(ff, "frame %4d | sm %6d SM %6d MP %6d CGB %6d RAM %6d | vs %5d | mp %d note %2d ply %3d trk %2d/%2d/%2d tempoC %3d | c15 %2d->%2d |",
		        k, r->m4aSoundMain, r->soundMain, r->mplayMain, r->cgbSound, r->soundMainRAM, r->vsync,
		        r->mplayMainCalls, r->plyNote, r->plyAll, r->tracksExist, r->tracksStart, r->trackCount, r->tempoC, r->c15Before, r->c15After);
		for (int i = 0; i < r->numWrites; ++i) {
			const char* nm = regName(r->writes[i].reg);
			if (nm) fprintf(ff, " %s=%02X@%d", nm, r->writes[i].value, r->writes[i].cycle);
			else fprintf(ff, " %02X=%02X@%d", r->writes[i].reg, r->writes[i].value, r->writes[i].cycle);
		}
		fputc('\n', ff);
		/* per-handler counts, when any */
		int any = 0;
		for (int i = 0; i < gNumSyms && i < 256; ++i) if (r->plyCount[i]) any = 1;
		if (any) {
			fprintf(ff, "           ply:");
			for (int i = 0; i < gNumSyms && i < 256; ++i) if (r->plyCount[i]) fprintf(ff, " %s=%d", gSyms[i].name, r->plyCount[i]);
			fputc('\n', ff);
		}
	}
	fclose(ff);

	snprintf(path, sizeof path, "%s.events", argv[2]);
	FILE* fe = fopen(path, "w");
	fprintf(fe, "# mgba scheduler lateness over %ld frames (slot = %d-cycle DAC slot)\n", gFrames, gGba->audio.sampleInterval);
	writeStats(fe, &gTimer0);
	writeStats(fe, &gSample);
	writeStats(fe, &gPsgFrame);
	fclose(fe);
	return 0;
}
