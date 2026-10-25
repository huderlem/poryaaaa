#ifndef M4A_CRY_H
#define M4A_CRY_H

/*
 * Pokemon cries: the song a cry plays through.
 *
 * The GBA has no song data for a cry.  m4aSoundInit copies
 * gPokemonCrySongTemplate (src/m4a_tables.c) into gPokemonCrySong, the
 * SetPokemonCry* setters (src/m4a.c) patch its parameter bytes, and
 * SetPokemonCryTone copies the result into gPokemonCrySongs[i] with the
 * cry's ToneData as the one-voice voicegroup and the part / GOTO pointers
 * resolved, then MPlayStart's it on cry player i (gPokemonCryMusicPlayers,
 * two tracks each; from silence it is player 0).  The template is one
 * track (two with chorus):
 *
 *   TUNE c_v ; GOTO cont               (part0; part1 is TUNE c_v+16 then cont)
 *   cont: VOICE 0 ; VOL 127 ; XCMD 0x0D 0 (u32) ; XCMD xRELE 0 ; PAN c_v ;
 *         TIE Cn3 v127 ; XCMD xWAIT 60 (u16) ; EOT ; FINE
 *
 * m4a_cry_song_new builds that PokemonCrySong as an M4ASong (the pointer
 * words hold offsets, as the parser's do) with dataInRam set: the ROM keeps
 * gPokemonCrySongs in IWRAM, which the sequencer's timing model charges
 * (M4A_CYC_ROM_BYTE).  The ToneData itself comes from the voicegroup loader
 * (voicegroup_load_cry) and goes into voice 0 of the engine's voicegroup.
 */

#include <stdint.h>
#include "m4a_song.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The setters' arguments, applied in PlayCryInternal's order (src/sound.c:
 * volume, panpot, pitch, length, progress, release, chorus, priority). */
typedef struct {
    uint8_t volume;     /* SetPokemonCryVolume: & 0x7F (CRY_VOLUME 120) */
    int8_t pan;         /* SetPokemonCryPanpot: (pan + c_v) & 0x7F (0) */
    int16_t pitch;      /* SetPokemonCryPitch: 15360 = key 60 at c_v (CRY_MODE_NORMAL) */
    uint16_t length;    /* SetPokemonCryLength: xWAIT ticks (140) */
    uint32_t progress;  /* SetPokemonCryProgress: XCMD 0x0D's word (0) */
    uint8_t release;    /* SetPokemonCryRelease: xRELE (0) */
    int8_t chorus;      /* SetPokemonCryChorus: 0 = one track, else two with TUNE c_v + chorus */
    uint8_t priority;   /* SetPokemonCryPriority (CRY_PRIORITY_NORMAL 10) */
} M4ACryParams;

/* PlayCryInternal's CRY_MODE_NORMAL values (the accuracy ROM's StartCry defaults). */
void m4a_cry_params_default(M4ACryParams *p);

#define M4A_CRY_SONG_SIZE 0x34   /* sizeof(struct PokemonCrySong) */

/* The template with the setters applied, as the ROM holds it after
 * SetPokemonCryTone (voice 0 = the cry's ToneData).  Free with
 * m4a_song_free.  NULL on allocation failure. */
M4ASong *m4a_cry_song_new(const M4ACryParams *p);

/* The template's bytes before any setter (gPokemonCrySongTemplate's first
 * M4A_CRY_SONG_SIZE bytes as the ROM holds them, pointer words 0). */
extern const uint8_t gM4ACrySongTemplate[M4A_CRY_SONG_SIZE];

#ifdef __cplusplus
}
#endif

#endif /* M4A_CRY_H */
