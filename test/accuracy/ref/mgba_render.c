/*
 * mgba_render: headless reference renderer for the poryaaaa accuracy harness.
 *
 * Loads the accuracy test ROM into libmgba, patches its parameter block
 * (magic "PORYACC1") with what to play, runs N frames from reset and writes the
 * emulator's stereo output at the requested rate as a 16-bit WAV.
 *
 * By default the raw DAC stream is captured through mAVStream.postAudioFrame
 * (one int16 pair per DAC slot: 65536 Hz once Emerald sets SOUNDBIAS
 * resolution 1, 256 CPU cycles per sample) and resampled with the engine's own
 * band-limited resampler, plugin/m4a_blip.c, exactly as plugin/m4a_gba.c does
 * (clock 16777216 Hz, level deltas stamped at a clock that advances one DAC
 * interval per sample, a frame every 2048 cycles).  Both sides of the null
 * test then share one bit-exact resampler and the harness measures only the
 * GBA-side model (docs/ACCURACY_PLAN.md, output-stage decision 2026-10-24).
 * --native-resampler writes mgba's own blip_buf output (the sinc kernel on
 * desktop builds) instead, for listening checks.  The two modes report
 * different --version strings so the harness caches their references apart.
 *
 *   mgba_render <rom.gba> <out.wav> --frames N [--rate 48000]
 *               [--type song|cry|micro|none] [--id N] [--pitch N] [--pan N]
 *               [--volume N] [--length N] [--release N] [--chorus N]
 *               [--start-frame N] [--id2 N --start-frame2 N] [--id3 N --start-frame3 N]
 *               [--native-resampler]
 *
 * --id2 / --start-frame2 start a second cry (gCryTable index N, PlayCryInternal's
 * CRY_MODE_NORMAL defaults) on VBlank N while the first test plays; start
 * frame 0 (the default) means no second cry.  --id3 / --start-frame3 start a
 * third the same way (AccParams block bytes 24..26); with both cry players
 * busy the ROM's SetPokemonCryTone replaces the player with the larger clock.
 */
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/version.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>

#include "m4a_blip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARAM_MAGIC "PORYACC1"
#define PARAM_SIZE 32

enum { TYPE_SONG = 0, TYPE_CRY = 1, TYPE_MICRO = 2, TYPE_NONE = 0xFF };

struct Params {
	int type;
	int id;
	int pitch;
	int pan;
	int volume;
	int length;   /* cry: SetPokemonCryLength's u16 (PlayCryInternal's 140) */
	int release;  /* cry: SetPokemonCryRelease's u8 (PlayCryInternal's 0) */
	int chorus;   /* cry: SetPokemonCryChorus's s8 (PlayCryInternal's 0; its u32 192 is the s8 -64) */
	int startFrame;
	int id2;          /* second cry: gCryTable index, CRY_MODE_NORMAL's defaults */
	int startFrame2;  /* its start frame; 0 = none */
	int id3;          /* third cry, the same way (block offset 24) */
	int startFrame3;  /* its start frame (block offset 26); 0 = none */
};

static void quietLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(format);
	UNUSED(args);
	if (level & (mLOG_FATAL | mLOG_ERROR)) {
		vfprintf(stderr, format, args);
		fputc('\n', stderr);
	}
}
static struct mLogger sLogger = { .log = quietLog };

/* Reference resampler: the engine's blip fed with the DAC stream. */
#define CAPTURE_BLIP_SIZE 0x8000
#define CAPTURE_CLOCK_RATE 16777216
#define CAPTURE_FRAME 2048
#define RESAMPLER_TAG "m4a_blip"

struct Capture {
	struct mAVStream stream;
	struct GBA* gba;
	M4ABlip* left;
	M4ABlip* right;
	int16_t lastLeft;
	int16_t lastRight;
	unsigned clock;
};

static void captureAudioFrame(struct mAVStream* stream, int16_t left, int16_t right) {
	struct Capture* c = (struct Capture*) stream;
	/* Mirror _sample in mgba's src/gba/audio.c: one level delta per DAC slot,
	 * the clock advancing by the current SOUNDBIAS interval (512 cycles before
	 * m4aSoundInit sets resolution 1, 256 after). */
	m4a_blip_add_delta(c->left, c->clock, left - c->lastLeft);
	m4a_blip_add_delta(c->right, c->clock, right - c->lastRight);
	c->lastLeft = left;
	c->lastRight = right;
	c->clock += c->gba->audio.sampleInterval;
	if (c->clock >= CAPTURE_FRAME) {
		m4a_blip_end_frame(c->left, CAPTURE_FRAME);
		m4a_blip_end_frame(c->right, CAPTURE_FRAME);
		c->clock -= CAPTURE_FRAME;
	}
}

static void usage(const char* prog) {
	fprintf(stderr,
	        "Usage: %s <rom.gba> <out.wav> --frames N [--rate HZ] [--type song|cry|micro|none]\n"
	        "          [--id N] [--pitch N] [--pan N] [--volume N] [--length N] [--release N]\n"
	        "          [--chorus N] [--start-frame N] [--id2 N --start-frame2 N]\n"
	        "          [--id3 N --start-frame3 N] [--native-resampler]\n",
	        prog);
}

static uint8_t* readFile(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* buf = malloc(n);
	if (!buf || fread(buf, 1, n, f) != (size_t) n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = n;
	return buf;
}

static int patchParams(uint8_t* rom, size_t size, const struct Params* p) {
	size_t off;
	for (off = 0; off + PARAM_SIZE <= size; off += 4) {
		if (memcmp(rom + off, PARAM_MAGIC, 8) == 0) {
			uint8_t* b = rom + off + 8;
			b[0] = (uint8_t) p->type;
			b[1] = (uint8_t) p->volume;
			b[2] = (uint8_t) (p->id & 0xFF);
			b[3] = (uint8_t) (p->id >> 8);
			b[4] = (uint8_t) (p->pitch & 0xFF);
			b[5] = (uint8_t) ((p->pitch >> 8) & 0xFF);
			b[6] = (uint8_t) (int8_t) p->pan;
			b[7] = (uint8_t) p->startFrame;
			b[8] = (uint8_t) (p->length & 0xFF);
			b[9] = (uint8_t) ((p->length >> 8) & 0xFF);
			b[10] = (uint8_t) p->release;
			b[11] = (uint8_t) (int8_t) p->chorus;
			b[12] = (uint8_t) (p->id2 & 0xFF);
			b[13] = (uint8_t) (p->id2 >> 8);
			b[14] = (uint8_t) p->startFrame2;
			b[16] = (uint8_t) (p->id3 & 0xFF);
			b[17] = (uint8_t) (p->id3 >> 8);
			b[18] = (uint8_t) p->startFrame3;
			return 1;
		}
	}
	return 0;
}

static void writeLE32(FILE* f, uint32_t v) {
	uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 };
	fwrite(b, 1, 4, f);
}
static void writeLE16(FILE* f, uint16_t v) {
	uint8_t b[2] = { v, v >> 8 };
	fwrite(b, 1, 2, f);
}

static void writeWavHeader(FILE* f, uint32_t rate, uint32_t frames) {
	uint32_t dataBytes = frames * 4;
	fwrite("RIFF", 1, 4, f);
	writeLE32(f, 36 + dataBytes);
	fwrite("WAVEfmt ", 1, 8, f);
	writeLE32(f, 16);
	writeLE16(f, 1);          /* PCM */
	writeLE16(f, 2);          /* stereo */
	writeLE32(f, rate);
	writeLE32(f, rate * 4);
	writeLE16(f, 4);
	writeLE16(f, 16);
	fwrite("data", 1, 4, f);
	writeLE32(f, dataBytes);
}

int main(int argc, char** argv) {
	int nativeResampler = 0;
	int i;
	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--native-resampler")) {
			nativeResampler = 1;
		}
	}
	if (argc >= 2 && !strcmp(argv[1], "--version")) {
		/* The resampler is part of the reference definition: the harness keys
		 * its reference cache on this string. */
		if (nativeResampler) {
			printf("%s %s\n", projectName, projectVersion);
		} else {
			printf("%s %s+%s\n", projectName, projectVersion, RESAMPLER_TAG);
		}
		return 0;
	}
	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}
	const char* romPath = argv[1];
	const char* outPath = argv[2];
	long frames = -1;
	int rate = 48000;
	struct Params p = { TYPE_NONE, 0, 15360, 0, 120, 140, 0, 0, 1, 0, 0 };

	for (i = 3; i < argc; ++i) {
		const char* a = argv[i];
		const char* v = (i + 1 < argc) ? argv[i + 1] : NULL;
		if (!strcmp(a, "--native-resampler")) {
			continue;
		} else if (!strcmp(a, "--frames") && v) {
			frames = atol(v); ++i;
		} else if (!strcmp(a, "--rate") && v) {
			rate = atoi(v); ++i;
		} else if (!strcmp(a, "--type") && v) {
			if (!strcmp(v, "song")) p.type = TYPE_SONG;
			else if (!strcmp(v, "cry")) p.type = TYPE_CRY;
			else if (!strcmp(v, "micro")) p.type = TYPE_MICRO;
			else if (!strcmp(v, "none")) p.type = TYPE_NONE;
			else { usage(argv[0]); return 1; }
			++i;
		} else if (!strcmp(a, "--id") && v) {
			p.id = atoi(v); ++i;
		} else if (!strcmp(a, "--pitch") && v) {
			p.pitch = atoi(v); ++i;
		} else if (!strcmp(a, "--pan") && v) {
			p.pan = atoi(v); ++i;
		} else if (!strcmp(a, "--volume") && v) {
			p.volume = atoi(v); ++i;
		} else if (!strcmp(a, "--length") && v) {
			p.length = atoi(v); ++i;
		} else if (!strcmp(a, "--release") && v) {
			p.release = atoi(v); ++i;
		} else if (!strcmp(a, "--chorus") && v) {
			p.chorus = atoi(v); ++i;
		} else if (!strcmp(a, "--start-frame") && v) {
			p.startFrame = atoi(v); ++i;
		} else if (!strcmp(a, "--id2") && v) {
			p.id2 = atoi(v); ++i;
		} else if (!strcmp(a, "--start-frame2") && v) {
			p.startFrame2 = atoi(v); ++i;
		} else if (!strcmp(a, "--id3") && v) {
			p.id3 = atoi(v); ++i;
		} else if (!strcmp(a, "--start-frame3") && v) {
			p.startFrame3 = atoi(v); ++i;
		} else {
			usage(argv[0]);
			return 1;
		}
	}
	if (frames < 0) {
		fprintf(stderr, "--frames is required\n");
		return 1;
	}

	size_t romSize;
	uint8_t* rom = readFile(romPath, &romSize);
	if (!rom) {
		fprintf(stderr, "Cannot read ROM %s\n", romPath);
		return 1;
	}
	if (!patchParams(rom, romSize, &p)) {
		fprintf(stderr, "Parameter block \"%s\" not found in ROM\n", PARAM_MAGIC);
		return 1;
	}

	mLogSetDefaultLogger(&sLogger);
	struct VFile* vf = VFileMemChunk(rom, romSize);
	free(rom);
	struct mCore* core = mCoreFindVF(vf);
	if (!core) {
		fprintf(stderr, "libmgba does not recognise %s as a ROM\n", romPath);
		return 1;
	}
	core->init(core);
	mCoreConfigInit(&core->config, NULL);
	/* No user config: keep the run deterministic and independent of ~/.config. */
	mCoreConfigSetDefaultValue(&core->config, "idleOptimization", "ignore");
	mCoreConfigSetDefaultIntValue(&core->config, "skipBios", 0);
	mCoreConfigSetDefaultIntValue(&core->config, "audioBuffers", 8192);
	mCoreConfigSetDefaultIntValue(&core->config, "sampleRate", rate);
	mCoreConfigSetDefaultIntValue(&core->config, "volume", 0x100); /* GBA_AUDIO_VOLUME_MAX: unity */
	mCoreConfigSetDefaultIntValue(&core->config, "mute", 0);
	mCoreLoadConfig(core);

	unsigned w, h;
	core->desiredVideoDimensions(core, &w, &h);
	color_t* video = calloc(w * h, sizeof(color_t));
	core->setVideoBuffer(core, video, w);
	core->setAudioBufferSize(core, 8192);

	if (!core->loadROM(core, vf)) {
		fprintf(stderr, "loadROM failed\n");
		return 1;
	}
	struct Capture cap;
	memset(&cap, 0, sizeof(cap));
	if (!nativeResampler) {
		cap.stream.postAudioFrame = captureAudioFrame;
		cap.gba = core->board;
		cap.left = m4a_blip_new(CAPTURE_BLIP_SIZE);
		cap.right = m4a_blip_new(CAPTURE_BLIP_SIZE);
		if (!cap.left || !cap.right) {
			fprintf(stderr, "Out of memory\n");
			return 1;
		}
		m4a_blip_set_rates(cap.left, CAPTURE_CLOCK_RATE, rate);
		m4a_blip_set_rates(cap.right, CAPTURE_CLOCK_RATE, rate);
		core->setAVStream(core, &cap.stream);
	}
	core->reset(core);

	struct blip_t* left = core->getAudioChannel(core, 0);
	struct blip_t* right = core->getAudioChannel(core, 1);
	blip_set_rates(left, core->frequency(core), rate);
	blip_set_rates(right, core->frequency(core), rate);

	FILE* out = fopen(outPath, "wb");
	if (!out) {
		fprintf(stderr, "Cannot open %s\n", outPath);
		return 1;
	}
	writeWavHeader(out, rate, 0);

	int16_t* buf = malloc(8192 * 2 * sizeof(int16_t));
	uint32_t total = 0;
	long f;
	for (f = 0; f < frames; ++f) {
		core->runFrame(core);
		int avail;
		if (nativeResampler) {
			avail = blip_samples_avail(left);
			if (avail > 8192) {
				avail = 8192;
			}
			if (avail > 0) {
				blip_read_samples(left, buf, avail, 1);
				blip_read_samples(right, buf + 1, avail, 1);
			}
		} else {
			/* mgba's own blip is drained and discarded so it never fills. */
			int drain = blip_samples_avail(left);
			if (drain > 8192) {
				drain = 8192;
			}
			if (drain > 0) {
				blip_read_samples(left, buf, drain, 1);
				blip_read_samples(right, buf + 1, drain, 1);
			}
			avail = m4a_blip_samples_avail(cap.left);
			if (avail > 8192) {
				avail = 8192;
			}
			if (avail > 0) {
				m4a_blip_read_samples(cap.left, buf, avail, 1);
				m4a_blip_read_samples(cap.right, buf + 1, avail, 1);
			}
		}
		if (avail > 0) {
			fwrite(buf, sizeof(int16_t) * 2, avail, out);
			total += avail;
		}
	}
	fseek(out, 0, SEEK_SET);
	writeWavHeader(out, rate, total);
	fclose(out);

	m4a_blip_delete(cap.left);
	m4a_blip_delete(cap.right);
	free(buf);
	free(video);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	printf("%s: %ld frames, %u samples at %d Hz\n", outPath, frames, total, rate);
	return 0;
}
