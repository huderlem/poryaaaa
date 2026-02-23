#include "voicegroup_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

#define MAX_LINE 1024
#define MAX_PATH_LEN 512
#define MAX_SYMBOL_LEN 256
#define INITIAL_CAPACITY 64

#define MAX_DISCOVERED_PATHS 32

/* ---- Diagnostic logging ---- */

static const char *s_vgLogPath = NULL;

void voicegroup_loader_set_log_path(const char *path)
{
    s_vgLogPath = path;
}

static void vg_log(const char *fmt, ...)
{
    if (!s_vgLogPath) return;
    FILE *f = fopen(s_vgLogPath, "a");
    if (!f) return;
    time_t t = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&t));
    fprintf(f, "[%s] vg_loader: ", tbuf);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---- Discovery data structures ---- */

typedef struct {
    char paths[MAX_DISCOVERED_PATHS][MAX_PATH_LEN];
    int count;
} PathList;

typedef struct {
    PathList directSoundDataFiles;   /* paths to direct_sound_data.inc files */
    PathList progWaveDataFiles;      /* paths to programmable_wave_data.inc files */
    PathList keySplitTableFiles;     /* paths to keysplit_tables.inc files */
    PathList voicegroupDirs;         /* directories with individual .inc/.s voicegroup files */
    PathList monolithicVGFiles;      /* files containing multiple voicegroups (voice_groups.inc) */
    PathList wavSampleDirs;          /* directories with .wav sample files */
} ProjectDiscovery;

typedef struct {
    char filePath[MAX_PATH_LEN];
    char label[MAX_SYMBOL_LEN];  /* non-empty if inside a monolithic file */
    int found;
} VoicegroupLocation;

/* ---- Symbol maps ---- */

typedef struct {
    char symbol[MAX_SYMBOL_LEN];
    char filePath[MAX_PATH_LEN];
} SymbolMapping;

typedef struct {
    SymbolMapping *entries;
    int count;
    int capacity;
} SymbolMap;

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

static int parse_direct_sound_data_file(const char *filePath, const char *projectRoot, SymbolMap *map);
static int parse_programmable_wave_data_file(const char *filePath, const char *projectRoot, SymbolMap *map);
static int parse_keysplit_tables_file(const char *filePath, KeySplitMap *map);
static WaveData *load_wave_data_from_wav(const char *projectRoot, const char *relativeBinPath);
static WaveData *load_wav_from_path(const char *absoluteWavPath);
static WaveData *load_wave_data(const char *projectRoot, const char *relativePath);
static uint32_t *load_prog_wave(const char *projectRoot, const char *relativePath);
/* ---- WaveData deduplication cache ---- */

#define WAVE_CACHE_CAPACITY 128

typedef struct {
    char absPath[MAX_PATH_LEN];
    WaveData *wd;
} WaveCacheEntry;

typedef struct WaveCache {
    WaveCacheEntry entries[WAVE_CACHE_CAPACITY];
    int count;
} WaveCache;

static void wave_cache_init(WaveCache *cache) { cache->count = 0; }

static WaveData *wave_cache_find(const WaveCache *cache, const char *absPath)
{
    for (int i = 0; i < cache->count; i++)
        if (strcmp(cache->entries[i].absPath, absPath) == 0)
            return cache->entries[i].wd;
    return NULL;
}

static void wave_cache_insert(WaveCache *cache, const char *absPath, WaveData *wd)
{
    if (cache->count >= WAVE_CACHE_CAPACITY) return;
    strncpy(cache->entries[cache->count].absPath, absPath, MAX_PATH_LEN - 1);
    cache->entries[cache->count].absPath[MAX_PATH_LEN - 1] = '\0';
    cache->entries[cache->count].wd = wd;
    cache->count++;
}

static int parse_voicegroup_file(const char *projectRoot, const char *filePath,
                                  const char *startLabel,
                                  LoadedVoiceGroup *vg,
                                  const SymbolMap *dsMap, const SymbolMap *pwMap,
                                  const KeySplitMap *ksMap,
                                  const ProjectDiscovery *disc,
                                  WaveCache *waveCache);

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

/* Helper: try to open a file path, return 1 if it exists, 0 otherwise */
static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Helper: check if a path is a directory */
static int is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* Helper: add a path to a PathList if not already present and not full */
static void pathlist_add(PathList *list, const char *path)
{
    if (list->count >= MAX_DISCOVERED_PATHS) return;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->paths[i], path) == 0) return;
    }
    strncpy(list->paths[list->count], path, MAX_PATH_LEN - 1);
    list->paths[list->count][MAX_PATH_LEN - 1] = '\0';
    list->count++;
}

/* Helper: check if a string ends with a given suffix (case-insensitive) */
static int str_ends_with_ci(const char *str, const char *suffix)
{
    size_t slen = strlen(str);
    size_t sufflen = strlen(suffix);
    if (sufflen > slen) return 0;
    for (size_t i = 0; i < sufflen; i++) {
        if (tolower((unsigned char)str[slen - sufflen + i]) != tolower((unsigned char)suffix[i]))
            return 0;
    }
    return 1;
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

/* ---- Directory scanning helpers ---- */

/*
 * Check if a directory contains files matching a given extension.
 * Returns 1 if at least one matching file is found.
 */
static int dir_has_files_with_ext(const char *dirPath, const char *ext)
{
    DIR *d = opendir(dirPath);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (str_ends_with_ci(ent->d_name, ext)) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/*
 * Check if a directory contains any voice macro definitions (.inc or .s files
 * with voice_directsound, voice_square, voice_keysplit, etc.).
 * Quick heuristic: check first few matching files for voice macro keywords.
 */
static int dir_has_voice_macros(const char *dirPath)
{
    DIR *d = opendir(dirPath);
    if (!d) return 0;
    struct dirent *ent;
    int checked = 0;
    while ((ent = readdir(d)) != NULL && checked < 5) {
        if (ent->d_name[0] == '.') continue;
        if (!str_ends_with_ci(ent->d_name, ".inc") && !str_ends_with_ci(ent->d_name, ".s"))
            continue;
        char filePath[MAX_PATH_LEN];
        snprintf(filePath, sizeof(filePath), "%s%c%s", dirPath, PATH_SEP, ent->d_name);
        FILE *f = fopen(filePath, "r");
        if (!f) continue;
        char line[MAX_LINE];
        int lineCount = 0;
        while (fgets(line, sizeof(line), f) && lineCount < 50) {
            if (strstr(line, "voice_directsound") || strstr(line, "voice_square") ||
                strstr(line, "voice_programmable_wave") || strstr(line, "voice_noise") ||
                strstr(line, "voice_keysplit") || strstr(line, "voice_group")) {
                fclose(f);
                closedir(d);
                return 1;
            }
            lineCount++;
        }
        fclose(f);
        checked++;
    }
    closedir(d);
    return 0;
}

/*
 * Recursively scan under a base directory for subdirectories, up to maxDepth levels.
 * Calls the provided callback for each directory found (including basePath itself).
 */
typedef void (*dir_visit_fn)(const char *dirPath, void *ctx);

static void scan_dirs_recursive(const char *basePath, int depth, int maxDepth, dir_visit_fn visit, void *ctx)
{
    visit(basePath, ctx);
    if (depth >= maxDepth) return;

    DIR *d = opendir(basePath);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char subPath[MAX_PATH_LEN];
        snprintf(subPath, sizeof(subPath), "%s%c%s", basePath, PATH_SEP, ent->d_name);
        if (is_directory(subPath)) {
            scan_dirs_recursive(subPath, depth + 1, maxDepth, visit, ctx);
        }
    }
    closedir(d);
}

/* Combined visitor context for voicegroup and wav directory discovery */
typedef struct {
    ProjectDiscovery *disc;
} CombinedDirVisitorCtx;

static void visit_for_voicegroup_and_wav_dirs(const char *dirPath, void *ctx)
{
    CombinedDirVisitorCtx *vctx = (CombinedDirVisitorCtx *)ctx;
    if (dir_has_voice_macros(dirPath))
        pathlist_add(&vctx->disc->voicegroupDirs, dirPath);
    if (dir_has_files_with_ext(dirPath, ".wav"))
        pathlist_add(&vctx->disc->wavSampleDirs, dirPath);
}

/*
 * Check if a file is a monolithic voicegroup file (contains multiple labeled voicegroups).
 * Heuristic: file has multiple `<word>::` labels AND contains voice macros,
 * but is NOT just a list of .include directives pointing to a voicegroups/ subdir.
 */
static int is_monolithic_voicegroup_file(const char *filePath)
{
    FILE *f = fopen(filePath, "r");
    if (!f) return 0;

    char line[MAX_LINE];
    int labelCount = 0;
    int voiceMacroCount = 0;
    int includeCount = 0;
    int lineCount = 0;

    while (fgets(line, sizeof(line), f) && lineCount < 500) {
        strip_comment(line);
        rtrim(line);
        char *trimmed = ltrim(line);

        if (strstr(trimmed, "::") && trimmed[0] != '.' && trimmed[0] != '\0') {
            labelCount++;
        }
        if (strstr(trimmed, "voice_directsound") || strstr(trimmed, "voice_square") ||
            strstr(trimmed, "voice_programmable_wave") || strstr(trimmed, "voice_noise") ||
            strstr(trimmed, "voice_keysplit") || strstr(trimmed, "voice_group")) {
            voiceMacroCount++;
        }
        if (strstr(trimmed, ".include")) {
            includeCount++;
        }
        lineCount++;
    }
    fclose(f);

    /* It's monolithic if it has multiple labels AND voice macros,
     * and it's NOT primarily a hub of .include directives */
    if (labelCount >= 2 && voiceMacroCount > 0 && voiceMacroCount > includeCount) {
        return 1;
    }
    return 0;
}

/* ---- Project discovery ---- */

static void discover_project(const char *projectRoot,
                             const VoicegroupLoaderConfig *cfg,
                             ProjectDiscovery *out)
{
    memset(out, 0, sizeof(ProjectDiscovery));

    char path[MAX_PATH_LEN];
    char soundDir[MAX_PATH_LEN];
    build_path(soundDir, sizeof(soundDir), projectRoot, "sound");
    vg_log("discover_project: soundDir='%s' exists=%d", soundDir, is_directory(soundDir));

    /* 1. Config overrides first (prepended) */
    if (cfg) {
        for (int i = 0; i < cfg->soundDataPathCount && i < 8; i++) {
            build_path(path, sizeof(path), projectRoot, cfg->soundDataPaths[i]);
            if (file_exists(path))
                pathlist_add(&out->directSoundDataFiles, path);
        }
        for (int i = 0; i < cfg->voicegroupPathCount && i < 8; i++) {
            build_path(path, sizeof(path), projectRoot, cfg->voicegroupPaths[i]);
            if (is_directory(path)) {
                /* If it's a directory, add as voicegroup dir and scan for voice macros */
                pathlist_add(&out->voicegroupDirs, path);
                /* Also check if files inside are monolithic */
                DIR *d = opendir(path);
                if (d) {
                    struct dirent *ent;
                    while ((ent = readdir(d)) != NULL) {
                        if (ent->d_name[0] == '.') continue;
                        if (str_ends_with_ci(ent->d_name, ".inc") || str_ends_with_ci(ent->d_name, ".s")) {
                            char fpath[MAX_PATH_LEN];
                            snprintf(fpath, sizeof(fpath), "%s%c%s", path, PATH_SEP, ent->d_name);
                            if (is_monolithic_voicegroup_file(fpath))
                                pathlist_add(&out->monolithicVGFiles, fpath);
                        }
                    }
                    closedir(d);
                }
            } else if (file_exists(path)) {
                /* It's a file - check if it's monolithic or a voicegroup dir entry */
                if (is_monolithic_voicegroup_file(path))
                    pathlist_add(&out->monolithicVGFiles, path);
            }
        }
        for (int i = 0; i < cfg->sampleDirCount && i < 8; i++) {
            build_path(path, sizeof(path), projectRoot, cfg->sampleDirs[i]);
            if (is_directory(path))
                pathlist_add(&out->wavSampleDirs, path);
        }
    }

    /* 2. Standard direct_sound_data.inc, programmable_wave_data.inc, keysplit_tables.inc */
    build_path(path, sizeof(path), projectRoot, "sound/direct_sound_data.inc");
    if (file_exists(path))
        pathlist_add(&out->directSoundDataFiles, path);

    build_path(path, sizeof(path), projectRoot, "sound/programmable_wave_data.inc");
    if (file_exists(path))
        pathlist_add(&out->progWaveDataFiles, path);

    build_path(path, sizeof(path), projectRoot, "sound/keysplit_tables.inc");
    if (file_exists(path))
        pathlist_add(&out->keySplitTableFiles, path);

    /* 3. Standard voicegroup directories */
    build_path(path, sizeof(path), projectRoot, "sound/voicegroups");
    if (is_directory(path)) {
        pathlist_add(&out->voicegroupDirs, path);
        /* Also add keysplits/ and drumsets/ subdirs */
        char subPath[MAX_PATH_LEN];
        snprintf(subPath, sizeof(subPath), "%s%ckeysplits", path, PATH_SEP);
        if (is_directory(subPath))
            pathlist_add(&out->voicegroupDirs, subPath);
        snprintf(subPath, sizeof(subPath), "%s%cdrumsets", path, PATH_SEP);
        if (is_directory(subPath))
            pathlist_add(&out->voicegroupDirs, subPath);
    }

    /* 4. Scan under sound/ for voicegroup dirs AND wav dirs in one pass */
    vg_log("discover_project: scanning for voicegroup and wav dirs under '%s'", soundDir);
    if (is_directory(soundDir)) {
        CombinedDirVisitorCtx vctx = { .disc = out };
        scan_dirs_recursive(soundDir, 0, 3, visit_for_voicegroup_and_wav_dirs, &vctx);
    }
    vg_log("discover_project: dir scan done, vgDirs=%d wavDirs=%d",
           out->voicegroupDirs.count, out->wavSampleDirs.count);

    /* 5. Check for monolithic voicegroup files */
    build_path(path, sizeof(path), projectRoot, "sound/voice_groups.inc");
    vg_log("discover_project: checking monolithic '%s' exists=%d", path, file_exists(path));
    if (file_exists(path) && is_monolithic_voicegroup_file(path))
        pathlist_add(&out->monolithicVGFiles, path);
}

/* ---- Symbol data file parsing (parameterized by file path) ---- */

/*
 * Parse a direct_sound_data .inc file.
 * Builds symbol name -> file path mapping.
 */
static int parse_direct_sound_data_file(const char *filePath, const char *projectRoot, SymbolMap *map)
{
    (void)projectRoot; /* paths inside the file are relative to projectRoot, stored as-is */
    FILE *f = fopen(filePath, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", filePath);
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
 * Parse a programmable_wave_data .inc file.
 */
static int parse_programmable_wave_data_file(const char *filePath, const char *projectRoot, SymbolMap *map)
{
    (void)projectRoot;
    FILE *f = fopen(filePath, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", filePath);
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
 * Parse a keysplit_tables .inc file.
 */
static int parse_keysplit_tables_file(const char *filePath, KeySplitMap *map)
{
    FILE *f = fopen(filePath, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", filePath);
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
            /* pokeemerald macro format: keysplit tableName, startNote */
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
            int index, endNote;
            if (sscanf(trimmed + 6, "%d, %d", &index, &endNote) == 2) {
                for (int n = lastNote; n < endNote && n < 128; n++) {
                    current->table[n] = (uint8_t)index;
                }
                lastNote = endNote;
                if (endNote > current->maxNote)
                    current->maxNote = endNote;
            }
        } else if (strncmp(trimmed, ".set ", 5) == 0) {
            /* pokefirered raw format: .set TableName, . - startNote */
            char name[MAX_SYMBOL_LEN];
            int startNote = 0;
            if (sscanf(trimmed + 5, "%[^,], . - %d", name, &startNote) == 2) {
                rtrim(name);
                if (map->count >= map->capacity) {
                    map->capacity = map->capacity ? map->capacity * 2 : INITIAL_CAPACITY;
                    map->entries = realloc(map->entries, sizeof(KeySplitDef) * map->capacity);
                }
                current = &map->entries[map->count];
                memset(current, 0, sizeof(KeySplitDef));
                strncpy(current->name, name, MAX_SYMBOL_LEN - 1);
                current->name[MAX_SYMBOL_LEN - 1] = '\0';
                current->startingNote = startNote;
                current->maxNote = 0;
                lastNote = startNote;
                map->count++;
            }
        } else if (strncmp(trimmed, ".byte ", 6) == 0 && current) {
            /* raw per-note byte values; strip_comment already removed the @ note annotation */
            char *p = trimmed + 6;
            while (*p) {
                char *end;
                long val = strtol(p, &end, 10);
                if (end == p) break;
                if (lastNote < 128) {
                    current->table[lastNote] = (uint8_t)val;
                    if (lastNote > current->maxNote)
                        current->maxNote = lastNote;
                    lastNote++;
                }
                p = end;
                while (isspace((unsigned char)*p)) p++;
                if (*p == ',') p++;
                while (isspace((unsigned char)*p)) p++;
            }
        }
    }

    fclose(f);
    return 0;
}

/* Wrappers that iterate over all discovered paths */

static void parse_all_direct_sound_data(const ProjectDiscovery *disc, const char *projectRoot, SymbolMap *map)
{
    for (int i = 0; i < disc->directSoundDataFiles.count; i++) {
        parse_direct_sound_data_file(disc->directSoundDataFiles.paths[i], projectRoot, map);
    }
}

static void parse_all_programmable_wave_data(const ProjectDiscovery *disc, const char *projectRoot, SymbolMap *map)
{
    for (int i = 0; i < disc->progWaveDataFiles.count; i++) {
        parse_programmable_wave_data_file(disc->progWaveDataFiles.paths[i], projectRoot, map);
    }
}

static void parse_all_keysplit_tables(const ProjectDiscovery *disc, KeySplitMap *map)
{
    for (int i = 0; i < disc->keySplitTableFiles.count; i++) {
        parse_keysplit_tables_file(disc->keySplitTableFiles.paths[i], map);
    }
}

/* ---- Sample loading ---- */

/*
 * Load a .wav file from an absolute path.
 * Parses RIFF/WAVE fmt, smpl, agbp, agbl, and data chunks.
 */
static WaveData *load_wav_from_path(const char *absoluteWavPath)
{
    FILE *f = fopen(absoluteWavPath, "rb");
    if (!f) return NULL;

    /* Read RIFF/WAVE header (12 bytes) */
    uint8_t riffHdr[12];
    if (fread(riffHdr, 1, 12, f) != 12 ||
        memcmp(riffHdr, "RIFF", 4) != 0 ||
        memcmp(riffHdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        fprintf(stderr, "voicegroup_loader: invalid RIFF/WAVE header in %s\n", absoluteWavPath);
        return NULL;
    }
    uint32_t riffSize = riffHdr[4] | ((uint32_t)riffHdr[5] << 8) |
                        ((uint32_t)riffHdr[6] << 16) | ((uint32_t)riffHdr[7] << 24);
    long fileEnd = 8 + (long)riffSize;

    /* Chunk parsing state */
    int fmtFound = 0, dataFound = 0;

    /* fmt fields */
    int fmtTag = 0;
    uint32_t sampleRate = 0;
    uint16_t blockAlign = 0, bitsPerSample = 0;

    /* smpl fields */
    uint32_t midiKey = 60, midiPitchFraction = 0;
    uint32_t smplLoopStart = 0, smplLoopEnd = 0;
    int loopEnabled = 0;

    /* agbp / agbl custom chunk values */
    uint32_t agbPitch = 0, agbLoopEnd = 0;

    /* data chunk location */
    long dataOffset = 0;
    uint32_t dataLen = 0;

    /* Iterate RIFF chunks */
    while (1) {
        long pos = ftell(f);
        if (pos < 0 || pos + 8 > fileEnd)
            break;

        uint8_t chunkHdr[8];
        if (fread(chunkHdr, 1, 8, f) != 8)
            break;

        uint32_t chunkLen = chunkHdr[4] | ((uint32_t)chunkHdr[5] << 8) |
                            ((uint32_t)chunkHdr[6] << 16) | ((uint32_t)chunkHdr[7] << 24);
        long chunkDataStart = ftell(f);

        if (memcmp(chunkHdr, "fmt ", 4) == 0 && chunkLen >= 16) {
            uint8_t d[16];
            if (fread(d, 1, 16, f) == 16) {
                fmtTag        = d[0] | (d[1] << 8);
                sampleRate    = d[4]  | ((uint32_t)d[5]  << 8) |
                                ((uint32_t)d[6]  << 16) | ((uint32_t)d[7]  << 24);
                blockAlign    = (uint16_t)(d[12] | (d[13] << 8));
                bitsPerSample = (uint16_t)(d[14] | (d[15] << 8));
                fmtFound = 1;
            }
        } else if (memcmp(chunkHdr, "smpl", 4) == 0 && chunkLen >= 32) {
            uint32_t readLen = chunkLen < 52 ? chunkLen : 52;
            uint8_t d[52];
            if (fread(d, 1, readLen, f) == readLen) {
                midiKey = d[12] | ((uint32_t)d[13] << 8) |
                          ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
                if (midiKey > 127) midiKey = 127;
                midiPitchFraction = d[16] | ((uint32_t)d[17] << 8) |
                                    ((uint32_t)d[18] << 16) | ((uint32_t)d[19] << 24);
                uint32_t numLoops = d[28] | ((uint32_t)d[29] << 8) |
                                    ((uint32_t)d[30] << 16) | ((uint32_t)d[31] << 24);
                if (numLoops == 1 && readLen >= 52) {
                    smplLoopStart = d[44] | ((uint32_t)d[45] << 8) |
                                    ((uint32_t)d[46] << 16) | ((uint32_t)d[47] << 24);
                    uint32_t loopEndIncl = d[48] | ((uint32_t)d[49] << 8) |
                                          ((uint32_t)d[50] << 16) | ((uint32_t)d[51] << 24);
                    smplLoopEnd = loopEndIncl + 1;
                    loopEnabled = 1;
                }
            }
        } else if (memcmp(chunkHdr, "agbp", 4) == 0 && chunkLen >= 4) {
            uint8_t d[4];
            if (fread(d, 1, 4, f) == 4)
                agbPitch = d[0] | ((uint32_t)d[1] << 8) |
                           ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
        } else if (memcmp(chunkHdr, "agbl", 4) == 0 && chunkLen >= 4) {
            uint8_t d[4];
            if (fread(d, 1, 4, f) == 4)
                agbLoopEnd = d[0] | ((uint32_t)d[1] << 8) |
                             ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
        } else if (memcmp(chunkHdr, "data", 4) == 0) {
            dataOffset = chunkDataStart;
            dataLen    = chunkLen;
            dataFound  = 1;
        }

        long nextChunk = chunkDataStart + (long)chunkLen;
        if (chunkLen & 1) nextChunk++;
        if (fseek(f, nextChunk, SEEK_SET) != 0)
            break;
    }

    if (!fmtFound || !dataFound) {
        fclose(f);
        fprintf(stderr, "voicegroup_loader: missing fmt or data chunk in %s\n", absoluteWavPath);
        return NULL;
    }

    /* Determine bytes per sample from fmt chunk */
    uint32_t bytesPerSample;
    if (fmtTag == 1) {
        if      (blockAlign == 1 && bitsPerSample == 8)  bytesPerSample = 1;
        else if (blockAlign == 2 && bitsPerSample == 16) bytesPerSample = 2;
        else if (blockAlign == 3 && bitsPerSample == 24) bytesPerSample = 3;
        else if (blockAlign == 4 && bitsPerSample == 32) bytesPerSample = 4;
        else {
            fclose(f);
            fprintf(stderr, "voicegroup_loader: unsupported integer PCM format in %s\n", absoluteWavPath);
            return NULL;
        }
    } else if (fmtTag == 3) {
        if      (blockAlign == 4 && bitsPerSample == 32) bytesPerSample = 4;
        else if (blockAlign == 8 && bitsPerSample == 64) bytesPerSample = 8;
        else {
            fclose(f);
            fprintf(stderr, "voicegroup_loader: unsupported float format in %s\n", absoluteWavPath);
            return NULL;
        }
    } else {
        fclose(f);
        fprintf(stderr, "voicegroup_loader: unsupported audio format %d in %s\n", fmtTag, absoluteWavPath);
        return NULL;
    }

    uint32_t numSamples = dataLen / bytesPerSample;

    uint32_t loopEnd;
    if (loopEnabled)
        loopEnd = smplLoopEnd;
    else
        loopEnd = numSamples;
    if (loopEnd > numSamples)
        loopEnd = numSamples;
    if (agbLoopEnd != 0)
        loopEnd = agbLoopEnd;

    uint32_t size = loopEnd;

    uint32_t freq;
    if (agbPitch != 0) {
        freq = agbPitch;
    } else if (midiKey == 60 && midiPitchFraction == 0) {
        freq = (uint32_t)((double)sampleRate * 1024.0);
    } else {
        double tuning = (double)midiPitchFraction / (4294967296.0 * 100.0);
        double pitch  = (double)sampleRate *
                        pow(2.0, (60.0 - (double)midiKey) / 12.0 + tuning / 1200.0);
        freq = (uint32_t)(pitch * 1024.0);
    }

    WaveData *wd = malloc(sizeof(WaveData) + (size_t)size + 1);
    if (!wd) {
        fclose(f);
        return NULL;
    }
    wd->type      = 0;
    wd->status    = loopEnabled ? 0x4000 : 0;
    wd->freq      = freq;
    wd->loopStart = smplLoopStart;
    wd->size      = size;
    wd->data      = (int8_t *)((uint8_t *)wd + sizeof(WaveData));

    size_t rawBytes = (size_t)size * bytesPerSample;
    uint8_t *rawData = NULL;
    if (rawBytes > 0) {
        rawData = malloc(rawBytes);
        if (!rawData) {
            free(wd);
            fclose(f);
            return NULL;
        }
        if (fseek(f, dataOffset, SEEK_SET) != 0) {
            free(rawData);
            free(wd);
            fclose(f);
            return NULL;
        }
        size_t bytesRead = fread(rawData, 1, rawBytes, f);
        if (bytesRead < rawBytes)
            memset(rawData + bytesRead, 0, rawBytes - bytesRead);
    }
    fclose(f);

    /* Convert raw samples to int8_t */
    for (uint32_t i = 0; i < size; i++) {
        uint8_t *sp = rawData + (size_t)i * bytesPerSample;
        int8_t s;
        if (fmtTag == 1) {
            if (bytesPerSample == 1) {
                s = (int8_t)((int)sp[0] - 128);
            } else if (bytesPerSample == 2) {
                int16_t v = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
                s = (int8_t)(v >> 8);
            } else if (bytesPerSample == 3) {
                uint32_t raw = (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16);
                int32_t v = (raw & 0x800000u) ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;
                s = (int8_t)(v >> 16);
            } else {
                int32_t v = (int32_t)((uint32_t)sp[0] | ((uint32_t)sp[1] << 8) |
                                      ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24));
                s = (int8_t)(v >> 24);
            }
        } else {
            double ds;
            if (bytesPerSample == 4) {
                uint32_t bits = (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) |
                                ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24);
                float fv;
                memcpy(&fv, &bits, sizeof(fv));
                ds = (double)fv;
            } else {
                uint64_t bits = (uint64_t)sp[0] | ((uint64_t)sp[1] << 8) |
                                ((uint64_t)sp[2] << 16) | ((uint64_t)sp[3] << 24) |
                                ((uint64_t)sp[4] << 32) | ((uint64_t)sp[5] << 40) |
                                ((uint64_t)sp[6] << 48) | ((uint64_t)sp[7] << 56);
                double dv;
                memcpy(&dv, &bits, sizeof(dv));
                ds = dv;
            }
            int si = (int)floor(ds * 128.0);
            if (si < -128) si = -128;
            if (si >  127) si =  127;
            s = (int8_t)si;
        }
        wd->data[i] = s;
    }

    free(rawData);
    wd->data[size] = (size > 0) ? wd->data[size - 1] : 0;
    return wd;
}

/*
 * Load a PCM instrument sample from a .wav file.
 * Derives the .wav path by replacing the .bin extension in relativeBinPath.
 * Falls back to load_wave_data() if the .wav is not found.
 */
static WaveData *load_wave_data_from_wav(const char *projectRoot, const char *relativeBinPath)
{
    char relativeWavPath[MAX_PATH_LEN];
    strncpy(relativeWavPath, relativeBinPath, MAX_PATH_LEN - 1);
    relativeWavPath[MAX_PATH_LEN - 1] = '\0';

    size_t pathLen = strlen(relativeWavPath);
    char *ext = NULL;
    if (pathLen >= 4 && strcmp(relativeWavPath + pathLen - 4, ".bin") == 0)
        ext = relativeWavPath + pathLen - 4;

    if (!ext) {
        return load_wave_data(projectRoot, relativeBinPath);
    }
    ext[1] = 'w'; ext[2] = 'a'; ext[3] = 'v';

    char fullPath[MAX_PATH_LEN];
    build_path(fullPath, sizeof(fullPath), projectRoot, relativeWavPath);

    WaveData *wd = load_wav_from_path(fullPath);
    if (wd) return wd;

    /* .wav not found or failed — fall back to .bin loader */
    return load_wave_data(projectRoot, relativeBinPath);
}

/*
 * Load a .bin sample file (DirectSound wave data).
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

    uint8_t header[16];
    if (fread(header, 1, 16, f) != 16) {
        fprintf(stderr, "voicegroup_loader: short read on header %s\n", fullPath);
        fclose(f);
        return NULL;
    }

    uint16_t type = header[0] | (header[1] << 8);
    uint32_t freq = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);
    uint32_t loopStart = header[8] | (header[9] << 8) | (header[10] << 16) | (header[11] << 24);
    uint32_t size = header[12] | (header[13] << 8) | (header[14] << 16) | (header[15] << 24);

    WaveData *wd = malloc(sizeof(WaveData) + size + 1);
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

    size_t bytesRead = fread(wd->data, 1, size, f);
    if (bytesRead < size) {
        memset(wd->data + bytesRead, 0, size - bytesRead);
    }
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

/* ---- Sample fallback resolution ---- */

/*
 * Try to find and load a .wav sample by searching discovered wav directories.
 */
static WaveData *resolve_sample_from_wav_dirs(const char *symbol,
                                               const ProjectDiscovery *disc)
{
    for (int i = 0; i < disc->wavSampleDirs.count; i++) {
        char wavPath[MAX_PATH_LEN];
        snprintf(wavPath, sizeof(wavPath), "%s%c%s.wav", disc->wavSampleDirs.paths[i], PATH_SEP, symbol);
        WaveData *wd = load_wav_from_path(wavPath);
        if (wd) return wd;
    }
    return NULL;
}

/*
 * Unified sample resolution: try symbol map first, then fallback to wav dirs.
 * Uses waveCache to avoid loading the same file more than once.
 * Registers newly loaded WaveData with vg; cache hits are NOT re-registered.
 */
static WaveData *resolve_and_load_sample(const char *projectRoot, const char *symbol,
                                          const SymbolMap *dsMap, const ProjectDiscovery *disc,
                                          LoadedVoiceGroup *vg, WaveCache *waveCache)
{
    const char *samplePath = symbol_map_find(dsMap, symbol);
    if (samplePath) {
        /* Build the absolute .wav path to use as cache key */
        char relWavPath[MAX_PATH_LEN];
        strncpy(relWavPath, samplePath, MAX_PATH_LEN - 1);
        relWavPath[MAX_PATH_LEN - 1] = '\0';
        size_t pathLen = strlen(relWavPath);
        if (pathLen >= 4 && strcmp(relWavPath + pathLen - 4, ".bin") == 0) {
            relWavPath[pathLen - 3] = 'w';
            relWavPath[pathLen - 2] = 'a';
            relWavPath[pathLen - 1] = 'v';
        }
        char absWavPath[MAX_PATH_LEN];
        build_path(absWavPath, sizeof(absWavPath), projectRoot, relWavPath);

        WaveData *cached = wave_cache_find(waveCache, absWavPath);
        if (cached) return cached;

        WaveData *wd = load_wave_data_from_wav(projectRoot, samplePath);
        if (wd) {
            vg_register_wavedata(vg, wd);
            wave_cache_insert(waveCache, absWavPath, wd);
            return wd;
        }
    }
    /* Fallback: search wav directories */
    if (disc) {
        for (int i = 0; i < disc->wavSampleDirs.count; i++) {
            char wavPath[MAX_PATH_LEN];
            snprintf(wavPath, sizeof(wavPath), "%s%c%s.wav",
                     disc->wavSampleDirs.paths[i], PATH_SEP, symbol);
            WaveData *cached = wave_cache_find(waveCache, wavPath);
            if (cached) return cached;
            WaveData *wd = load_wav_from_path(wavPath);
            if (wd) {
                vg_register_wavedata(vg, wd);
                wave_cache_insert(waveCache, wavPath, wd);
                return wd;
            }
        }
    }
    return NULL;
}

/* ---- Flexible voicegroup finding ---- */

/* Returns 1 if the last path component of dirPath equals name. */
static int dir_last_component_is(const char *dirPath, const char *name)
{
    size_t dlen = strlen(dirPath);
    size_t nlen = strlen(name);
    if (nlen > dlen) return 0;
    const char *tail = dirPath + dlen - nlen;
    if (strcmp(tail, name) != 0) return 0;
    if (tail == dirPath) return 1;
    char c = *(tail - 1);
    return c == '/' || c == '\\';
}

/*
 * Search for a voicegroup by name across all discovered locations.
 */
static VoicegroupLocation find_voicegroup(const char *projectRoot,
                                           const char *vgName,
                                           const ProjectDiscovery *disc)
{
    VoicegroupLocation loc;
    memset(&loc, 0, sizeof(loc));

    char path[MAX_PATH_LEN];

    /* 1. Individual files in discovered voicegroup directories */
    for (int i = 0; i < disc->voicegroupDirs.count; i++) {
        /* Try <dir>/<name>.inc */
        snprintf(path, sizeof(path), "%s%c%s.inc", disc->voicegroupDirs.paths[i], PATH_SEP, vgName);
        if (file_exists(path)) {
            strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
            loc.found = 1;
            return loc;
        }
        /* Try <dir>/<name>.s */
        snprintf(path, sizeof(path), "%s%c%s.s", disc->voicegroupDirs.paths[i], PATH_SEP, vgName);
        if (file_exists(path)) {
            strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
            loc.found = 1;
            return loc;
        }
    }

    /* 2. Keysplit/drumset suffix conventions.
     *
     * IMPORTANT: only search inside directories whose last path component is
     * "keysplits" (or "drumsets"), and also try an explicit
     * <voicegroupDir>/keysplits/<base>.inc probe.  Searching every voicegroup
     * dir would find the *main* <base>.inc file (e.g. petalburg.inc) instead
     * of the keysplit sub-voicegroup, causing infinite recursion.
     */

    {
        const char *suffix = strstr(vgName, "_keysplit");
        if (suffix) {
            char baseName[MAX_SYMBOL_LEN];
            int baseLen = (int)(suffix - vgName);
            if (baseLen > 0 && baseLen < MAX_SYMBOL_LEN) {
                memcpy(baseName, vgName, baseLen);
                baseName[baseLen] = '\0';
                /* Explicit <dir>/keysplits/<base>.inc probe for each voicegroup dir */
                for (int i = 0; i < disc->voicegroupDirs.count; i++) {
                    snprintf(path, sizeof(path), "%s%ckeysplits%c%s.inc",
                             disc->voicegroupDirs.paths[i], PATH_SEP, PATH_SEP, baseName);
                    if (file_exists(path)) {
                        strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
                        loc.found = 1;
                        return loc;
                    }
                    snprintf(path, sizeof(path), "%s%ckeysplits%c%s.s",
                             disc->voicegroupDirs.paths[i], PATH_SEP, PATH_SEP, baseName);
                    if (file_exists(path)) {
                        strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
                        loc.found = 1;
                        return loc;
                    }
                }
                /* Also check dirs that are themselves named "keysplits" */
                for (int i = 0; i < disc->voicegroupDirs.count; i++) {
                    if (!dir_last_component_is(disc->voicegroupDirs.paths[i], "keysplits"))
                        continue;
                    snprintf(path, sizeof(path), "%s%c%s.inc", disc->voicegroupDirs.paths[i], PATH_SEP, baseName);
                    if (file_exists(path)) {
                        strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
                        loc.found = 1;
                        return loc;
                    }
                    snprintf(path, sizeof(path), "%s%c%s.s", disc->voicegroupDirs.paths[i], PATH_SEP, baseName);
                    if (file_exists(path)) {
                        strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
                        loc.found = 1;
                        return loc;
                    }
                }
            }
        }
    }
    {
        const char *suffix = strstr(vgName, "_drumset");
        if (suffix) {
            char baseName[MAX_SYMBOL_LEN];
            int baseLen = (int)(suffix - vgName);
            if (baseLen > 0 && baseLen < MAX_SYMBOL_LEN) {
                memcpy(baseName, vgName, baseLen);
                baseName[baseLen] = '\0';
                /* Explicit <dir>/drumsets/<base>.inc probe for each voicegroup dir */
                for (int i = 0; i < disc->voicegroupDirs.count; i++) {
                    snprintf(path, sizeof(path), "%s%cdrumsets%c%s.inc",
                             disc->voicegroupDirs.paths[i], PATH_SEP, PATH_SEP, baseName);
                    if (file_exists(path)) {
                        strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
                        loc.found = 1;
                        return loc;
                    }
                    snprintf(path, sizeof(path), "%s%cdrumsets%c%s.s",
                             disc->voicegroupDirs.paths[i], PATH_SEP, PATH_SEP, baseName);
                    if (file_exists(path)) {
                        strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
                        loc.found = 1;
                        return loc;
                    }
                }
                /* Also check dirs that are themselves named "drumsets" */
                for (int i = 0; i < disc->voicegroupDirs.count; i++) {
                    if (!dir_last_component_is(disc->voicegroupDirs.paths[i], "drumsets"))
                        continue;
                    snprintf(path, sizeof(path), "%s%c%s.inc", disc->voicegroupDirs.paths[i], PATH_SEP, baseName);
                    if (file_exists(path)) {
                        strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
                        loc.found = 1;
                        return loc;
                    }
                    snprintf(path, sizeof(path), "%s%c%s.s", disc->voicegroupDirs.paths[i], PATH_SEP, baseName);
                    if (file_exists(path)) {
                        strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
                        loc.found = 1;
                        return loc;
                    }
                }
            }
        }
    }


    /* 3. Also try vg_<name>.s and vg_<name>.inc patterns (eventide convention) */
    for (int i = 0; i < disc->voicegroupDirs.count; i++) {
        snprintf(path, sizeof(path), "%s%cvg_%s.inc", disc->voicegroupDirs.paths[i], PATH_SEP, vgName);
        if (file_exists(path)) {
            strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
            loc.found = 1;
            return loc;
        }
        snprintf(path, sizeof(path), "%s%cvg_%s.s", disc->voicegroupDirs.paths[i], PATH_SEP, vgName);
        if (file_exists(path)) {
            strncpy(loc.filePath, path, MAX_PATH_LEN - 1);
            loc.found = 1;
            return loc;
        }
    }

    /* 4. Monolithic files: scan for <name>:: label */
    for (int i = 0; i < disc->monolithicVGFiles.count; i++) {
        FILE *f = fopen(disc->monolithicVGFiles.paths[i], "r");
        if (!f) continue;

        char searchLabel[MAX_SYMBOL_LEN + 4];
        snprintf(searchLabel, sizeof(searchLabel), "%s::", vgName);

        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            strip_comment(line);
            char *trimmed = ltrim(line);
            if (strstr(trimmed, searchLabel) == trimmed) {
                strncpy(loc.filePath, disc->monolithicVGFiles.paths[i], MAX_PATH_LEN - 1);
                strncpy(loc.label, vgName, MAX_SYMBOL_LEN - 1);
                loc.found = 1;
                fclose(f);
                return loc;
            }
        }
        fclose(f);
    }

    return loc;
}

/* ---- Voicegroup parsing ---- */

/*
 * Load a sub-voicegroup (for keysplit/keysplit_all references).
 */
static ToneData *load_sub_voicegroup(const char *projectRoot, const char *vgSymbol,
                                      LoadedVoiceGroup *vg,
                                      const SymbolMap *dsMap, const SymbolMap *pwMap,
                                      const KeySplitMap *ksMap,
                                      const ProjectDiscovery *disc,
                                      WaveCache *waveCache)
{
    const char *name = vgSymbol;
    if (strncmp(name, "voicegroup_", 11) == 0)
        name += 11;

    VoicegroupLocation loc = find_voicegroup(projectRoot, name, disc);
    if (!loc.found) {
        fprintf(stderr, "voicegroup_loader: cannot find sub-voicegroup '%s'\n", vgSymbol);
        return NULL;
    }

    ToneData *subVg = calloc(VOICEGROUP_SIZE, sizeof(ToneData));
    if (!subVg) return NULL;

    ToneData savedVoices[VOICEGROUP_SIZE];
    memcpy(savedVoices, vg->voices, sizeof(savedVoices));
    memset(vg->voices, 0, sizeof(vg->voices));

    const char *startLabel = loc.label[0] ? loc.label : NULL;
    if (parse_voicegroup_file(projectRoot, loc.filePath, startLabel,
                               vg, dsMap, pwMap, ksMap, disc, waveCache) != 0) {
        free(subVg);
        memcpy(vg->voices, savedVoices, sizeof(savedVoices));
        return NULL;
    }

    memcpy(subVg, vg->voices, sizeof(ToneData) * VOICEGROUP_SIZE);
    memcpy(vg->voices, savedVoices, sizeof(savedVoices));

    vg_register_subgroup(vg, subVg);
    return subVg;
}

/*
 * Parse a voicegroup file and populate the ToneData array.
 *
 * When startLabel is non-NULL, scanning starts at the "<startLabel>::" label
 * and stops when a new label or .align 2 is encountered (monolithic file mode).
 * When startLabel is NULL, the entire file is parsed (individual file mode).
 */
static int parse_voicegroup_file(const char *projectRoot, const char *filePath,
                                  const char *startLabel,
                                  LoadedVoiceGroup *vg,
                                  const SymbolMap *dsMap, const SymbolMap *pwMap,
                                  const KeySplitMap *ksMap,
                                  const ProjectDiscovery *disc,
                                  WaveCache *waveCache)
{
    vg_log("parse_voicegroup_file: '%s' label='%s'", filePath, startLabel ? startLabel : "(none)");
    FILE *f = fopen(filePath, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", filePath);
        return -1;
    }

    char line[MAX_LINE];
    int voiceIndex = 0;
    int inSection = (startLabel == NULL); /* if no startLabel, parse from the beginning */
    int voicesParsedInSection = 0;

    /* If startLabel is set, build the search string */
    char searchLabel[MAX_SYMBOL_LEN + 4];
    if (startLabel) {
        snprintf(searchLabel, sizeof(searchLabel), "%s::", startLabel);
    }

    while (fgets(line, sizeof(line), f) && voiceIndex < VOICEGROUP_SIZE) {
        strip_comment(line);
        rtrim(line);
        char *trimmed = ltrim(line);

        if (trimmed[0] == '\0')
            continue;

        /* When looking for a start label, skip until we find it */
        if (startLabel && !inSection) {
            if (strstr(trimmed, searchLabel) == trimmed) {
                inSection = 1;
            }
            continue;
        }

        /* In monolithic mode, stop at the next label or .align 2 after parsing voices */
        if (startLabel && inSection && voicesParsedInSection > 0) {
            /* Check for a new label (word followed by ::) */
            char *cc = strstr(trimmed, "::");
            if (cc && cc > trimmed && !isspace((unsigned char)trimmed[0])) {
                break;
            }
            /* Check for .align 2 which separates voicegroups */
            if (strncmp(trimmed, ".align", 6) == 0) {
                break;
            }
        }

        /* Parse voice_group declaration for starting_note offset */
        if (strncmp(trimmed, "voice_group ", 12) == 0) {
            char vgDeclName[MAX_SYMBOL_LEN];
            int startingNote = 0;
            if (sscanf(trimmed + 12, "%[^,\n], %d", vgDeclName, &startingNote) >= 2) {
                if (startingNote > 0 && startingNote < VOICEGROUP_SIZE)
                    voiceIndex = startingNote;
            }
            continue;
        }

        /* voice_directsound variants */
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

                WaveData *wd = resolve_and_load_sample(projectRoot, sampleSymbol, dsMap, disc, vg, waveCache);
                if (wd) {
                    td->wav = wd;
                }
            }
            voiceIndex++;
            voicesParsedInSection++;
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

                WaveData *wd = resolve_and_load_sample(projectRoot, sampleSymbol, dsMap, disc, vg, waveCache);
                if (wd) {
                    td->wav = wd;
                }
            }
            voiceIndex++;
            voicesParsedInSection++;
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

                WaveData *wd = resolve_and_load_sample(projectRoot, sampleSymbol, dsMap, disc, vg, waveCache);
                if (wd) {
                    td->wav = wd;
                }
            }
            voiceIndex++;
            voicesParsedInSection++;
        }
        /* voice_square_1 */
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
            voicesParsedInSection++;
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
            voicesParsedInSection++;
        }
        /* voice_square_2 */
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
            voicesParsedInSection++;
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
            voicesParsedInSection++;
        }
        /* voice_programmable_wave */
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
            voicesParsedInSection++;
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
            voicesParsedInSection++;
        }
        /* voice_noise */
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
            voicesParsedInSection++;
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
            voicesParsedInSection++;
        }
        /* voice_keysplit */
        else if (strncmp(trimmed, "voice_keysplit_all ", 19) == 0) {
            char vgSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 19, "%s", vgSymbol) == 1) {
                rtrim(vgSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_KEYSPLIT_ALL;

                ToneData *subVg = load_sub_voicegroup(projectRoot, vgSymbol,
                                                       vg, dsMap, pwMap, ksMap, disc, waveCache);
                td->subGroup = subVg;
            }
            voiceIndex++;
            voicesParsedInSection++;
        } else if (strncmp(trimmed, "voice_keysplit ", 15) == 0) {
            char vgSymbol[MAX_SYMBOL_LEN];
            char ksSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 15, "%[^,], %s", vgSymbol, ksSymbol) == 2) {
                rtrim(vgSymbol);
                rtrim(ksSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_KEYSPLIT;

                ToneData *subVg = load_sub_voicegroup(projectRoot, vgSymbol,
                                                       vg, dsMap, pwMap, ksMap, disc, waveCache);
                td->subGroup = subVg;

                KeySplitDef *ksDef = keysplit_map_find(ksMap, ksSymbol);
                if (ksDef) {
                    uint8_t *table = malloc(128);
                    memcpy(table, ksDef->table, 128);
                    td->keySplitTable = table;
                    vg_register_keysplittable(vg, table);
                }
            }
            voiceIndex++;
            voicesParsedInSection++;
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
            voicesParsedInSection++;
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
            voicesParsedInSection++;
        }
    }

    vg_log("parse_voicegroup_file: done, voiceIndex=%d", voiceIndex);
    fclose(f);
    return 0;
}

/*
 * Main entry point: load a voicegroup from a project.
 */
LoadedVoiceGroup *voicegroup_load(const char *projectRoot, const char *voicegroupName,
                                   const VoicegroupLoaderConfig *config)
{
    vg_log("voicegroup_load: start root='%s' vg='%s'", projectRoot, voicegroupName);

    LoadedVoiceGroup *vg = calloc(1, sizeof(LoadedVoiceGroup));
    if (!vg) return NULL;

    /* Heap-allocate ProjectDiscovery: ~96 KB on the stack would risk overflow
     * in Reaper's plugin-load thread (Windows default: 1 MB stack). */
    ProjectDiscovery *disc = calloc(1, sizeof(ProjectDiscovery));
    if (!disc) {
        voicegroup_free(vg);
        return NULL;
    }

    /* Discover project structure */
    vg_log("voicegroup_load: calling discover_project");
    discover_project(projectRoot, config, disc);
    vg_log("voicegroup_load: discover done - dsFiles=%d pwFiles=%d ksFiles=%d vgDirs=%d monoFiles=%d wavDirs=%d",
           disc->directSoundDataFiles.count, disc->progWaveDataFiles.count,
           disc->keySplitTableFiles.count, disc->voicegroupDirs.count,
           disc->monolithicVGFiles.count, disc->wavSampleDirs.count);

    /* Per-load WaveData deduplication cache */
    WaveCache waveCache;
    wave_cache_init(&waveCache);

    /* Parse symbol maps from all discovered files */
    SymbolMap dsMap, pwMap;
    KeySplitMap ksMap;
    symbol_map_init(&dsMap);
    symbol_map_init(&pwMap);
    keysplit_map_init(&ksMap);

    vg_log("voicegroup_load: parsing symbol maps");
    parse_all_direct_sound_data(disc, projectRoot, &dsMap);
    vg_log("voicegroup_load: dsMap entries=%d", dsMap.count);
    parse_all_programmable_wave_data(disc, projectRoot, &pwMap);
    vg_log("voicegroup_load: pwMap entries=%d", pwMap.count);
    parse_all_keysplit_tables(disc, &ksMap);
    vg_log("voicegroup_load: ksMap entries=%d", ksMap.count);

    /* Find the voicegroup */
    vg_log("voicegroup_load: searching for voicegroup '%s'", voicegroupName);
    VoicegroupLocation loc = find_voicegroup(projectRoot, voicegroupName, disc);
    if (!loc.found) {
        vg_log("voicegroup_load: voicegroup '%s' not found", voicegroupName);
        fprintf(stderr, "voicegroup_loader: cannot find voicegroup '%s'\n", voicegroupName);
        goto fail;
    }
    vg_log("voicegroup_load: found at '%s' label='%s'", loc.filePath, loc.label);

    /* Parse the voicegroup */
    const char *startLabel = loc.label[0] ? loc.label : NULL;
    vg_log("voicegroup_load: parsing voicegroup file");
    if (parse_voicegroup_file(projectRoot, loc.filePath, startLabel,
                               vg, &dsMap, &pwMap, &ksMap, disc, &waveCache) != 0) {
        vg_log("voicegroup_load: parse_voicegroup_file failed");
        goto fail;
    }
    vg_log("voicegroup_load: done OK");

    symbol_map_free(&dsMap);
    symbol_map_free(&pwMap);
    keysplit_map_free(&ksMap);
    free(disc);
    return vg;

fail:
    symbol_map_free(&dsMap);
    symbol_map_free(&pwMap);
    keysplit_map_free(&ksMap);
    free(disc);
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
