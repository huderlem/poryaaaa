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
 *       build/accuracy/rom/accuracy_test.map
 *
 * type/id as in mgba_render (2 = micro, id = line in micro.cfg from 0).
 * Filter FIFO writes (040000A0/A4) out of the log to see the rest.
 */
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/debugger/debugger.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/video.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct GBA* gGba;
static FILE* gOut;
static long gFrames;
struct Sym { uint32_t addr; const char* name; };
static struct Sym gSyms[64]; static int gNumSyms;

static void quietLog(struct mLogger* l, int c, enum mLogLevel lv, const char* f, va_list a) { (void)l;(void)c;(void)lv;(void)f;(void)a; }
static struct mLogger sLogger = { .log = quietLog };

static const char* symName(uint32_t a) {
	for (int i = 0; i < gNumSyms; ++i) if ((gSyms[i].addr & ~1u) == (a & ~1u)) return gSyms[i].name;
	return "?";
}

static void entered(struct mDebugger* d, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
	int32_t t = mTimingCurrentTime(&gGba->timing);
	unsigned fc = gGba->video.frameCounter;
	int vc = gGba->video.vcount;
	if (reason == DEBUGGER_ENTER_WATCHPOINT) {
		fprintf(gOut, "%10d f%u v%3d W %08X <- %08X (old %08X)\n", t, fc, vc, info->address, info->type.wp.newValue, info->type.wp.oldValue);
	} else if (reason == DEBUGGER_ENTER_BREAKPOINT) {
		fprintf(gOut, "%10d f%u v%3d B %08X %s\n", t, fc, vc, info->address, symName(info->address));
	}
	d->state = DEBUGGER_RUNNING;
}
static void paused(struct mDebugger* d) { d->state = DEBUGGER_RUNNING; }

static int patchParams(uint8_t* rom, size_t size, int type, int id) {
	for (size_t off = 0; off + 32 <= size; off += 4) {
		if (memcmp(rom + off, "PORYACC1", 8) == 0) {
			uint8_t* b = rom + off + 8;
			b[0] = type; b[1] = 120; b[2] = id & 0xFF; b[3] = id >> 8; b[4] = 0; b[5] = 0x3C; b[6] = 0; b[7] = 1;
			return 1;
		}
	}
	return 0;
}

int main(int argc, char** argv) {
	if (argc < 6) { fprintf(stderr, "probe rom out.txt frames type id [map]\n"); return 1; }
	gFrames = atol(argv[3]);
	int type = atoi(argv[4]), id = atoi(argv[5]);
	FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t* rom = malloc(n); fread(rom, 1, n, f); fclose(f);
	patchParams(rom, n, type, id);
	gOut = fopen(argv[2], "w");
	if (argc > 6) {
		FILE* m = fopen(argv[6], "r"); char line[512];
		const char* want[] = { "VBlankIntr", "VCountIntr", "IntrMain", "m4aSoundMain", "m4aSoundVSync", "SoundMain", "MPlayMain", "CgbSound", "MPlayStart", "SampleFreqSet", "m4aSoundInit", "AgbMain", "SoundMainRAM_Buffer", "SoundMainBTM", "StartTest", NULL };
		while (m && fgets(line, sizeof line, m)) {
			unsigned long a; char name[128];
			if (sscanf(line, " 0x%lx %127s", &a, name) == 2) {
				for (int i = 0; want[i]; ++i) if (!strcmp(name, want[i]) && gNumSyms < 64) { gSyms[gNumSyms].addr = a; gSyms[gNumSyms].name = strdup(name); gNumSyms++; }
			}
		}
		if (m) fclose(m);
	}
	mLogSetDefaultLogger(&sLogger);
	struct VFile* vf = VFileMemChunk(rom, n); free(rom);
	struct mCore* core = mCoreFindVF(vf);
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
		fprintf(gOut, "---- runFrame %ld done: t=%d (frame start t=%d) vcount=%d\n", fr, mTimingCurrentTime(&gGba->timing), t0, gGba->video.vcount);
	}
	fclose(gOut);
	return 0;
}
