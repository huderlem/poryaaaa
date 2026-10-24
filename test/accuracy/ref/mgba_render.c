/*
 * mgba_render: headless reference renderer for the poryaaaa accuracy harness.
 *
 * Loads the accuracy test ROM into libmgba, patches its parameter block
 * (magic "PORYACC1") with what to play, runs N frames from reset and writes the
 * emulator's stereo output at the requested rate as a 16-bit WAV.
 *
 *   mgba_render <rom.gba> <out.wav> --frames N [--rate 48000]
 *               [--type song|cry|micro|none] [--id N] [--pitch N] [--pan N]
 *               [--volume N] [--start-frame N]
 */
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/version.h>
#include <mgba-util/vfs.h>

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
	int startFrame;
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

static void usage(const char* prog) {
	fprintf(stderr,
	        "Usage: %s <rom.gba> <out.wav> --frames N [--rate HZ] [--type song|cry|micro|none]\n"
	        "          [--id N] [--pitch N] [--pan N] [--volume N] [--start-frame N]\n",
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
	if (argc == 2 && !strcmp(argv[1], "--version")) {
		printf("%s %s\n", projectName, projectVersion);
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
	struct Params p = { TYPE_NONE, 0, 15360, 0, 120, 1 };

	int i;
	for (i = 3; i < argc; ++i) {
		const char* a = argv[i];
		const char* v = (i + 1 < argc) ? argv[i + 1] : NULL;
		if (!strcmp(a, "--frames") && v) {
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
		} else if (!strcmp(a, "--start-frame") && v) {
			p.startFrame = atoi(v); ++i;
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
		int avail = blip_samples_avail(left);
		if (avail > 8192) {
			avail = 8192;
		}
		if (avail > 0) {
			blip_read_samples(left, buf, avail, 1);
			blip_read_samples(right, buf + 1, avail, 1);
			fwrite(buf, sizeof(int16_t) * 2, avail, out);
			total += avail;
		}
	}
	fseek(out, 0, SEEK_SET);
	writeWavHeader(out, rate, total);
	fclose(out);

	free(buf);
	free(video);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	printf("%s: %ld frames, %u samples at %d Hz\n", outPath, frames, total, rate);
	return 0;
}
