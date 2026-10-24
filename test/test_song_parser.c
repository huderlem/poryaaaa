/*
 * Byte-exact check of the .s song parser against the accuracy test ROM.
 *
 *   poryaaaa_song_tests [<rom build dir>]
 *
 * <rom build dir> (default build/accuracy/rom) holds accuracy_test.gba,
 * accuracy_test.map and the mid2agb output the ROM was built from under
 * micro/*.s and songs/*.s.  For every .s file the parser's output, relocated
 * to the song's ROM address from the map, must equal the ROM bytes.  Also
 * checks the header fields and the voicegroup name resolved from `_grp`.
 *
 * Exit status 0 when every song matches, 1 otherwise, 2 when the ROM build is
 * missing (skipped).
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m4a_song.h"

#define ROM_BASE 0x08000000u

typedef struct { char *name; uint32_t addr; } MapSym;
static MapSym *gSyms;
static int gSymCount, gSymCap;

static int load_map(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* "                0x00000000082b9d40                mus_littleroot" */
        unsigned long long addr;
        char sym[256];
        int n = sscanf(line, " 0x%llx %255s", &addr, sym);
        if (n != 2 || strchr(sym, '/') || sym[0] == '.' || sym[0] == '*')
            continue;
        if (gSymCount == gSymCap) {
            gSymCap = gSymCap ? gSymCap * 2 : 1024;
            gSyms = realloc(gSyms, (size_t)gSymCap * sizeof(MapSym));
        }
        gSyms[gSymCount].name = strdup(sym);
        gSyms[gSymCount].addr = (uint32_t)addr;
        gSymCount++;
    }
    fclose(f);
    return 0;
}

static int map_lookup(const char *name, uint32_t *addr)
{
    for (int i = 0; i < gSymCount; i++)
        if (strcmp(gSyms[i].name, name) == 0) { *addr = gSyms[i].addr; return 0; }
    return -1;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static int check_song(const char *path, const uint8_t *rom, size_t romSize)
{
    char err[256];
    M4ASong *song = m4a_song_load(path, err, sizeof(err));
    if (!song) {
        printf("FAIL %s: parse error: %s\n", path, err);
        return 1;
    }

    int bad = 0;
    uint32_t headerAddr, vgAddr = 0;
    if (map_lookup(song->name, &headerAddr) != 0) {
        printf("FAIL %s: symbol %s not in map\n", path, song->name);
        m4a_song_free(song);
        return 1;
    }
    char vgSym[160];
    snprintf(vgSym, sizeof(vgSym), "voicegroup_%s", song->voicegroup);
    if (map_lookup(vgSym, &vgAddr) != 0) {
        printf("FAIL %s: voicegroup symbol %s not in map\n", path, vgSym);
        bad = 1;
    }

    uint32_t base = headerAddr - song->headerOffset;
    uint32_t romOff = base - ROM_BASE;
    if (romOff + song->size > romSize) {
        printf("FAIL %s: song (0x%08x + %u) lies outside the ROM\n", path, base, song->size);
        m4a_song_free(song);
        return 1;
    }

    uint8_t *img = malloc(song->size);
    m4a_song_relocate(song, base, vgAddr, img);
    for (uint32_t i = 0; i < song->size; i++) {
        if (img[i] != rom[romOff + i]) {
            printf("FAIL %s: byte mismatch at offset 0x%x (rom %02x, parsed %02x)\n",
                   path, i, rom[romOff + i], img[i]);
            bad = 1;
            break;
        }
    }
    free(img);

    /* Header fields must agree with the ROM's view of the header. */
    const uint8_t *h = rom + romOff + song->headerOffset;
    if (h[0] != song->trackCount || h[1] != song->blockCount || h[2] != song->priority || h[3] != song->reverb) {
        printf("FAIL %s: header fields differ\n", path);
        bad = 1;
    }
    for (int t = 0; t < song->trackCount && !bad; t++) {
        const uint8_t *w = h + 8 + 4 * t;
        uint32_t addr = (uint32_t)w[0] | ((uint32_t)w[1] << 8) | ((uint32_t)w[2] << 16) | ((uint32_t)w[3] << 24);
        if (addr != base + song->trackOffsets[t]) {
            printf("FAIL %s: track %d pointer differs\n", path, t + 1);
            bad = 1;
        }
    }

    if (!bad)
        printf("ok   %-40s %6u bytes, %2u tracks, %3d relocs, voicegroup %s\n",
               song->name, song->size, song->trackCount, song->relocCount, song->voicegroup);
    m4a_song_free(song);
    return bad;
}

static int check_dir(const char *romDir, const char *sub, const uint8_t *rom, size_t romSize, int *count)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", romDir, sub);
    DIR *d = opendir(dir);
    if (!d) {
        printf("FAIL cannot open %s\n", dir);
        return 1;
    }
    /* Sort for stable output. */
    char **names = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 2 && strcmp(e->d_name + len - 2, ".s") == 0) {
            names = realloc(names, (size_t)(n + 1) * sizeof(char *));
            names[n++] = strdup(e->d_name);
        }
    }
    closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
        }
    int failures = 0;
    for (int i = 0; i < n; i++) {
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        failures += check_song(path, rom, romSize);
        (*count)++;
        free(names[i]);
    }
    free(names);
    return failures;
}

int main(int argc, char **argv)
{
    const char *romDir = argc > 1 ? argv[1] : "build/accuracy/rom";
    char path[1024];

    snprintf(path, sizeof(path), "%s/accuracy_test.gba", romDir);
    size_t romSize;
    uint8_t *rom = read_file(path, &romSize);
    if (!rom) {
        printf("SKIP: %s not found (build the accuracy ROM first)\n", path);
        return 2;
    }
    snprintf(path, sizeof(path), "%s/accuracy_test.map", romDir);
    if (load_map(path) != 0) {
        printf("SKIP: %s not found\n", path);
        return 2;
    }

    int count = 0, failures = 0;
    failures += check_dir(romDir, "micro", rom, romSize, &count);
    failures += check_dir(romDir, "songs", rom, romSize, &count);

    printf("\n%d/%d songs match the ROM byte for byte\n", count - failures, count);
    free(rom);
    return failures ? 1 : 0;
}
