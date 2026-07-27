#include "voicegroup_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

/* MSVC's sys/stat.h lacks the POSIX S_IS* predicate macros. */
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#ifdef _WIN32
/*
 * Minimal dirent shim over FindFirstFile.  Only d_name and d_type are used in
 * this file, and directory iteration is strictly sequential (never
 * concurrent), so a single static dirent entry per stream is sufficient.
 */

/* POSIX d_type values the loader uses; define for the shim. */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK     10

struct dirent {
    char d_name[MAX_PATH];
    unsigned char d_type;
};

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA findData;
    int first;
    struct dirent entry;
} DIR;

static DIR *opendir(const char *path)
{
    char pattern[MAX_PATH];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", path) >= (int)sizeof(pattern))
        return NULL;
    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) return NULL;
    d->handle = FindFirstFileExA(pattern, FindExInfoBasic, &d->findData,
                                 FindExSearchNameMatch, NULL,
                                 FIND_FIRST_EX_LARGE_FETCH);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static struct dirent *readdir(DIR *d)
{
    if (d->first)
        d->first = 0;
    else if (!FindNextFileA(d->handle, &d->findData))
        return NULL;
    snprintf(d->entry.d_name, sizeof(d->entry.d_name), "%s", d->findData.cFileName);
    DWORD attrs = d->findData.dwFileAttributes;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        d->entry.d_type = DT_LNK;      /* forces the stat() fallback below */
    else if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        d->entry.d_type = DT_DIR;
    else
        d->entry.d_type = DT_REG;
    return &d->entry;
}

static int closedir(DIR *d)
{
    FindClose(d->handle);
    free(d);
    return 0;
}
#endif /* _WIN32 */

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
    /* Lazy deep scan (see discovery_ensure_deep_scan) */
    char projectRoot[MAX_PATH_LEN];
    const VoicegroupLoaderConfig *cfg;   /* borrowed; valid for the load call */
    int deepScanned;
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
    /* Inline Golden Sun synth definition (set_synth_* macros) instead of a
     * sample file.  synthDesc holds the 6 descriptor bytes that follow a
     * zero-size WaveData header (0x80, type, then 4 pulse parameters). */
    uint8_t isSynth;
    uint8_t synthDesc[6];
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
    /* How many ProjectDiscovery keySplitTableFiles entries have been parsed
     * into this map; the lazy deep scan appends files past this index. */
    int parsedFileCount;
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
static WaveData *load_aif_from_path(const char *absoluteAifPath);
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
                                  KeySplitMap *ksMap,
                                  ProjectDiscovery *disc,
                                  WaveCache *waveCache,
                                  int startIndex, int contiguousFill, int noSubRecurse);

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

/* Helper: build a path. A path that doesn't fit would name the wrong file
 * if truncated, so it becomes an empty (inert) string instead. */
static void build_path(char *dest, size_t destSize, const char *base, const char *relative)
{
    size_t baseLen = strlen(base);
    size_t relLen = strlen(relative);
    if (baseLen + 1 + relLen >= destSize) {
        dest[0] = '\0';
        return;
    }
    memcpy(dest, base, baseLen);
    dest[baseLen] = PATH_SEP;
    memcpy(dest + baseLen + 1, relative, relLen + 1);
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

/*
 * Entry-type check that avoids a stat() when readdir already told us.
 * DT_UNKNOWN/DT_LNK (possible on Linux for some filesystems, and for
 * symlinks/reparse points everywhere) fall back to stat(), which follows
 * links -- matching the previous behavior exactly.
 */
static int dirent_is_dir(const char *parentPath, const struct dirent *ent)
{
    if (ent->d_type == DT_DIR) return 1;
    if (ent->d_type == DT_REG) return 0;
    char p[MAX_PATH_LEN];
    build_path(p, sizeof(p), parentPath, ent->d_name);
    return is_directory(p);
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
    memset(&map->entries[map->count], 0, sizeof(SymbolMapping));
    strncpy(map->entries[map->count].symbol, symbol, MAX_SYMBOL_LEN - 1);
    map->entries[map->count].symbol[MAX_SYMBOL_LEN - 1] = '\0';
    strncpy(map->entries[map->count].filePath, path, MAX_PATH_LEN - 1);
    map->entries[map->count].filePath[MAX_PATH_LEN - 1] = '\0';
    map->count++;
}

static void symbol_map_add_synth(SymbolMap *map, const char *symbol,
                                 const uint8_t desc[6])
{
    symbol_map_add(map, symbol, "");
    map->entries[map->count - 1].isSynth = 1;
    memcpy(map->entries[map->count - 1].synthDesc, desc, 6);
}

/* Returns the file path for a symbol, or NULL if unknown.  Inline synth
 * entries have no file and are deliberately not returned here; use
 * symbol_map_find_synth for those. */
static const char *symbol_map_find(const SymbolMap *map, const char *symbol)
{
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].symbol, symbol) == 0)
            return map->entries[i].isSynth ? NULL : map->entries[i].filePath;
    }
    return NULL;
}

/* Returns the 6 synth descriptor bytes for an inline synth symbol, or NULL. */
static const uint8_t *symbol_map_find_synth(const SymbolMap *map, const char *symbol)
{
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].symbol, symbol) == 0)
            return map->entries[i].isSynth ? map->entries[i].synthDesc : NULL;
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
    map->parsedFileCount = 0;
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
 * Check the first 50 lines of a file for voice macro keywords
 * (voice_directsound, voice_square, voice_keysplit, etc.).
 */
static int file_has_voice_macros(const char *filePath)
{
    FILE *f = fopen(filePath, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    int lineCount = 0;
    while (fgets(line, sizeof(line), f) && lineCount < 50) {
        if (strstr(line, "voice_directsound") || strstr(line, "voice_square") ||
            strstr(line, "voice_programmable_wave") || strstr(line, "voice_noise") ||
            strstr(line, "voice_keysplit") || strstr(line, "voice_group")) {
            fclose(f);
            return 1;
        }
        lineCount++;
    }
    fclose(f);
    return 0;
}

/*
 * Probe for keysplit-table data adjacent to a voicegroup-style directory.
 * Adds <dir>/keysplit_tables.{s,inc} and any .s/.inc files inside <dir>/keysplits/
 * to the keySplitTableFiles list. Safe to call repeatedly (pathlist_add dedups).
 *
 * This supports project layouts (e.g. the eventide pokeemerald fork) that keep
 * keysplit tables next to their voicegroups rather than in sound/keysplit_tables.inc.
 * It is purely additive for standard pokeemerald/pokefirered layouts: if those
 * files/dirs don't exist it is a no-op, and files in a keysplits/ subdir that turn
 * out to be sub-voicegroup definitions rather than keysplit tables are harmless --
 * parse_keysplit_tables_file only acts on lines beginning with "keysplit ".
 */
static void probe_keysplit_data_in_dir(const char *dirPath, ProjectDiscovery *out)
{
    char p[MAX_PATH_LEN];

    build_path(p, sizeof(p), dirPath, "keysplit_tables.inc");
    if (file_exists(p))
        pathlist_add(&out->keySplitTableFiles, p);
    build_path(p, sizeof(p), dirPath, "keysplit_tables.s");
    if (file_exists(p))
        pathlist_add(&out->keySplitTableFiles, p);

    char ksDir[MAX_PATH_LEN];
    build_path(ksDir, sizeof(ksDir), dirPath, "keysplits");
    if (is_directory(ksDir)) {
        DIR *d = opendir(ksDir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                if (!str_ends_with_ci(ent->d_name, ".s") &&
                    !str_ends_with_ci(ent->d_name, ".inc"))
                    continue;
                char fp[MAX_PATH_LEN];
                build_path(fp, sizeof(fp), ksDir, ent->d_name);
                pathlist_add(&out->keySplitTableFiles, fp);
            }
            closedir(d);
        }
    }
}

/* Facts about one directory, collected in a single readdir() pass. */
#define MAX_DIRENT_NAME 260

typedef struct {
    int hasWavOrAif;
    int hasKeysplitTablesInc;   /* entry named exactly "keysplit_tables.inc" */
    int hasKeysplitTablesS;     /* entry named exactly "keysplit_tables.s"  */
    int hasKeysplitsSubdir;     /* directory entry named exactly "keysplits" */
    char macroCandidates[5][MAX_DIRENT_NAME];
    int macroCandidateCount;    /* first 5 .inc/.s files, readdir order */
    char (*subdirs)[MAX_DIRENT_NAME];   /* heap; readdir order */
    int subdirCount, subdirCapacity;
} DirFacts;

/*
 * Recursively discover voicegroup dirs, wav sample dirs, and keysplit table
 * files under dirPath, enumerating each directory exactly once: collect all
 * facts in a single readdir() pass, apply them (parent's adds before its
 * children's, so PathList ordering matches the old multi-pass scan), then
 * recurse into subdirectories in readdir order.
 */
static void discover_scan_tree(const char *dirPath, int depth, int maxDepth,
                               ProjectDiscovery *out)
{
    DirFacts facts;
    memset(&facts, 0, sizeof(facts));

    DIR *d = opendir(dirPath);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (dirent_is_dir(dirPath, ent)) {
                if (strcmp(ent->d_name, "keysplits") == 0)
                    facts.hasKeysplitsSubdir = 1;
                if (facts.subdirCount >= facts.subdirCapacity) {
                    facts.subdirCapacity = facts.subdirCapacity ? facts.subdirCapacity * 2 : INITIAL_CAPACITY;
                    facts.subdirs = realloc(facts.subdirs, sizeof(*facts.subdirs) * facts.subdirCapacity);
                }
                snprintf(facts.subdirs[facts.subdirCount], sizeof(facts.subdirs[0]), "%s", ent->d_name);
                facts.subdirCount++;
            } else {
                if (str_ends_with_ci(ent->d_name, ".wav") || str_ends_with_ci(ent->d_name, ".aif"))
                    facts.hasWavOrAif = 1;
                if (strcmp(ent->d_name, "keysplit_tables.inc") == 0)
                    facts.hasKeysplitTablesInc = 1;
                else if (strcmp(ent->d_name, "keysplit_tables.s") == 0)
                    facts.hasKeysplitTablesS = 1;
                if (facts.macroCandidateCount < 5 &&
                    (str_ends_with_ci(ent->d_name, ".inc") || str_ends_with_ci(ent->d_name, ".s"))) {
                    snprintf(facts.macroCandidates[facts.macroCandidateCount],
                             sizeof(facts.macroCandidates[0]), "%s", ent->d_name);
                    facts.macroCandidateCount++;
                }
            }
        }
        closedir(d);
    }

    char p[MAX_PATH_LEN];

    for (int i = 0; i < facts.macroCandidateCount; i++) {
        build_path(p, sizeof(p), dirPath, facts.macroCandidates[i]);
        if (file_has_voice_macros(p)) {
            pathlist_add(&out->voicegroupDirs, dirPath);
            break;
        }
    }

    if (facts.hasWavOrAif)
        pathlist_add(&out->wavSampleDirs, dirPath);

    /* The directory listing already proved these exist; no stat() probes. */
    if (facts.hasKeysplitTablesInc) {
        build_path(p, sizeof(p), dirPath, "keysplit_tables.inc");
        pathlist_add(&out->keySplitTableFiles, p);
    }
    if (facts.hasKeysplitTablesS) {
        build_path(p, sizeof(p), dirPath, "keysplit_tables.s");
        pathlist_add(&out->keySplitTableFiles, p);
    }

    if (facts.hasKeysplitsSubdir) {
        /* Enumerate <dirPath>/keysplits here even though the recursion below
         * may also visit it: a keysplits/ sitting at depth maxDepth + 1 is out
         * of the recursion's reach, and pathlist_add dedups the overlap when
         * it isn't. */
        char ksDir[MAX_PATH_LEN];
        build_path(ksDir, sizeof(ksDir), dirPath, "keysplits");
        DIR *ks = opendir(ksDir);
        if (ks) {
            struct dirent *ent;
            while ((ent = readdir(ks)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                if (!str_ends_with_ci(ent->d_name, ".s") &&
                    !str_ends_with_ci(ent->d_name, ".inc"))
                    continue;
                build_path(p, sizeof(p), ksDir, ent->d_name);
                pathlist_add(&out->keySplitTableFiles, p);
            }
            closedir(ks);
        }
    }

    if (depth < maxDepth) {
        for (int i = 0; i < facts.subdirCount; i++) {
            char subPath[MAX_PATH_LEN];
            build_path(subPath, sizeof(subPath), dirPath, facts.subdirs[i]);
            discover_scan_tree(subPath, depth + 1, maxDepth, out);
        }
    }
    free(facts.subdirs);
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
    snprintf(out->projectRoot, sizeof(out->projectRoot), "%s", projectRoot);
    out->cfg = cfg;

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
                            build_path(fpath, sizeof(fpath), path, ent->d_name);
                            if (is_monolithic_voicegroup_file(fpath))
                                pathlist_add(&out->monolithicVGFiles, fpath);
                        }
                    }
                    closedir(d);
                }
                probe_keysplit_data_in_dir(path, out);
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

    /* Inline Golden Sun synth definitions (pokeemerald-expansion layout);
     * parsed by the same direct_sound_data parser, which recognizes the
     * set_synth_* macros. */
    build_path(path, sizeof(path), projectRoot, "sound/direct_sound_synth_data.inc");
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
        build_path(subPath, sizeof(subPath), path, "keysplits");
        if (is_directory(subPath))
            pathlist_add(&out->voicegroupDirs, subPath);
        build_path(subPath, sizeof(subPath), path, "drumsets");
        if (is_directory(subPath))
            pathlist_add(&out->voicegroupDirs, subPath);
    }

    /* 4. The recursive scan under sound/ for voicegroup dirs and wav dirs is
     * deferred to discovery_ensure_deep_scan(): it runs only when a lookup
     * misses the eager entries above (stock layouts never need it). */

    /* 5. Check for monolithic voicegroup files */
    build_path(path, sizeof(path), projectRoot, "sound/voice_groups.inc");
    vg_log("discover_project: checking monolithic '%s' exists=%d", path, file_exists(path));
    if (file_exists(path) && is_monolithic_voicegroup_file(path))
        pathlist_add(&out->monolithicVGFiles, path);
}

/*
 * Run discover_project's deferred recursive sound/ scan (step 4), at most
 * once per discovery. The scan exists only to support nonstandard project
 * layouts (e.g. the eventide fork); on stock projects every lookup is
 * satisfied by the eager entries and this never runs. Deferral preserves
 * behavior: the scan appends to PathLists that already hold the eager
 * entries, and every consumer iterates them in order, first-hit-wins, so
 * eager hits resolve identically with or without the scanned tail.
 *
 * NOTE: this is lazy work within a single voicegroup_load(_samples) call,
 * not cross-call caching — disc still lives and dies with the call.
 */
static void discovery_ensure_deep_scan(ProjectDiscovery *disc)
{
    if (disc->deepScanned) return;
    disc->deepScanned = 1;
    vg_log("discovery: deep scan triggered");
    char soundDir[MAX_PATH_LEN];
    build_path(soundDir, sizeof(soundDir), disc->projectRoot, "sound");
    if (is_directory(soundDir))
        discover_scan_tree(soundDir, 0, 3, disc);
    vg_log("discovery: deep scan done, vgDirs=%d wavDirs=%d",
           disc->voicegroupDirs.count, disc->wavSampleDirs.count);
}

/* ---- Symbol data file parsing (parameterized by file path) ---- */

/*
 * Parse a "set_synth_*" macro line into a Golden Sun synth descriptor (the 6
 * bytes that follow a zero-size WaveData header).  Returns 1 and fills desc
 * on success, 0 if the line is not a synth macro.  Recognized macros, with
 * the pokeemerald-expansion names and the preferred aliases:
 *   set_synth_custom p1, p2, p3, p4   /  set_synth_pulse p1, p2, p3, p4
 *   set_synth_25                      /  set_synth_saw
 *   set_synth_50                      /  set_synth_triangle
 * For the pulse macros: p1 = base duty cycle, p2 = duty LFO step per frame,
 * p3 = modulation amount, p4 = duty LFO phase offset (0x0-0xFF each).
 */
static int parse_synth_macro_line(const char *trimmed, uint8_t desc[6])
{
    static const struct { const char *name; uint8_t type; int hasParams; } kMacros[] = {
        { "set_synth_custom",   0, 1 },
        { "set_synth_pulse",    0, 1 },
        { "set_synth_25",       1, 0 },
        { "set_synth_saw",      1, 0 },
        { "set_synth_50",       2, 0 },
        { "set_synth_triangle", 2, 0 },
    };

    for (size_t m = 0; m < sizeof(kMacros) / sizeof(kMacros[0]); m++) {
        size_t len = strlen(kMacros[m].name);
        if (strncmp(trimmed, kMacros[m].name, len) != 0)
            continue;
        char next = trimmed[len];
        if (next != '\0' && next != ' ' && next != '\t')
            continue;  /* avoid e.g. "set_synth_50" matching "set_synth_5" */

        memset(desc, 0, 6);
        desc[0] = 0x80;
        desc[1] = kMacros[m].type;
        if (kMacros[m].hasParams) {
            const char *p = trimmed + len;
            for (int n = 0; n < 4; n++) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                desc[2 + n] = (uint8_t)strtoul(p, (char **)&p, 0);
            }
        }
        return 1;
    }
    return 0;
}

/*
 * If a (trimmed) line begins with a "Name::" or "Name:" label, copy the name
 * into out and return 1.  GAS accepts both forms — a single colon merely
 * makes the symbol file-local, which still assembles and links because
 * sample data lives in the same assembly unit as the voicegroups that
 * reference it.  Voicegroup labels elsewhere stay strictly "::": songs
 * reference those from separate assembly units.
 */
static int parse_sample_label(const char *trimmed, char *out, size_t outSize)
{
    size_t nameLen = strspn(trimmed, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                     "abcdefghijklmnopqrstuvwxyz0123456789_");
    if (nameLen == 0 || trimmed[nameLen] != ':')
        return 0;
    if (nameLen >= outSize)
        nameLen = outSize - 1;
    memcpy(out, trimmed, nameLen);
    out[nameLen] = '\0';
    return 1;
}

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

        /* Look for "label::" / "label:" lines */
        if (parse_sample_label(trimmed, currentSymbol, MAX_SYMBOL_LEN))
            continue;

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
        /* Inline Golden Sun synth definitions (set_synth_* macros) */
        else if (currentSymbol[0]) {
            uint8_t desc[6];
            if (parse_synth_macro_line(trimmed, desc)) {
                symbol_map_add_synth(map, currentSymbol, desc);
                currentSymbol[0] = '\0';
            }
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

        if (parse_sample_label(trimmed, currentSymbol, MAX_SYMBOL_LEN))
            continue;

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

static void parse_keysplit_tables_range(const ProjectDiscovery *disc, KeySplitMap *map, int fromIndex)
{
    for (int i = fromIndex; i < disc->keySplitTableFiles.count; i++) {
        parse_keysplit_tables_file(disc->keySplitTableFiles.paths[i], map);
    }
    map->parsedFileCount = disc->keySplitTableFiles.count;
}

static void parse_all_keysplit_tables(const ProjectDiscovery *disc, KeySplitMap *map)
{
    parse_keysplit_tables_range(disc, map, 0);
}

/*
 * keysplit_map_find with a lazy-deep-scan miss hook: on a miss, run the
 * deferred sound/ scan (if it hasn't run yet) and parse only the keysplit
 * table files it appended, then look again. Precedence is preserved:
 * eagerly-discovered files were parsed first and keysplit_map_find returns
 * the first match, same as when discovery was fully eager. The parsedFileCount
 * check (not just deepScanned) matters when another lookup already triggered
 * the scan: the appended table files still need parsing into this map.
 */
static KeySplitDef *keysplit_map_find_or_rescan(KeySplitMap *map, const char *name,
                                                ProjectDiscovery *disc)
{
    KeySplitDef *ks = keysplit_map_find(map, name);
    if (ks || !disc)
        return ks;
    if (disc->deepScanned && map->parsedFileCount >= disc->keySplitTableFiles.count)
        return NULL;
    discovery_ensure_deep_scan(disc);
    parse_keysplit_tables_range(disc, map, map->parsedFileCount);
    return keysplit_map_find(map, name);
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

/* 80-bit IEEE 754 extended float, big-endian (AIFF COMM sampleRate). */
static double read_extended80(const uint8_t *b)
{
    int sign = (b[0] & 0x80) ? -1 : 1;
    int exponent = ((b[0] & 0x7F) << 8) | b[1];
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; i++)
        mantissa = (mantissa << 8) | b[2 + i];
    if (exponent == 0 && mantissa == 0)
        return 0.0;
    /* Explicit integer bit: value = mantissa * 2^(exponent - 16383 - 63). */
    return sign * ldexp((double)mantissa, exponent - 16383 - 63);
}

/*
 * Load an .aif (AIFF) file from an absolute path — the sample source format
 * of projects that predate pret's .wav conversion. Mirrors aif2pcm so the
 * result matches loading the .bin it would generate: freq = COMM rate * 1024
 * (the INST base note is ignored, as aif2pcm ignores it), loopStart / size
 * from the INST sustain-loop MARK positions (size = loop-end position, else
 * COMM frame count, minus one), 8-bit data used as-is (AIFF PCM is signed),
 * 16-bit big-endian data reduced to its high byte.
 */
static WaveData *load_aif_from_path(const char *absoluteAifPath)
{
    FILE *f = fopen(absoluteAifPath, "rb");
    if (!f) return NULL;

    uint8_t formHdr[12];
    if (fread(formHdr, 1, 12, f) != 12 ||
        memcmp(formHdr, "FORM", 4) != 0 ||
        memcmp(formHdr + 8, "AIFF", 4) != 0) {
        fclose(f);
        fprintf(stderr, "voicegroup_loader: invalid FORM/AIFF header in %s\n", absoluteAifPath);
        return NULL;
    }

    int commFound = 0, ssndFound = 0;
    uint32_t numFrames = 0;
    int sampleSize = 0;
    double sampleRate = 0.0;

    /* INST sustain loop: marker ids resolved against the MARK chunk. */
    int haveSustainLoop = 0;
    uint16_t loopStartId = 0, loopEndId = 0;

    struct { uint16_t id; uint32_t position; } *markers = NULL;
    uint16_t numMarkers = 0;

    long ssndDataOffset = 0;
    uint32_t ssndDataBytes = 0;

    while (1) {
        uint8_t chunkHdr[8];
        if (fread(chunkHdr, 1, 8, f) != 8)
            break;
        uint32_t chunkLen = ((uint32_t)chunkHdr[4] << 24) | ((uint32_t)chunkHdr[5] << 16) |
                            ((uint32_t)chunkHdr[6] << 8) | chunkHdr[7];
        long chunkDataStart = ftell(f);

        if (memcmp(chunkHdr, "COMM", 4) == 0 && chunkLen >= 18) {
            uint8_t d[18];
            if (fread(d, 1, 18, f) == 18) {
                int numChannels = (d[0] << 8) | d[1];
                numFrames = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) |
                            ((uint32_t)d[4] << 8) | d[5];
                sampleSize = (d[6] << 8) | d[7];
                sampleRate = read_extended80(d + 8);
                if (numChannels != 1) {
                    fprintf(stderr, "voicegroup_loader: %s has %d channels, must be mono\n",
                            absoluteAifPath, numChannels);
                    free(markers);
                    fclose(f);
                    return NULL;
                }
                if (sampleSize != 8 && sampleSize != 16) {
                    fprintf(stderr, "voicegroup_loader: unsupported AIFF sample size %d in %s\n",
                            sampleSize, absoluteAifPath);
                    free(markers);
                    fclose(f);
                    return NULL;
                }
                commFound = 1;
            }
        } else if (memcmp(chunkHdr, "MARK", 4) == 0 && chunkLen >= 2 && !markers) {
            uint8_t d[6];
            if (fread(d, 1, 2, f) == 2) {
                numMarkers = (uint16_t)((d[0] << 8) | d[1]);
                if (numMarkers > 0)
                    markers = calloc(numMarkers, sizeof(*markers));
                for (uint16_t i = 0; markers && i < numMarkers; i++) {
                    if (fread(d, 1, 6, f) != 6)
                        break;
                    markers[i].id = (uint16_t)((d[0] << 8) | d[1]);
                    markers[i].position = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) |
                                          ((uint32_t)d[4] << 8) | d[5];
                    /* Pascal-style name, padded so the record length is even. */
                    int nameSize;
                    if ((nameSize = fgetc(f)) == EOF)
                        break;
                    if (fseek(f, nameSize + !(nameSize & 1), SEEK_CUR) != 0)
                        break;
                }
            }
        } else if (memcmp(chunkHdr, "INST", 4) == 0 && chunkLen >= 20) {
            uint8_t d[20];
            if (fread(d, 1, 20, f) == 20) {
                /* d[0] base note, d[1..7] detune/keys/velocities/gain. */
                int loopType = (d[8] << 8) | d[9];
                if (loopType) {
                    loopStartId = (uint16_t)((d[10] << 8) | d[11]);
                    loopEndId = (uint16_t)((d[12] << 8) | d[13]);
                    haveSustainLoop = 1;
                }
                /* d[14..19] release loop, unused. */
            }
        } else if (memcmp(chunkHdr, "SSND", 4) == 0 && chunkLen >= 8) {
            /* Skip offset and blockSize. */
            ssndDataOffset = chunkDataStart + 8;
            ssndDataBytes = chunkLen - 8;
            ssndFound = 1;
        }

        long nextChunk = chunkDataStart + (long)chunkLen;
        if (chunkLen & 1) nextChunk++;
        if (fseek(f, nextChunk, SEEK_SET) != 0)
            break;
    }

    if (!commFound || !ssndFound) {
        fprintf(stderr, "voicegroup_loader: missing COMM or SSND chunk in %s\n", absoluteAifPath);
        free(markers);
        fclose(f);
        return NULL;
    }

    /* Resolve the loop markers exactly as aif2pcm does: the end marker's
     * position bounds the sample, and the smaller of the two positions is
     * the loop start (guards marker-order mistakes in hand-made files). */
    int loopEnabled = 0;
    uint32_t loopStart = 0;
    uint32_t numSamples = numFrames;
    if (haveSustainLoop) {
        for (uint16_t i = 0; i < numMarkers; i++) {
            if (markers[i].id == loopStartId) {
                loopStart = markers[i].position;
                loopEnabled = 1;
                break;
            }
        }
        for (uint16_t i = 0; i < numMarkers; i++) {
            if (markers[i].id == loopEndId) {
                if (markers[i].position < loopStart || !loopEnabled) {
                    loopStart = markers[i].position;
                    loopEnabled = 1;
                }
                numSamples = markers[i].position;
                break;
            }
        }
    }
    free(markers);

    uint32_t size = (numSamples > 0) ? numSamples - 1 : 0;
    uint32_t bytesPerSample = (sampleSize == 16) ? 2 : 1;
    uint32_t availableSamples = ssndDataBytes / bytesPerSample;
    if (size > availableSamples)
        size = availableSamples;

    WaveData *wd = malloc(sizeof(WaveData) + (size_t)size + 1);
    if (!wd) {
        fclose(f);
        return NULL;
    }
    wd->type      = 0;
    wd->status    = loopEnabled ? 0x4000 : 0;
    wd->freq      = (uint32_t)(sampleRate * 1024.0);
    wd->loopStart = loopStart;
    wd->size      = size;
    wd->data      = (int8_t *)((uint8_t *)wd + sizeof(WaveData));

    size_t rawBytes = (size_t)size * bytesPerSample;
    if (rawBytes > 0) {
        uint8_t *rawData = malloc(rawBytes);
        if (!rawData) {
            free(wd);
            fclose(f);
            return NULL;
        }
        size_t bytesRead = 0;
        if (fseek(f, ssndDataOffset, SEEK_SET) == 0)
            bytesRead = fread(rawData, 1, rawBytes, f);
        if (bytesRead < rawBytes)
            memset(rawData + bytesRead, 0, rawBytes - bytesRead);
        if (bytesPerSample == 1) {
            memcpy(wd->data, rawData, size);
        } else {
            for (uint32_t i = 0; i < size; i++)
                wd->data[i] = (int8_t)rawData[(size_t)i * 2]; /* big-endian high byte */
        }
        free(rawData);
    }
    fclose(f);

    wd->data[size] = (size > 0) ? wd->data[size - 1] : 0;
    return wd;
}

/*
 * Load a PCM instrument sample from a source audio file.
 * Derives the .wav then .aif path by replacing the .bin extension in
 * relativeBinPath. Falls back to load_wave_data() if neither is found.
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

    ext[1] = 'a'; ext[2] = 'i'; ext[3] = 'f';
    build_path(fullPath, sizeof(fullPath), projectRoot, relativeWavPath);
    wd = load_aif_from_path(fullPath);
    if (wd) return wd;

    /* Neither source format found — fall back to the .bin build artifact */
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

    /* A zero-length sample is a Golden Sun synth-instrument descriptor
     * (ipatix improved-mixer feature): the bytes after the header select the
     * waveform and pulse parameters instead of holding PCM data.  Keep up to
     * 16 descriptor bytes so the engine can read them. */
    uint32_t dataSize = (size > 0) ? size : 16;

    WaveData *wd = malloc(sizeof(WaveData) + dataSize + 1);
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

    size_t bytesRead = fread(wd->data, 1, dataSize, f);
    if (bytesRead < dataSize) {
        memset(wd->data + bytesRead, 0, dataSize - bytesRead);
    }
    wd->data[dataSize] = wd->data[dataSize - 1];

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
 * Unified sample resolution: try symbol map first, then fallback to wav dirs.
 * Uses waveCache to avoid loading the same file more than once.
 * Registers newly loaded WaveData with vg; cache hits are NOT re-registered.
 */
static WaveData *resolve_and_load_sample(const char *projectRoot, const char *symbol,
                                          const SymbolMap *dsMap, ProjectDiscovery *disc,
                                          LoadedVoiceGroup *vg, WaveCache *waveCache)
{
    /* Inline Golden Sun synth definition (set_synth_* macros)?  Build the
     * WaveData directly: the standard synth header (loop flag, freq
     * 0x01058920 so the 64-sample wave period lands on middle C, size 0)
     * followed by the descriptor bytes -- byte-identical to what the macros
     * assemble on the GBA and to the equivalent .bin sample files. */
    const uint8_t *synthDesc = symbol_map_find_synth(dsMap, symbol);
    if (synthDesc) {
        char cacheKey[MAX_PATH_LEN];
        snprintf(cacheKey, sizeof(cacheKey), "synth-macro:%s", symbol);
        WaveData *cached = wave_cache_find(waveCache, cacheKey);
        if (cached) return cached;

        uint32_t dataSize = 16;
        WaveData *wd = calloc(1, sizeof(WaveData) + dataSize + 1);
        if (!wd) return NULL;
        wd->type = 0;
        wd->status = 0x4000;
        wd->freq = 0x01058920;
        wd->loopStart = 0;
        wd->size = 0;
        wd->data = (int8_t *)((uint8_t *)wd + sizeof(WaveData));
        memcpy(wd->data, synthDesc, 6);
        vg_register_wavedata(vg, wd);
        wave_cache_insert(waveCache, cacheKey, wd);
        return wd;
    }

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
    /* Fallback: search sample directories (.wav, then .aif). Only the deep
     * scan populates wavSampleDirs beyond config overrides, so a lookup that
     * reaches this point needs it to have run. On stock projects every
     * symbol resolves via dsMap above and the scan never fires. */
    if (disc) {
        if (!disc->deepScanned)
            discovery_ensure_deep_scan(disc);
        for (int i = 0; i < disc->wavSampleDirs.count; i++) {
            for (int fmt = 0; fmt < 2; fmt++) {
                char wavPath[MAX_PATH_LEN];
                snprintf(wavPath, sizeof(wavPath), "%s%c%s.%s",
                         disc->wavSampleDirs.paths[i], PATH_SEP, symbol,
                         fmt == 0 ? "wav" : "aif");
                WaveData *cached = wave_cache_find(waveCache, wavPath);
                if (cached) return cached;
                WaveData *wd = fmt == 0 ? load_wav_from_path(wavPath)
                                        : load_aif_from_path(wavPath);
                if (wd) {
                    vg_register_wavedata(vg, wd);
                    wave_cache_insert(waveCache, wavPath, wd);
                    return wd;
                }
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
 * Search for a voicegroup by name across all currently discovered locations.
 */
static VoicegroupLocation find_voicegroup_probe(const char *projectRoot,
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
            /* The drumset file keeps whatever follows "_drumset" (e.g.
             * voicegroup_emerald_drumset_1 -> drumsets/emerald_1.inc, and
             * voicegroup_frlg_drumset -> drumsets/frlg.inc), so splice the
             * "_drumset" infix out rather than truncating the name at it. */
            const char *tail = suffix + 8; /* strlen("_drumset") */
            if (baseLen > 0 && baseLen + (int)strlen(tail) < MAX_SYMBOL_LEN) {
                memcpy(baseName, vgName, baseLen);
                strcpy(baseName + baseLen, tail);
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

/*
 * Search for a voicegroup by name; on a miss, run the deferred deep scan
 * (which can add voicegroup dirs from nonstandard layouts) and probe again.
 */
static VoicegroupLocation find_voicegroup(const char *projectRoot,
                                          const char *vgName,
                                          ProjectDiscovery *disc)
{
    VoicegroupLocation loc = find_voicegroup_probe(projectRoot, vgName, disc);
    if (!loc.found && !disc->deepScanned) {
        discovery_ensure_deep_scan(disc);
        loc = find_voicegroup_probe(projectRoot, vgName, disc);
    }
    return loc;
}

/* ---- Voicegroup parsing ---- */

/* Helper: last path component (handles both separators). */
static const char *path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    return base;
}

/*
 * ROM-contiguity successor of a per-file voicegroup: voicegroups are
 * assembled back to back in voice_groups.inc include order, so indexing past
 * one group's end reads the next included group's entries on the GBA.
 * Finds the .include line matching currentFilePath's basename and returns
 * (in outPath) the absolute path of the next included file that exists.
 */
static int next_included_voicegroup(const char *projectRoot, const char *currentFilePath,
                                    char *outPath, size_t outSize)
{
    static const char *indexFiles[] = {"sound/voice_groups.inc", "sound/voicegroups.inc"};
    const char *curBase = path_basename(currentFilePath);

    for (size_t i = 0; i < sizeof(indexFiles) / sizeof(indexFiles[0]); i++) {
        char indexPath[MAX_PATH_LEN];
        build_path(indexPath, sizeof(indexPath), projectRoot, indexFiles[i]);
        FILE *f = fopen(indexPath, "r");
        if (!f) continue;

        char line[MAX_LINE];
        int foundCurrent = 0;
        while (fgets(line, sizeof(line), f)) {
            strip_comment(line);
            char *trimmed = ltrim(line);
            char incPath[MAX_PATH_LEN];
            if (sscanf(trimmed, ".include \"%511[^\"]\"", incPath) != 1)
                continue;
            if (foundCurrent) {
                char absPath[MAX_PATH_LEN];
                build_path(absPath, sizeof(absPath), projectRoot, incPath);
                if (file_exists(absPath)) {
                    strncpy(outPath, absPath, outSize - 1);
                    outPath[outSize - 1] = '\0';
                    fclose(f);
                    return 1;
                }
                /* Included file missing on disk: contiguity is unknowable */
                break;
            }
            if (strcmp(path_basename(incPath), curBase) == 0)
                foundCurrent = 1;
        }
        fclose(f);
        if (foundCurrent)
            break; /* current file located in this index; don't try others */
    }
    return 0;
}

/*
 * Load a sub-voicegroup (for keysplit/keysplit_all references).
 */
static ToneData *load_sub_voicegroup(const char *projectRoot, const char *vgSymbol,
                                      LoadedVoiceGroup *vg,
                                      const SymbolMap *dsMap, const SymbolMap *pwMap,
                                      KeySplitMap *ksMap,
                                      ProjectDiscovery *disc,
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

    /* The parser writes into vg->voices/voiceNames, so save the caller's
     * top-level data around the sub-voicegroup parse.  Sub-voice names are
     * discarded; only top-level names are kept (heap-allocated: the two
     * arrays are ~30 KB, too big to risk on a plugin-load thread's stack). */
    ToneData *savedVoices = malloc(sizeof(vg->voices));
    char (*savedNames)[VG_VOICE_NAME_LEN] = malloc(sizeof(vg->voiceNames));
    if (!savedVoices || !savedNames) {
        free(savedVoices);
        free(savedNames);
        free(subVg);
        return NULL;
    }
    memcpy(savedVoices, vg->voices, sizeof(vg->voices));
    memcpy(savedNames, vg->voiceNames, sizeof(vg->voiceNames));
    memset(vg->voices, 0, sizeof(vg->voices));
    memset(vg->voiceNames, 0, sizeof(vg->voiceNames));

    /* contiguousFill: a sub-voicegroup shorter than 128 voices keeps going
     * into whatever is assembled after it on the GBA, and old-style drumsets
     * index into that overflow region deliberately. Within a monolithic file
     * the parse itself continues across labels; for per-file layouts, keep
     * filling from the following files in voice_groups.inc include order. */
    const char *startLabel = loc.label[0] ? loc.label : NULL;
    int endIndex = parse_voicegroup_file(projectRoot, loc.filePath, startLabel,
                                         vg, dsMap, pwMap, ksMap, disc, waveCache,
                                         0, 1, 0);
    if (endIndex > 0 && !startLabel) {
        char curPath[MAX_PATH_LEN];
        strncpy(curPath, loc.filePath, sizeof(curPath) - 1);
        curPath[sizeof(curPath) - 1] = '\0';
        for (int hops = 0; endIndex < VOICEGROUP_SIZE && hops < VOICEGROUP_SIZE; hops++) {
            char nextPath[MAX_PATH_LEN];
            if (!next_included_voicegroup(projectRoot, curPath, nextPath, sizeof(nextPath)))
                break;
            int r = parse_voicegroup_file(projectRoot, nextPath, NULL,
                                          vg, dsMap, pwMap, ksMap, disc, waveCache,
                                          endIndex, 0, 1);
            if (r <= endIndex)
                break; /* nothing gained: stop rather than spin */
            endIndex = r;
            strncpy(curPath, nextPath, sizeof(curPath) - 1);
            curPath[sizeof(curPath) - 1] = '\0';
        }
    }
    if (endIndex >= 0)
        memcpy(subVg, vg->voices, sizeof(ToneData) * VOICEGROUP_SIZE);
    memcpy(vg->voices, savedVoices, sizeof(vg->voices));
    memcpy(vg->voiceNames, savedNames, sizeof(vg->voiceNames));
    free(savedVoices);
    free(savedNames);
    if (endIndex < 0) {
        free(subVg);
        return NULL;
    }

    vg_register_subgroup(vg, subVg);
    return subVg;
}

/*
 * Store a friendly display name for a voice slot, derived from the symbol on
 * the voice's line.  Common symbol prefixes are stripped for readability
 * (e.g. "DirectSoundWaveData_sc88pro_trumpet" -> "sc88pro_trumpet").
 */
static void vg_set_voice_name(LoadedVoiceGroup *vg, int voiceIndex, const char *symbol)
{
    if (voiceIndex < 0 || voiceIndex >= VOICEGROUP_SIZE)
        return;
    static const char *prefixes[] = {
        "DirectSoundWaveData_", "ProgrammableWaveData_", "voicegroup_",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t len = strlen(prefixes[i]);
        if (strncmp(symbol, prefixes[i], len) == 0 && symbol[len] != '\0') {
            symbol += len;
            break;
        }
    }
    /* Deliberate truncation into the fixed-size display name */
    strncpy(vg->voiceNames[voiceIndex], symbol, VG_VOICE_NAME_LEN - 1);
    vg->voiceNames[voiceIndex][VG_VOICE_NAME_LEN - 1] = '\0';
}

/*
 * Parse a voicegroup file and populate the ToneData array.
 *
 * When startLabel is non-NULL, scanning starts at the "<startLabel>::" label
 * and stops when a new label or .align 2 is encountered (monolithic file mode).
 * When startLabel is NULL, the entire file is parsed (individual file mode).
 *
 * Slot numbering begins at startIndex; returns the slot index after the last
 * parsed voice, or -1 if the file can't be opened.
 *
 * contiguousFill emulates ROM layout for sub-voicegroups: instead of stopping
 * at the section end, parsing continues across label/.align boundaries so
 * indexing past a group's end reaches the neighbors assembled after it, as it
 * does on the GBA (old-style drumsets rely on this: e.g. a 29-voice
 * voicegroup001 whose kick at key 36 is really voicegroup002's slot 7).
 * Voices beyond the first boundary — and every voice when noSubRecurse is set
 * (used for cross-file continuation) — do not load nested sub-voicegroups:
 * the hardware never substitutes a keysplit twice, and not recursing there
 * keeps include-order cycles (group A's overflow reaching a keysplit back
 * into A) from looping forever.
 */
static int parse_voicegroup_file(const char *projectRoot, const char *filePath,
                                  const char *startLabel,
                                  LoadedVoiceGroup *vg,
                                  const SymbolMap *dsMap, const SymbolMap *pwMap,
                                  KeySplitMap *ksMap,
                                  ProjectDiscovery *disc,
                                  WaveCache *waveCache,
                                  int startIndex, int contiguousFill, int noSubRecurse)
{
    vg_log("parse_voicegroup_file: '%s' label='%s' start=%d", filePath,
           startLabel ? startLabel : "(none)", startIndex);
    FILE *f = fopen(filePath, "r");
    if (!f) {
        fprintf(stderr, "voicegroup_loader: cannot open %s\n", filePath);
        return -1;
    }

    char line[MAX_LINE];
    int voiceIndex = startIndex;
    int inSection = (startLabel == NULL); /* if no startLabel, parse from the beginning */
    int voicesParsedInSection = 0;
    int inContinuation = 0; /* past the section end under contiguousFill */

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
        if (startLabel && inSection && voicesParsedInSection > 0 && !inContinuation) {
            /* Check for a new label (word followed by ::) */
            char *cc = strstr(trimmed, "::");
            int boundary = (cc && cc > trimmed && !isspace((unsigned char)trimmed[0]))
                           || strncmp(trimmed, ".align", 6) == 0;
            if (boundary) {
                if (!contiguousFill)
                    break;
                /* ROM contiguity: keep filling from the next group's voices.
                 * The boundary line itself matches no macro and is skipped. */
                inContinuation = 1;
            }
        }

        /* Parse voice_group declaration for starting_note offset */
        if (strncmp(trimmed, "voice_group ", 12) == 0) {
            /* A comma-form declaration makes the label virtual (it points
             * before the file's data), so contiguity can't continue through
             * one — and its slot jump must not apply mid-fill. */
            if (inContinuation || noSubRecurse)
                break;
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
                vg_set_voice_name(vg, voiceIndex, sampleSymbol);
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
                vg_set_voice_name(vg, voiceIndex, sampleSymbol);
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
                vg_set_voice_name(vg, voiceIndex, sampleSymbol);
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
                vg_set_voice_name(vg, voiceIndex, waveSymbol);
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
                vg_set_voice_name(vg, voiceIndex, waveSymbol);
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
                vg_set_voice_name(vg, voiceIndex, vgSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_KEYSPLIT_ALL;

                if (!noSubRecurse && !inContinuation) {
                    ToneData *subVg = load_sub_voicegroup(projectRoot, vgSymbol,
                                                           vg, dsMap, pwMap, ksMap, disc, waveCache);
                    td->subGroup = subVg;
                }
            }
            voiceIndex++;
            voicesParsedInSection++;
        } else if (strncmp(trimmed, "voice_keysplit ", 15) == 0) {
            char vgSymbol[MAX_SYMBOL_LEN];
            char ksSymbol[MAX_SYMBOL_LEN];
            if (sscanf(trimmed + 15, "%[^,], %s", vgSymbol, ksSymbol) == 2) {
                rtrim(vgSymbol);
                rtrim(ksSymbol);
                vg_set_voice_name(vg, voiceIndex, vgSymbol);
                ToneData *td = &vg->voices[voiceIndex];
                td->type = VOICE_KEYSPLIT;

                if (!noSubRecurse && !inContinuation) {
                    ToneData *subVg = load_sub_voicegroup(projectRoot, vgSymbol,
                                                           vg, dsMap, pwMap, ksMap, disc, waveCache);
                    td->subGroup = subVg;
                }

                KeySplitDef *ksDef = keysplit_map_find_or_rescan(ksMap, ksSymbol, disc);
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
                vg_set_voice_name(vg, voiceIndex, sampleSymbol);
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
                vg_set_voice_name(vg, voiceIndex, sampleSymbol);
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
    return voiceIndex;
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
                               vg, &dsMap, &pwMap, &ksMap, disc, &waveCache,
                               0, 0, 0) < 0) {
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

LoadedSampleSet *voicegroup_load_samples(
    const char *projectRoot, const char *const *sampleSymbols, int sampleCount,
    const char *const *waveSymbols, int waveCount,
    const char *const *keysplitSymbols, const char *const *keysplitTableSymbols,
    int keysplitCount, const VoicegroupLoaderConfig *config)
{
    LoadedSampleSet *set = calloc(1, sizeof(LoadedSampleSet));
    if (!set) return NULL;
    set->container = calloc(1, sizeof(LoadedVoiceGroup));
    set->waves = calloc(sampleCount > 0 ? sampleCount : 1, sizeof(WaveData *));
    set->progWaves = calloc(waveCount > 0 ? waveCount : 1, sizeof(uint32_t *));
    set->keysplits =
        calloc(keysplitCount > 0 ? keysplitCount : 1, sizeof(LoadedKeysplit));
    ProjectDiscovery *disc = calloc(1, sizeof(ProjectDiscovery));
    if (!set->container || !set->waves || !set->progWaves || !set->keysplits
        || !disc) {
        free(disc);
        voicegroup_free_samples(set);
        return NULL;
    }
    set->count = sampleCount;
    set->progWaveCount = waveCount;
    set->keysplitCount = keysplitCount;

    vg_log("voicegroup_load_samples: start root='%s' samples=%d waves=%d keysplits=%d",
           projectRoot, sampleCount, waveCount, keysplitCount);
    discover_project(projectRoot, config, disc);

    /* The dedup cache caps at WAVE_CACHE_CAPACITY entries; past that, symbols
     * aliasing one file merely load their own copy (each registered in the
     * container, so cleanup stays correct). */
    WaveCache waveCache;
    wave_cache_init(&waveCache);

    SymbolMap dsMap, pwMap;
    KeySplitMap ksMap;
    symbol_map_init(&dsMap);
    symbol_map_init(&pwMap);
    keysplit_map_init(&ksMap);
    parse_all_direct_sound_data(disc, projectRoot, &dsMap);
    if (waveCount > 0 || keysplitCount > 0)
        parse_all_programmable_wave_data(disc, projectRoot, &pwMap);
    if (keysplitCount > 0)
        parse_all_keysplit_tables(disc, &ksMap);

    for (int i = 0; i < sampleCount; i++)
        set->waves[i] = resolve_and_load_sample(projectRoot, sampleSymbols[i],
                                                &dsMap, disc, set->container,
                                                &waveCache);

    for (int i = 0; i < waveCount; i++) {
        const char *wavePath = symbol_map_find(&pwMap, waveSymbols[i]);
        if (!wavePath) continue;
        uint32_t *pw = load_prog_wave(projectRoot, wavePath);
        if (!pw) continue;
        vg_register_progwave(set->container, pw);
        set->progWaves[i] = pw;
    }

    for (int i = 0; i < keysplitCount; i++) {
        set->keysplits[i].subGroup =
            load_sub_voicegroup(projectRoot, keysplitSymbols[i], set->container,
                                &dsMap, &pwMap, &ksMap, disc, &waveCache);
        const KeySplitDef *ksDef =
            keysplit_map_find_or_rescan(&ksMap, keysplitTableSymbols[i], disc);
        if (ksDef) {
            uint8_t *table = malloc(128);
            if (table) {
                memcpy(table, ksDef->table, 128);
                vg_register_keysplittable(set->container, table);
                set->keysplits[i].table = table;
            }
        }
    }

    symbol_map_free(&dsMap);
    symbol_map_free(&pwMap);
    keysplit_map_free(&ksMap);
    free(disc);
    vg_log("voicegroup_load_samples: done");
    return set;
}

void voicegroup_free_samples(LoadedSampleSet *set)
{
    if (!set) return;
    voicegroup_free(set->container);
    free(set->waves);
    free(set->progWaves);
    free(set->keysplits);
    free(set);
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
