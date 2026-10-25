#include "m4a_cry.h"
#include <stdlib.h>
#include <string.h>

/* struct PokemonCrySong (include/gba/m4a_internal.h) offsets */
enum {
    CRY_TRACK_COUNT = 0x00,
    CRY_BLOCK_COUNT = 0x01,
    CRY_PRIORITY    = 0x02,
    CRY_REVERB      = 0x03,
    CRY_TONE        = 0x04,   /* struct ToneData * */
    CRY_PART0_PTR   = 0x08,   /* u8 *part[2] */
    CRY_PART1_PTR   = 0x0C,
    CRY_GAP         = 0x10,
    CRY_PART0       = 0x11,   /* TUNE */
    CRY_TUNE        = 0x12,
    CRY_GOTO        = 0x13,
    CRY_GOTO_TARGET = 0x14,   /* u32 */
    CRY_PART1       = 0x18,   /* TUNE */
    CRY_TUNE2       = 0x19,
    CRY_CONT        = 0x1A,   /* VOICE 0 */
    CRY_VOL_CMD     = 0x1C,
    CRY_VOLUME      = 0x1D,
    CRY_XCMD_0D     = 0x1E,   /* XCMD 0x0D */
    CRY_0D_PARAM    = 0x20,   /* u32 */
    CRY_XRELE_CMD   = 0x24,   /* XCMD xRELE */
    CRY_RELEASE     = 0x26,
    CRY_PAN_CMD     = 0x27,
    CRY_PAN         = 0x28,
    CRY_TIE_CMD     = 0x29,
    CRY_TIE_KEY     = 0x2A,
    CRY_TIE_VEL     = 0x2B,
    CRY_XWAIT_CMD   = 0x2C,   /* XCMD xWAIT */
    CRY_LENGTH      = 0x2E,   /* u16 */
    CRY_END         = 0x30    /* EOT FINE */
};

#define C_V 0x40

/* gPokemonCrySongTemplate (src/m4a_tables.c), the ROM's bytes at
 * 0x08002F20 in the 2026-10-24 accuracy ROM with the voicegroup_dummy word
 * zeroed: trackCount 1, blockCount 0, priority 255, reverb 0, tone, part
 * {0, 0}, gap 0, TUNE c_v, GOTO 0, TUNE c_v+16, VOICE 0, VOL 127,
 * XCMD 0x0D 0, XCMD xRELE 0, PAN c_v, TIE Cn3 v127, XCMD xWAIT 60, EOT,
 * FINE, two padding bytes. */
const uint8_t gM4ACrySongTemplate[M4A_CRY_SONG_SIZE] = {
    0x01, 0x00, 0xFF, 0x00,                         /* trackCount blockCount priority reverb */
    0x00, 0x00, 0x00, 0x00,                         /* tone */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* part[2] */
    0x00,                                           /* gap */
    0xC8, C_V,                                      /* part0: TUNE c_v */
    0xB2, 0x00, 0x00, 0x00, 0x00,                   /* GOTO cont */
    0xC8, C_V + 16,                                 /* part1: TUNE c_v+16 */
    0xBD, 0x00,                                     /* cont: VOICE 0 */
    0xBE, 0x7F,                                     /* VOL 127 */
    0xCD, 0x0D, 0x00, 0x00, 0x00, 0x00,             /* XCMD 0x0D, u32 0 */
    0xCD, 0x07, 0x00,                               /* XCMD xRELE 0 */
    0xBF, C_V,                                      /* PAN c_v */
    0xCF, 0x3C, 0x7F,                               /* TIE Cn3 v127 */
    0xCD, 0x0C, 0x3C, 0x00,                         /* XCMD xWAIT 60 */
    0xCE, 0xB1,                                     /* EOT FINE */
    0x00, 0x00                                      /* padding */
};

void m4a_cry_params_default(M4ACryParams *p)
{
    p->volume = 120;        /* CRY_VOLUME */
    p->pan = 0;
    p->pitch = 15360;       /* PlayCryInternal's CRY_MODE_NORMAL pitch */
    p->length = 140;        /* PlayCryInternal's length */
    p->progress = 0;
    p->release = 0;
    p->chorus = 0;
    p->priority = 10;       /* CRY_PRIORITY_NORMAL */
}

static void wr32(uint8_t *d, int off, uint32_t v)
{
    d[off] = (uint8_t)v;
    d[off + 1] = (uint8_t)(v >> 8);
    d[off + 2] = (uint8_t)(v >> 16);
    d[off + 3] = (uint8_t)(v >> 24);
}

M4ASong *m4a_cry_song_new(const M4ACryParams *p)
{
    M4ASong *song = calloc(1, sizeof(M4ASong));
    uint8_t *d = calloc(1, M4A_CRY_SONG_SIZE);
    M4ASongReloc *relocs = calloc(4, sizeof(M4ASongReloc));
    if (!song || !d || !relocs) {
        free(song);
        free(d);
        free(relocs);
        return NULL;
    }
    memcpy(d, gM4ACrySongTemplate, M4A_CRY_SONG_SIZE);

    /* The setters, in PlayCryInternal's order, on the template (m4a.c). */
    d[CRY_VOLUME] = p->volume & 0x7F;                           /* SetPokemonCryVolume */
    d[CRY_PAN] = (uint8_t)((p->pan + C_V) & 0x7F);              /* SetPokemonCryPanpot */
    {                                                           /* SetPokemonCryPitch */
        int16_t b = (int16_t)(p->pitch + 0x80);
        uint8_t a = (uint8_t)(d[CRY_TUNE2] - d[CRY_TUNE]);
        d[CRY_TIE_KEY] = (uint8_t)((b >> 8) & 0x7F);
        d[CRY_TUNE] = (uint8_t)((b >> 1) & 0x7F);
        d[CRY_TUNE2] = (uint8_t)((a + ((b >> 1) & 0x7F)) & 0x7F);
    }
    d[CRY_LENGTH] = (uint8_t)p->length;                         /* SetPokemonCryLength */
    d[CRY_LENGTH + 1] = (uint8_t)(p->length >> 8);
    wr32(d, CRY_0D_PARAM, p->progress);                         /* SetPokemonCryProgress */
    d[CRY_RELEASE] = p->release;                                /* SetPokemonCryRelease */
    if (p->chorus) {                                            /* SetPokemonCryChorus */
        d[CRY_TRACK_COUNT] = 2;
        d[CRY_TUNE2] = (uint8_t)((p->chorus + d[CRY_TUNE]) & 0x7F);
    } else {
        d[CRY_TRACK_COUNT] = 1;
    }
    d[CRY_PRIORITY] = p->priority;                              /* SetPokemonCryPriority */

    /* SetPokemonCryTone: the pointer words.  Offsets here; the ROM holds
     * &gPokemonCrySongs[i].part0 etc. (m4a_song_relocate rebuilds them). */
    wr32(d, CRY_TONE, 0);
    wr32(d, CRY_PART0_PTR, CRY_PART0);
    wr32(d, CRY_PART1_PTR, CRY_PART1);
    wr32(d, CRY_GOTO_TARGET, CRY_CONT);
    relocs[0] = (M4ASongReloc){ CRY_TONE, M4A_SONG_RELOC_VOICEGROUP, 0 };
    relocs[1] = (M4ASongReloc){ CRY_PART0_PTR, CRY_PART0, 0 };
    relocs[2] = (M4ASongReloc){ CRY_PART1_PTR, CRY_PART1, 0 };
    relocs[3] = (M4ASongReloc){ CRY_GOTO_TARGET, CRY_CONT, 0 };

    song->data = d;
    song->size = M4A_CRY_SONG_SIZE;
    song->headerOffset = 0;
    strcpy(song->name, "gPokemonCrySong");
    song->voicegroup[0] = '\0';         /* the cry's ToneData is the voicegroup */
    song->trackCount = d[CRY_TRACK_COUNT];
    song->blockCount = d[CRY_BLOCK_COUNT];
    song->priority = d[CRY_PRIORITY];
    song->reverb = d[CRY_REVERB];
    song->trackOffsets[0] = CRY_PART0;
    song->trackOffsets[1] = CRY_PART1;
    song->relocs = relocs;
    song->relocCount = 4;
    song->dataInRam = 1;
    return song;
}
