#include "voicegroup_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

#define MAX_LINE 1024
#define MAX_PATH_LEN 512
#define MAX_SYMBOL_LEN 256
#define INITIAL_CAPACITY 64

/* Symbol -> file path mapping */
typedef struct {
    char symbol[MAX_SYMBOL_LEN];
    char filePath[MAX_PATH_LEN];
} SymbolMapping;

typedef struct {
    SymbolMapping *entries;
    int count;
    int capacity;
} SymbolMap;

/* Keysplit table definition */
typedef struct {
    char name[MAX_SYMBOL_LEN];
    int startingNote;
    uint8_t table[128];
    int maxNote;
} KeySplitDef;

typedef struct {
    KeySplitDef *entries;
    int count;
    int capacity;
} KeySplitMap;

/* Forward declarations */
static void symbol_map_init(SymbolMap *map);
static void symbol_map_free(SymbolMap *map);
static void symbol_map_add(SymbolMap *map, const char *symbol, const char *path);
static const char *symbol_map_find(const SymbolMap *map, const char *symbol);

static void keysplit_map_init(KeySplitMap *map);
static void keysplit_map_free(KeySplitMap *map);
static KeySplitDef *keysplit_map_find(const KeySplitMap *map, const char *name);

static int parse_direct_sound_data(const char *projectRoot, SymbolMap *map);
static int parse_programmable_wave_data(const char *projectRoot, SymbolMap *map);
static int parse_keysplit_tables(const char *projectRoot, KeySplitMap *map);
static WaveData *load_wave_data(const char *projectRoot, const char *relativePath);
static uint32_t *load_prog_wave(const char *projectRoot, const char *relativePath);
static int parse_voicegroup_file(const char *projectRoot, const char *filePath,
                                  LoadedVoiceGroup *vg,
                                  const SymbolMap *dsMap, const SymbolMap *pwMap,
                                  const KeySplitMap *ksMap);

/* Helper: trim leading whitespace */
static char *ltrim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Helper: strip trailing whitespace/newline */
static void rtrim(char *s)
{
    int len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

/* Helper: strip inline comments (@ or //) */
static void strip_comment(char *s)
{
    char *p = strchr(s, '@');
    if (p) *p = '\0';
    p = strstr(s, "//");
    if (p) *p = '\0';
}

/* Helper: build a path */
static void build_path(char *dest, size_t destSize, const char *base, const char *relative)
{
    snprintf(dest, destSize, "%s%c%s", base, PATH_SEP, relative);
    /* Normalize separators */
    for (char *p = dest; *p; p++) {
        if (*p == '/' || *p == '\\')
            *p = PATH_SEP;
    }
}

/* Helper: register a WaveData in the loaded voicegroup for later cleanup */
static void vg_register_wavedata(LoadedVoiceGroup *vg, WaveData *wd)
{
    if (vg->waveDataCount >= vg->waveDataCapacity) {
        vg->waveDataCapacity = vg->waveDataCapacity ? vg->waveDataCapacity * 2 : INITIAL_CAPACITY;
        vg->waveDatas = realloc(vg->waveDatas, sizeof(WaveData *) * vg->waveDataCapacity);
    }
    vg->waveDatas[vg->waveDataCount++] = wd;
}

static void vg_register_progwave(LoadedVoiceGroup *vg, uint32_t *pw)
{
    if (vg->progWaveCount >= vg->progWaveCapacity) {
        vg->progWaveCapacity = vg->progWaveCapacity ? vg->progWaveCapacity * 2 : INITIAL_CAPACITY;
        vg->progWaves = realloc(vg->progWaves, sizeof(uint32_t *) * vg->progWaveCapacity);
    }
    vg->progWaves[vg->progWaveCount++] = pw;
}

static void vg_register_subgroup(LoadedVoiceGroup *vg, ToneData *sg)
{
    if (vg->subGroupCount >= vg->subGroupCapacity) {
        vg->subGroupCapacity = vg->subGroupCapacity ? vg->subGroupCapacity * 2 : INITIAL_CAPACITY;
        vg->subGroups = realloc(vg->subGroups, sizeof(ToneData *) * vg->subGroupCapacity);
    }
    vg->subGroups[vg->subGroupCount++] = sg;
}

static void vg_register_keysplittable(LoadedVoiceGroup *vg, uint8_t *ks)
{
    if (vg->keySplitTableCount >= vg->keySplitTableCapacity) {
        vg->keySplitTableCapacity = vg->keySplitTableCapacity ? vg->keySplitTableCapacity * 2 : INITIAL_CAPACITY;
        vg->keySplitTables = realloc(vg->keySplitTables, sizeof(uint8_t *) * vg->keySplitTableCapacity);
    }
    vg->keySplitTables[vg->keySplitTableCount++] = ks;
}

/*
 * Symbol map implementation
 */
static void symbol_map_init(SymbolMap *map)
{
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

static void symbol_map_free(SymbolMap *map)
{
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

static void symbol_map_add(SymbolMap *map, const char *symbol, const char *path)
{
    if (map->count >= map->capacity) {
        map->capacity = map->capacity ? map->capacity * 2 : INITIAL_CAPACITY;
        map->entries = realloc(map->entries, sizeof(SymbolMapping) * map->capacity);
    }
    strncpy(map->entries[map->count].symbol, symbol, MAX_SYMBOL_LEN - 1);
    map->entries[map->count].symbol[MAX_SYMBOL_LEN - 1] = '\0';
    strncpy(map->entries[map->count].filePath, path, MAX_PATH_LEN - 1);
    map->entries[map->count].filePath[MAX_PATH_LEN - 1] = '\0';
    map->count++;
}

static const char *symbol_map_find(const SymbolMap *map, const char *symbol)
{
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].symbol, symbol) == 0)
            return map->entries[i].filePath;
    }
    return NULL;
}

/*
 * Keysplit map implementation
 */
static void keysplit_map_init(KeySplitMap *map)
{
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

static void keysplit_map_free(KeySplitMap *map)
{
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

static KeySplitDef *keysplit_map_find(const KeySplitMap *map, const char *name)
{
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].name, name) == 0)
            return &map->entries[i];
    }
    return NULL;
}

/*
 * Parse sound/direct_sound_data.inc
 * Builds symbol name -> file path mapping.
 * Format:
 *   DirectSoundWaveData_xxx::
 *     .incbin "sound/direct_sound_samples/xxx.bin"
 */
static int parse_direct_sound_data(const char *projectRoot, SymbolMap *map)
{
    char path[MAX_PATH_LEN];
    build_path(path, sizeof(path), projectRoot, "sound/direct_sound_data.inc");

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", path);
        return -1;
    }

    char line[MAX_LINE];
    char currentSymbol[MAX_SYMBOL_LEN] = {0};

    while (fgets(line, sizeof(line), f)) {
        strip_comment(line);
        rtrim(line);
        char *trimmed = ltrim(line);

        /* Look for label:: lines */
        char *colonColon = strstr(trimmed, "::");
        if (colonColon && colonColon > trimmed) {
            *colonColon = '\0';
            strncpy(currentSymbol, trimmed, MAX_SYMBOL_LEN - 1);
            currentSymbol[MAX_SYMBOL_LEN - 1] = '\0';
            continue;
        }

        /* Look for .incbin lines */
        if (currentSymbol[0] && strstr(trimmed, ".incbin")) {
            char *quote1 = strchr(trimmed, '"');
            if (quote1) {
                quote1++;
                char *quote2 = strchr(quote1, '"');
                if (quote2) {
                    *quote2 = '\0';
                    symbol_map_add(map, currentSymbol, quote1);
                }
            }
            currentSymbol[0] = '\0';
        }
    }

    fclose(f);
    return 0;
}

/*
 * Parse sound/programmable_wave_data.inc
 * Format:
 *   ProgrammableWaveData_N::
 *     .incbin "sound/programmable_wave_samples/NN.pcm"
 */
static int parse_programmable_wave_data(const char *projectRoot, SymbolMap *map)
{
    char path[MAX_PATH_LEN];
    build_path(path, sizeof(path), projectRoot, "sound/programmable_wave_data.inc");

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", path);
        return -1;
    }

    char line[MAX_LINE];
    char currentSymbol[MAX_SYMBOL_LEN] = {0};

    while (fgets(line, sizeof(line), f)) {
        strip_comment(line);
        rtrim(line);
        char *trimmed = ltrim(line);

        char *colonColon = strstr(trimmed, "::");
        if (colonColon && colonColon > trimmed) {
            *colonColon = '\0';
            strncpy(currentSymbol, trimmed, MAX_SYMBOL_LEN - 1);
            currentSymbol[MAX_SYMBOL_LEN - 1] = '\0';
            continue;
        }

        if (currentSymbol[0] && strstr(trimmed, ".incbin")) {
            char *quote1 = strchr(trimmed, '"');
            if (quote1) {
                quote1++;
                char *quote2 = strchr(quote1, '"');
                if (quote2) {
                    *quote2 = '\0';
                    symbol_map_add(map, currentSymbol, quote1);
                }
            }
            currentSymbol[0] = '\0';
        }
    }

    fclose(f);
    return 0;
}

/*
 * Parse sound/keysplit_tables.inc
 * Format:
 *   keysplit piano, 36
 *     split 0, 55
 *     split 1, 70
 *     ...
 */
static int parse_keysplit_tables(const char *projectRoot, KeySplitMap *map)
{
    char path[MAX_PATH_LEN];
    build_path(path, sizeof(path), projectRoot, "sound/keysplit_tables.inc");

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", path);
        return -1;
    }

    char line[MAX_LINE];
    KeySplitDef *current = NULL;
    int lastNote = 0;

    while (fgets(line, sizeof(line), f)) {
        strip_comment(line);
        rtrim(line);
        char *trimmed = ltrim(line);

        if (strncmp(trimmed, "keysplit ", 9) == 0) {
            /* Parse: keysplit <name>, <starting_note> */
            char name[MAX_SYMBOL_LEN];
            int startNote = 0;
            if (sscanf(trimmed + 9, "%[^,], %d", name, &startNote) >= 1) {
                rtrim(name);
                if (map->count >= map->capacity) {
                    map->capacity = map->capacity ? map->capacity * 2 : INITIAL_CAPACITY;
                    map->entries = realloc(map->entries, sizeof(KeySplitDef) * map->capacity);
                }
                current = &map->entries[map->count];
                memset(current, 0, sizeof(KeySplitDef));
                snprintf(current->name, MAX_SYMBOL_LEN, "keysplit_%s", name);
                current->startingNote = startNote;
                current->maxNote = 0;
                lastNote = startNote;
                map->count++;
            }
        } else if (strncmp(trimmed, "split ", 6) == 0 && current) {
            /* Parse: split <index>, <ending_note> */
            int index, endNote;
            if (sscanf(trimmed + 6, "%d, %d", &index, &endNote) == 2) {
                /* Fill table from lastNote to endNote with index */
                for (int n = lastNote; n < endNote && n < 128; n++) {
                    current->table[n] = (uint8_t)index;
                }
                lastNote = endNote;
                if (endNote > current->maxNote)
                    current->maxNote = endNote;
            }
        }
    }

    fclose(f);
    return 0;
}

/*
 * Load a .bin sample file (DirectSound wave data).
 * Binary format: 16-byte header + signed 8-bit PCM samples.
 * Header: u16 type, u16 status, u32 freq, u32 loopStart, u32 size
 */
static WaveData *load_wave_data(const char *projectRoot, const char *relativePath)
{
    char fullPath[MAX_PATH_LEN];
    build_path(fullPath, sizeof(fullPath), projectRoot, relativePath);

    FILE *f = fopen(fullPath, "rb");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open sample %s\n", fullPath);
        return NULL;
    }

    /* Read header */
    uint8_t header[16];
    if (fread(header, 1, 16, f) != 16) {
        fprintf(stderr, "voicegroup_loader: short read on header %s\n", fullPath);
        fclose(f);
        return NULL;
    }

    uint16_t type = header[0] | (header[1] << 8);
    /* uint16_t status = header[2] | (header[3] << 8); */
    uint32_t freq = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);
    uint32_t loopStart = header[8] | (header[9] << 8) | (header[10] << 16) | (header[11] << 24);
    uint32_t size = header[12] | (header[13] << 8) | (header[14] << 16) | (header[15] << 24);

    /* Allocate WaveData + sample data */
    WaveData *wd = malloc(sizeof(WaveData) + size + 1); /* +1 for interpolation safety */
    if (!wd) {
        fclose(f);
        return NULL;
    }

    uint16_t status = header[2] | (header[3] << 8);

    wd->type = type;
    wd->status = status;
    wd->freq = freq;
    wd->loopStart = loopStart;
    wd->size = size;
    wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));

    /* Read sample data */
    size_t bytesRead = fread(wd->data, 1, size, f);
    if (bytesRead < size) {
        /* Pad with zeros if file is shorter than declared */
        memset(wd->data + bytesRead, 0, size - bytesRead);
    }
    /* Safety byte for interpolation at end of sample */
    wd->data[size] = wd->data[size > 0 ? size - 1 : 0];

    fclose(f);
    return wd;
}

/*
 * Load a .pcm programmable wave file (16 bytes = 32 4-bit samples).
 */
static uint32_t *load_prog_wave(const char *projectRoot, const char *relativePath)
{
    char fullPath[MAX_PATH_LEN];
    build_path(fullPath, sizeof(fullPath), projectRoot, relativePath);

    FILE *f = fopen(fullPath, "rb");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open wave %s\n", fullPath);
        return NULL;
    }

    /* 16 bytes = 4 uint32_t */
    uint32_t *data = malloc(16);
    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, 16, f) != 16) {
        fprintf(stderr, "voicegroup_loader: short read on wave %s\n", fullPath);
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return data;
}

/*
 * Parse a voicegroup .inc file and populate the ToneData array.
 * This handles the voice macros defined in asm/macros/music_voice.inc.
 */

/* Helper: try to open a file path, return 1 if it exists, 0 otherwise */
static int file_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}

/*
 * Helper to search for a voicegroup .inc file by trying common locations.
 * Handles naming conventions like:
 *   - "petalburg" -> sound/voicegroups/petalburg.inc
 *   - "petalburg_drumset" -> sound/voicegroups/drumsets/petalburg.inc
 *   - "piano_keysplit" -> sound/voicegroups/keysplits/piano.inc
 */
static int find_voicegroup_file(const char *projectRoot, const char *vgName,
                                 char *outPath, size_t outPathSize)
{
    /* 1. Try exact name: sound/voicegroups/<name>.inc */
    snprintf(outPath, outPathSize, "%s/sound/voicegroups/%s.inc", projectRoot, vgName);
    if (file_exists(outPath)) return 1;

    /* 2. Try exact name in keysplits/ and drumsets/ */
    snprintf(outPath, outPathSize, "%s/sound/voicegroups/keysplits/%s.inc", projectRoot, vgName);
    if (file_exists(outPath)) return 1;
    snprintf(outPath, outPathSize, "%s/sound/voicegroups/drumsets/%s.inc", projectRoot, vgName);
    if (file_exists(outPath)) return 1;

    /* 3. Handle suffix convention: strip _keysplit suffix, look in keysplits/ */
    {
        const char *suffix = strstr(vgName, "_keysplit");
        if (suffix) {
            char baseName[MAX_SYMBOL_LEN];
            int baseLen = (int)(suffix - vgName);
            if (baseLen > 0 && baseLen < MAX_SYMBOL_LEN) {
                memcpy(baseName, vgName, baseLen);
                baseName[baseLen] = '\0';
                snprintf(outPath, outPathSize, "%s/sound/voicegroups/keysplits/%s.inc",
                         projectRoot, baseName);
                if (file_exists(outPath)) return 1;
            }
        }
    }

    /* 4. Handle suffix convention: strip _drumset suffix, look in drumsets/ */
    {
        const char *suffix = strstr(vgName, "_drumset");
        if (suffix) {
            char baseName[MAX_SYMBOL_LEN];
            int baseLen = (int)(suffix - vgName);
            if (baseLen > 0 && baseLen < MAX_SYMBOL_LEN) {
                memcpy(baseName, vgName, baseLen);
                baseName[baseLen] = '\0';
                snprintf(outPath, outPathSize, "%s/sound/voicegroups/drumsets/%s.inc",
                         projectRoot, baseName);
                if (file_exists(outPath)) return 1;
            }
        }
    }

    return 0;
}

/*
 * Load a sub-voicegroup (for keysplit/keysplit_all references).
 * Returns an array of VOICEGROUP_SIZE ToneData entries.
 */
static ToneData *load_sub_voicegroup(const char *projectRoot, const char *vgSymbol,
                                      LoadedVoiceGroup *vg,
                                      const SymbolMap *dsMap, const SymbolMap *pwMap,
                                      const KeySplitMap *ksMap)
{
    /* Symbol format: voicegroup_<name>
     * Strip the "voicegroup_" prefix to get the .inc file name */
    const char *name = vgSymbol;
    if (strncmp(name, "voicegroup_", 11) == 0)
        name += 11;

    char filePath[MAX_PATH_LEN];
    if (!find_voicegroup_file(projectRoot, name, filePath, sizeof(filePath))) {
        fprintf(stderr, "voicegroup_loader: cannot find sub-voicegroup '%s'\n", vgSymbol);
        return NULL;
    }

    /* Allocate sub-voicegroup */
    ToneData *subVg = calloc(VOICEGROUP_SIZE, sizeof(ToneData));
    if (!subVg) return NULL;

    /* Temporarily swap out the main voices array to parse into subVg */
    ToneData savedVoices[VOICEGROUP_SIZE];
    memcpy(savedVoices, vg->voices, sizeof(savedVoices));
    memset(vg->voices, 0, sizeof(vg->voices));

    if (parse_voicegroup_file(projectRoot, filePath, vg, dsMap, pwMap, ksMap) != 0) {
        free(subVg);
        memcpy(vg->voices, savedVoices, sizeof(savedVoices));
        return NULL;
    }

    memcpy(subVg, vg->voices, sizeof(ToneData) * VOICEGROUP_SIZE);
    memcpy(vg->voices, savedVoices, sizeof(savedVoices));

    vg_register_subgroup(vg, subVg);
    return subVg;
}

static int parse_voicegroup_file(const char *projectRoot, const char *filePath,
                                  LoadedVoiceGroup *vg,
                                  const SymbolMap *dsMap, const SymbolMap *pwMap,
                                  const KeySplitMap *ksMap)
{
    FILE *f = fopen(filePath, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", filePath);
        return -1;
    }

    char line[MAX_LINE];
    int voiceIndex = 0;

    while (fgets(line, sizeof(line), f) && voiceIndex < VOICEGROUP_SIZE) {
        strip_comment(line);
        rtrim(line);
        char *trimmed = ltrim(line);

        if (trimmed[0] == '\0')
            continue;

        /* Parse voice_group declaration for starting_note offset */
        if (strncmp(trimmed, "voice_group ", 12) == 0) {
            /* Format: voice_group <name>[, <starting_note>] */
            char vgDeclName[MAX_SYMBOL_LEN];
            int startingNote = 0;
            if (sscanf(trimmed + 12, "%[^,\n], %d", vgDeclName, &startingNote) >= 2) {
                if (startingNote > 0 && startingNote < VOICEGROUP_SIZE)
                    voiceIndex = startingNote;
            }
            continue;
        }

        /* voice_directsound <key>, <pan>, <sample>, <A>, <D>, <S>, <R> */
        if (strncmp(trimmed, "voice_directsound_no_resample ", 30) == 0) {
            int key, pan, attack, decay, sustain, release;
            char sampleSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 30, "%d, %d, %[^,], %d, %d, %d, %d",
                       &key, &pan, sampleSymbol, &attack, &decay, &sustain, &release) == 7) {
                rtrim(sampleSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_DIRECTSOUND_NO_RESAMPLE;
                td->key = (uint8_t)key;
                td->panSweep = pan ? (0x80 | pan) : 0;
                td->attack = (uint8_t)attack;
                td->decay = (uint8_t)decay;
                td->sustain = (uint8_t)sustain;
                td->release = (uint8_t)release;

                const char *samplePath = symbol_map_find(dsMap, sampleSymbol);
                if (samplePath) {
                    WaveData *wd = load_wave_data(projectRoot, samplePath);
                    if (wd) {
                        td->wav = wd;
                        vg_register_wavedata(vg, wd);
                    }
                }
            }
            voiceIndex++;
        } else if (strncmp(trimmed, "voice_directsound_alt ", 22) == 0) {
            int key, pan, attack, decay, sustain, release;
            char sampleSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 22, "%d, %d, %[^,], %d, %d, %d, %d",
                       &key, &pan, sampleSymbol, &attack, &decay, &sustain, &release) == 7) {
                rtrim(sampleSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_DIRECTSOUND_ALT;
                td->key = (uint8_t)key;
                td->panSweep = pan ? (0x80 | pan) : 0;
                td->attack = (uint8_t)attack;
                td->decay = (uint8_t)decay;
                td->sustain = (uint8_t)sustain;
                td->release = (uint8_t)release;

                const char *samplePath = symbol_map_find(dsMap, sampleSymbol);
                if (samplePath) {
                    WaveData *wd = load_wave_data(projectRoot, samplePath);
                    if (wd) {
                        td->wav = wd;
                        vg_register_wavedata(vg, wd);
                    }
                }
            }
            voiceIndex++;
        } else if (strncmp(trimmed, "voice_directsound ", 18) == 0) {
            int key, pan, attack, decay, sustain, release;
            char sampleSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 18, "%d, %d, %[^,], %d, %d, %d, %d",
                       &key, &pan, sampleSymbol, &attack, &decay, &sustain, &release) == 7) {
                rtrim(sampleSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_DIRECTSOUND;
                td->key = (uint8_t)key;
                td->panSweep = pan ? (0x80 | pan) : 0;
                td->attack = (uint8_t)attack;
                td->decay = (uint8_t)decay;
                td->sustain = (uint8_t)sustain;
                td->release = (uint8_t)release;

                const char *samplePath = symbol_map_find(dsMap, sampleSymbol);
                if (samplePath) {
                    WaveData *wd = load_wave_data(projectRoot, samplePath);
                    if (wd) {
                        td->wav = wd;
                        vg_register_wavedata(vg, wd);
                    }
                }
            }
            voiceIndex++;
        }
        /* voice_square_1 <key>, <pan>, <sweep>, <duty>, <A>, <D>, <S>, <R> */
        else if (strncmp(trimmed, "voice_square_1_alt ", 19) == 0) {
            int key, pan, sweep, duty, attack, decay, sustain, release;
            if (sscanf(trimmed + 19, "%d, %d, %d, %d, %d, %d, %d, %d",
                       &key, &pan, &sweep, &duty, &attack, &decay, &sustain, &release) == 8) {
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_SQUARE_1_ALT;
                td->key = (uint8_t)key;
                td->panSweep = (uint8_t)sweep;
                td->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
                td->attack = (uint8_t)(attack & 0x07);
                td->decay = (uint8_t)(decay & 0x07);
                td->sustain = (uint8_t)(sustain & 0x0F);
                td->release = (uint8_t)(release & 0x07);
            }
            voiceIndex++;
        } else if (strncmp(trimmed, "voice_square_1 ", 15) == 0) {
            int key, pan, sweep, duty, attack, decay, sustain, release;
            if (sscanf(trimmed + 15, "%d, %d, %d, %d, %d, %d, %d, %d",
                       &key, &pan, &sweep, &duty, &attack, &decay, &sustain, &release) == 8) {
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_SQUARE_1;
                td->key = (uint8_t)key;
                td->panSweep = (uint8_t)sweep;
                td->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
                td->attack = (uint8_t)(attack & 0x07);
                td->decay = (uint8_t)(decay & 0x07);
                td->sustain = (uint8_t)(sustain & 0x0F);
                td->release = (uint8_t)(release & 0x07);
            }
            voiceIndex++;
        }
        /* voice_square_2 <key>, <pan>, <duty>, <A>, <D>, <S>, <R> */
        else if (strncmp(trimmed, "voice_square_2_alt ", 19) == 0) {
            int key, pan, duty, attack, decay, sustain, release;
            if (sscanf(trimmed + 19, "%d, %d, %d, %d, %d, %d, %d",
                       &key, &pan, &duty, &attack, &decay, &sustain, &release) == 7) {
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_SQUARE_2_ALT;
                td->key = (uint8_t)key;
                td->panSweep = 0;
                td->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
                td->attack = (uint8_t)(attack & 0x07);
                td->decay = (uint8_t)(decay & 0x07);
                td->sustain = (uint8_t)(sustain & 0x0F);
                td->release = (uint8_t)(release & 0x07);
            }
            voiceIndex++;
        } else if (strncmp(trimmed, "voice_square_2 ", 15) == 0) {
            int key, pan, duty, attack, decay, sustain, release;
            if (sscanf(trimmed + 15, "%d, %d, %d, %d, %d, %d, %d",
                       &key, &pan, &duty, &attack, &decay, &sustain, &release) == 7) {
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_SQUARE_2;
                td->key = (uint8_t)key;
                td->panSweep = 0;
                td->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
                td->attack = (uint8_t)(attack & 0x07);
                td->decay = (uint8_t)(decay & 0x07);
                td->sustain = (uint8_t)(sustain & 0x0F);
                td->release = (uint8_t)(release & 0x07);
            }
            voiceIndex++;
        }
        /* voice_programmable_wave <key>, <pan>, <wave_sym>, <A>, <D>, <S>, <R> */
        else if (strncmp(trimmed, "voice_programmable_wave_alt ", 27) == 0) {
            int key, pan, attack, decay, sustain, release;
            char waveSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 27, "%d, %d, %[^,], %d, %d, %d, %d",
                       &key, &pan, waveSymbol, &attack, &decay, &sustain, &release) == 7) {
                rtrim(waveSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_PROGRAMMABLE_WAVE_ALT;
                td->key = (uint8_t)key;
                td->attack = (uint8_t)(attack & 0x07);
                td->decay = (uint8_t)(decay & 0x07);
                td->sustain = (uint8_t)(sustain & 0x0F);
                td->release = (uint8_t)(release & 0x07);

                const char *wavePath = symbol_map_find(pwMap, waveSymbol);
                if (wavePath) {
                    uint32_t *pw = load_prog_wave(projectRoot, wavePath);
                    if (pw) {
                        td->wavePointer = pw;
                        vg_register_progwave(vg, pw);
                    }
                }
            }
            voiceIndex++;
        } else if (strncmp(trimmed, "voice_programmable_wave ", 23) == 0) {
            int key, pan, attack, decay, sustain, release;
            char waveSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 23, "%d, %d, %[^,], %d, %d, %d, %d",
                       &key, &pan, waveSymbol, &attack, &decay, &sustain, &release) == 7) {
                rtrim(waveSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_PROGRAMMABLE_WAVE;
                td->key = (uint8_t)key;
                td->attack = (uint8_t)(attack & 0x07);
                td->decay = (uint8_t)(decay & 0x07);
                td->sustain = (uint8_t)(sustain & 0x0F);
                td->release = (uint8_t)(release & 0x07);

                const char *wavePath = symbol_map_find(pwMap, waveSymbol);
                if (wavePath) {
                    uint32_t *pw = load_prog_wave(projectRoot, wavePath);
                    if (pw) {
                        td->wavePointer = pw;
                        vg_register_progwave(vg, pw);
                    }
                }
            }
            voiceIndex++;
        }
        /* voice_noise <key>, <pan>, <period>, <A>, <D>, <S>, <R> */
        else if (strncmp(trimmed, "voice_noise_alt ", 16) == 0) {
            int key, pan, period, attack, decay, sustain, release;
            if (sscanf(trimmed + 16, "%d, %d, %d, %d, %d, %d, %d",
                       &key, &pan, &period, &attack, &decay, &sustain, &release) == 7) {
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_NOISE_ALT;
                td->key = (uint8_t)key;
                td->wavePointer = (uint32_t *)(uintptr_t)(period & 0x01);
                td->attack = (uint8_t)(attack & 0x07);
                td->decay = (uint8_t)(decay & 0x07);
                td->sustain = (uint8_t)(sustain & 0x0F);
                td->release = (uint8_t)(release & 0x07);
            }
            voiceIndex++;
        } else if (strncmp(trimmed, "voice_noise ", 12) == 0) {
            int key, pan, period, attack, decay, sustain, release;
            if (sscanf(trimmed + 12, "%d, %d, %d, %d, %d, %d, %d",
                       &key, &pan, &period, &attack, &decay, &sustain, &release) == 7) {
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_NOISE;
                td->key = (uint8_t)key;
                td->wavePointer = (uint32_t *)(uintptr_t)(period & 0x01);
                td->attack = (uint8_t)(attack & 0x07);
                td->decay = (uint8_t)(decay & 0x07);
                td->sustain = (uint8_t)(sustain & 0x0F);
                td->release = (uint8_t)(release & 0x07);
            }
            voiceIndex++;
        }
        /* voice_keysplit <voicegroup_sym>, <keysplit_sym> */
        else if (strncmp(trimmed, "voice_keysplit_all ", 19) == 0) {
            char vgSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 19, "%s", vgSymbol) == 1) {
                rtrim(vgSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_KEYSPLIT_ALL;

                ToneData *subVg = load_sub_voicegroup(projectRoot, vgSymbol,
                                                       vg, dsMap, pwMap, ksMap);
                td->subGroup = subVg;
            }
            voiceIndex++;
        } else if (strncmp(trimmed, "voice_keysplit ", 15) == 0) {
            char vgSymbol[MAX_SYMBOL_LEN];
            char ksSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 15, "%[^,], %s", vgSymbol, ksSymbol) == 2) {
                rtrim(vgSymbol);
                rtrim(ksSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_KEYSPLIT;

                ToneData *subVg = load_sub_voicegroup(projectRoot, vgSymbol,
                                                       vg, dsMap, pwMap, ksMap);
                td->subGroup = subVg;

                /* Find and copy keysplit table */
                KeySplitDef *ksDef = keysplit_map_find(ksMap, ksSymbol);
                if (ksDef) {
                    uint8_t *table = malloc(128);
                    memcpy(table, ksDef->table, 128);
                    td->keySplitTable = table;
                    vg_register_keysplittable(vg, table);
                }
            }
            voiceIndex++;
        }
        /* cry / cry_reverse */
        else if (strncmp(trimmed, "cry_reverse ", 12) == 0) {
            char sampleSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 12, "%s", sampleSymbol) == 1) {
                rtrim(sampleSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_CRY_REVERSE;
                td->key = 60;
                td->attack = 0xFF;
                td->decay = 0;
                td->sustain = 0xFF;
                td->release = 0;

                const char *samplePath = symbol_map_find(dsMap, sampleSymbol);
                if (samplePath) {
                    WaveData *wd = load_wave_data(projectRoot, samplePath);
                    if (wd) {
                        td->wav = wd;
                        vg_register_wavedata(vg, wd);
                    }
                }
            }
            voiceIndex++;
        } else if (strncmp(trimmed, "cry ", 4) == 0) {
            char sampleSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 4, "%s", sampleSymbol) == 1) {
                rtrim(sampleSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_CRY;
                td->key = 60;
                td->attack = 0xFF;
                td->decay = 0;
                td->sustain = 0xFF;
                td->release = 0;

                const char *samplePath = symbol_map_find(dsMap, sampleSymbol);
                if (samplePath) {
                    WaveData *wd = load_wave_data(projectRoot, samplePath);
                    if (wd) {
                        td->wav = wd;
                        vg_register_wavedata(vg, wd);
                    }
                }
            }
            voiceIndex++;
        }
    }

    fclose(f);
    return 0;
}

/*
 * Main entry point: load a voicegroup from the pokeemerald project.
 */
LoadedVoiceGroup *voicegroup_load(const char *projectRoot, const char *voicegroupName)
{
    LoadedVoiceGroup *vg = calloc(1, sizeof(LoadedVoiceGroup));
    if (!vg) return NULL;

    /* Parse symbol maps */
    SymbolMap dsMap, pwMap;
    KeySplitMap ksMap;
    symbol_map_init(&dsMap);
    symbol_map_init(&pwMap);
    keysplit_map_init(&ksMap);

    if (parse_direct_sound_data(projectRoot, &dsMap) != 0) goto fail;
    if (parse_programmable_wave_data(projectRoot, &pwMap) != 0) goto fail;
    if (parse_keysplit_tables(projectRoot, &ksMap) != 0) goto fail;

    /* Find the voicegroup file */
    char filePath[MAX_PATH_LEN];
    if (!find_voicegroup_file(projectRoot, voicegroupName, filePath, sizeof(filePath))) {
        fprintf(stderr, "voicegroup_loader: cannot find voicegroup '%s'\n", voicegroupName);
        goto fail;
    }

    /* Parse the voicegroup */
    if (parse_voicegroup_file(projectRoot, filePath, vg, &dsMap, &pwMap, &ksMap) != 0) {
        goto fail;
    }

    symbol_map_free(&dsMap);
    symbol_map_free(&pwMap);
    keysplit_map_free(&ksMap);
    return vg;

fail:
    symbol_map_free(&dsMap);
    symbol_map_free(&pwMap);
    keysplit_map_free(&ksMap);
    voicegroup_free(vg);
    return NULL;
}

void voicegroup_free(LoadedVoiceGroup *vg)
{
    if (!vg) return;

    for (int i = 0; i < vg->waveDataCount; i++)
        free(vg->waveDatas[i]);
    free(vg->waveDatas);

    for (int i = 0; i < vg->progWaveCount; i++)
        free(vg->progWaves[i]);
    free(vg->progWaves);

    for (int i = 0; i < vg->subGroupCount; i++)
        free(vg->subGroups[i]);
    free(vg->subGroups);

    for (int i = 0; i < vg->keySplitTableCount; i++)
        free(vg->keySplitTables[i]);
    free(vg->keySplitTables);

    free(vg);
}
