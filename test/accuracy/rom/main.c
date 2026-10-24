/*
 * poryaaaa accuracy test ROM.
 *
 * Boots, initialises the m4a engine exactly as Pokemon Emerald does
 * (m4aSoundInit + the game's sound mode, m4aSoundVSync from the VCOUNT
 * interrupt at line 150, m4aSoundMain from the VBLANK interrupt), then starts
 * whatever the parameter block in crt0.s asks for and idles forever.
 */
#include "global.h"
#include "gba/m4a_internal.h"
#include "m4a.h"
#include "constants/sound.h"

struct AccParams
{
    u8 magic[8];
    u8 type;
    u8 volume;
    u16 id;
    s16 pitch;
    s8 pan;
    u8 startFrame;
    u8 reserved[12];
};

enum
{
    ACC_TYPE_SONG = 0,   /* id indexes gSongTable (music and SE) */
    ACC_TYPE_CRY = 1,    /* id indexes gCryTable */
    ACC_TYPE_MICRO = 2,  /* id indexes gAccMicroSongTable */
    ACC_TYPE_NONE = 0xFF
};

extern const struct AccParams gAccParams;
extern struct ToneData gCryTable[];
extern struct SongHeader *const gAccMicroSongTable[];
extern const u32 gAccMicroSongCount;

void VBlankIntr(void);
void VCountIntr(void);

void VBlankIntr(void)
{
    m4aSoundMain();
    INTR_CHECK |= INTR_FLAG_VBLANK;
}

void VCountIntr(void)
{
    m4aSoundVSync();
    INTR_CHECK |= INTR_FLAG_VCOUNT;
}

/* PlayCryInternal with CRY_MODE_NORMAL, indexing the cry table directly. */
static void StartCry(u16 index, s8 pan, u8 volume, s16 pitch)
{
    SetPokemonCryVolume(volume);
    SetPokemonCryPanpot(pan);
    SetPokemonCryPitch(pitch);
    SetPokemonCryLength(140);
    SetPokemonCryProgress(0);
    SetPokemonCryRelease(0);
    SetPokemonCryChorus(0);
    SetPokemonCryPriority(CRY_PRIORITY_NORMAL);
    SetPokemonCryTone(&gCryTable[index]);
}

static void StartTest(void)
{
    switch (gAccParams.type)
    {
    case ACC_TYPE_SONG:
        m4aSongNumStart(gAccParams.id);
        break;
    case ACC_TYPE_CRY:
        StartCry(gAccParams.id, gAccParams.pan, gAccParams.volume, gAccParams.pitch);
        break;
    case ACC_TYPE_MICRO:
        if (gAccParams.id < gAccMicroSongCount)
            MPlayStart(&gMPlayInfo_BGM, gAccMicroSongTable[gAccParams.id]);
        break;
    default:
        break;
    }
}

void AgbMain(void)
{
    u32 frame = 0;
    bool8 started = FALSE;

    RegisterRamReset(RESET_ALL);
    REG_WAITCNT = WAITCNT_PREFETCH_ENABLE | WAITCNT_WS0_S_1 | WAITCNT_WS0_N_3;

    m4aSoundInit();

    REG_DISPSTAT = DISPSTAT_VBLANK_INTR | DISPSTAT_VCOUNT_INTR | (150 << 8);
    REG_IE = INTR_FLAG_VBLANK | INTR_FLAG_VCOUNT;
    REG_IME = 1;

    for (;;)
    {
        VBlankIntrWait();
        frame++;
        if (!started && frame >= gAccParams.startFrame)
        {
            started = TRUE;
            StartTest();
        }
    }
}
