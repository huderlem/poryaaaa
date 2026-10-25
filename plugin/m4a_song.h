#ifndef M4A_SONG_H
#define M4A_SONG_H

/*
 * Parser for mid2agb's `.s` song output (and hand-written songs that use the
 * same vocabulary, sound/MPlayDef.s in pokeemerald).
 *
 * The parser is a tiny assembler for the subset of GNU as that these files
 * use: `.equ`, `.byte`, `.word`, `.align`, labels, `@` comments, integer
 * expressions with + - * / % and parentheses, plus the MPlayDef.s symbol
 * vocabulary built in (W00..W96, N01..N96, TIE, EOT, VOICE, VOL, PAN, ...,
 * note names CnM2..Gn8, velocities v000..v127, gtp1..3, mod_*, mem_*,
 * reverb_set, c_v, mxv).  `.include` lines are ignored: the vocabulary is
 * fixed, so binutils and the pokeemerald tree are not needed at run time.
 *
 * Output is the byte-exact song data as the ROM build would place it in
 * .rodata, with offset 0 at the first emitted byte (the first track).  The
 * `.word` fields (song header voicegroup pointer and track pointers, GOTO /
 * PATT / REPT / MEMACC targets) hold OFFSETS INTO `data` where the ROM holds
 * absolute addresses; `relocs` lists them so a caller can rebuild the ROM
 * image for a byte-for-byte comparison (see test/test_song_parser.c).
 *
 * The voicegroup is taken from the header's `_grp` symbol
 * (`.equ mus_x_grp, voicegroup_littleroot` -> "littleroot").
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4A_SONG_MAX_TRACKS 16

typedef struct {
    uint32_t offset;        /* position of the 32-bit little-endian word in data */
    int32_t target;         /* >= 0: label offset within data (word value = target
                             * + addend); M4A_SONG_RELOC_VOICEGROUP: the song
                             * header's voicegroup pointer (word value = 0) */
    int32_t addend;
} M4ASongReloc;

#define M4A_SONG_RELOC_VOICEGROUP (-1)

typedef struct {
    uint8_t *data;
    uint32_t size;

    /* Song header (SongHeader in m4a_internal.h) */
    uint32_t headerOffset;
    char name[96];          /* header label (from `.global`), e.g. "mus_littleroot" */
    char voicegroup[96];    /* voicegroup name without the "voicegroup_" prefix */
    uint8_t trackCount;
    uint8_t blockCount;
    uint8_t priority;
    uint8_t reverb;         /* bit 7 set: apply (reverb & 0x7F) at song start */
    uint32_t trackOffsets[M4A_SONG_MAX_TRACKS];

    M4ASongReloc *relocs;
    int relocCount;

    /* Where the ROM keeps the data.  0: ROM, as every .s song (the
     * parser's default); 1: IWRAM, as gPokemonCrySongs (m4a_cry.c).  The
     * sequencer's timing model charges M4A_CYC_ROM_BYTE per song-data byte
     * MPlayMain reads from ROM. */
    uint8_t dataInRam;
} M4ASong;

/* Parse song text.  On failure returns NULL and, when err is non-NULL, writes
 * a one-line message (with the line number) into it. */
M4ASong *m4a_song_parse(const char *text, size_t len, char *err, size_t errLen);

/* Read and parse a file. */
M4ASong *m4a_song_load(const char *path, char *err, size_t errLen);

void m4a_song_free(M4ASong *song);

/* Build the ROM image of the song: `data` with every relocated word replaced
 * by its absolute address, given the ROM address of offset 0 and of the
 * voicegroup.  `out` must hold song->size bytes. */
void m4a_song_relocate(const M4ASong *song, uint32_t baseAddr, uint32_t voicegroupAddr,
                       uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* M4A_SONG_H */
