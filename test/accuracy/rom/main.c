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
    u16 length;        /* cry: SetPokemonCryLength (PlayCryInternal's 140) */
    u8 release;        /* cry: SetPokemonCryRelease (PlayCryInternal's 0) */
    s8 chorus;         /* cry: SetPokemonCryChorus (PlayCryInternal's 0; nonzero = two tracks) */
    u16 id2;           /* second cry: gCryTable index, started on startFrame2 with CRY_MODE_NORMAL's
                        * defaults (volume 120, pan 0, pitch 15360, length 140, release 0, chorus 0)
                        * through the same StartCry path, so SetPokemonCryTone picks its player */
    u8 startFrame2;    /* VBlank count at which the second cry starts; 0 = no second cry (a block
                        * patched by an older renderer plays as before) */
    u8 reserved0;      /* alignment of id3 (block offset 23) */
    u16 id3;           /* third cry (block offset 24): gCryTable index, started on startFrame3 with the
                        * same defaults through the same StartCry path -- with both cry players busy,
                        * SetPokemonCryTone replaces the one with the larger clock */
    u8 startFrame3;    /* VBlank count at which the third cry starts (block offset 26); 0 = none */
    u8 reserved1;
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

/* PlayCryInternal with the parameter block's volume / pan / pitch / length /
 * release / chorus (CRY_MODE_NORMAL's at the block's defaults: 120, 0, 15360,
 * 140, 0, 0), indexing the cry table directly.  Progress stays 0. */
static void StartCry(u16 index, s8 pan, u8 volume, s16 pitch, u16 length, u8 release, s8 chorus)
{
    SetPokemonCryVolume(volume);
    SetPokemonCryPanpot(pan);
    SetPokemonCryPitch(pitch);
    SetPokemonCryLength(length);
    SetPokemonCryProgress(0);
    SetPokemonCryRelease(release);
    SetPokemonCryChorus(chorus);
    SetPokemonCryPriority(CRY_PRIORITY_NORMAL);
    SetPokemonCryTone(&gCryTable[index]);
}

/* Starts the test on gAccParams.startFrame (AgbMain's loop calls this on that
 * VBlank) and, if startFrame2 / startFrame3 are nonzero, waits here for the
 * second / third cry's frame and starts each with CRY_MODE_NORMAL's defaults,
 * then returns to the loop.
 * The wait lives in this function so that AgbMain stays textually identical to
 * the ROM before the second cry existed and agbcc emits the same prologue and
 * steady-state loop: mgba's ordering of a timer-0 overflow against the DAC
 * slot it coincides with depends on the timer's start cycle, and two extra
 * prologue instructions (6 cycles before m4aSoundInit) once moved every
 * DirectSound reference (plan, probe step 11). */
static void StartTest(void)
{
    u32 frame;

    switch (gAccParams.type)
    {
    case ACC_TYPE_SONG:
        m4aSongNumStart(gAccParams.id);
        break;
    case ACC_TYPE_CRY:
        StartCry(gAccParams.id, gAccParams.pan, gAccParams.volume, gAccParams.pitch,
                 gAccParams.length, gAccParams.release, gAccParams.chorus);
        break;
    case ACC_TYPE_MICRO:
        if (gAccParams.id < gAccMicroSongCount)
            MPlayStart(&gMPlayInfo_BGM, gAccMicroSongTable[gAccParams.id]);
        break;
    default:
        break;
    }

    /* AgbMain's frame count is startFrame here (it counts VBlanks from 1). */
    frame = gAccParams.startFrame ? gAccParams.startFrame : 1;

    if (gAccParams.startFrame2 != 0)
    {
        while (frame < gAccParams.startFrame2)
        {
            VBlankIntrWait();
            frame++;
        }
        StartCry(gAccParams.id2, 0, 120, 15360, 140, 0, 0);
    }

    if (gAccParams.startFrame3 != 0)
    {
        while (frame < gAccParams.startFrame3)
        {
            VBlankIntrWait();
            frame++;
        }
        StartCry(gAccParams.id3, 0, 120, 15360, 140, 0, 0);
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
