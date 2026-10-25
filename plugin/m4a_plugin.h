#ifndef M4A_PLUGIN_H
#define M4A_PLUGIN_H

#include "m4a_engine.h"
#include "voicegroup_loader.h"
#include "m4a_gui.h"
#include <clap/clap.h>

typedef struct {
    M4AEngine engine;
    LoadedVoiceGroup *loadedVg;
    VoicegroupLoaderConfig loaderConfig;
    char projectRoot[512];
    char voicegroupName[256];
    uint8_t reverbAmount;
    uint8_t masterVolume; // The m4a-level master volume (0-15)
    uint8_t songMasterVolume; // The song-level master volume (0-127)
    uint8_t maxPcmChannels;
    /* DirectSound PCM mix rate: the sound-mode frequency index
     * (M4A_PCM_FREQ_MODE_MIN..MAX; 4 = 13379 Hz, the game's). */
    uint8_t pcmFreqMode;
    /* Opt-in effect features (persisted; default off) */
    bool respectBaseMidiKey;
    bool portamentoEnabled;
    bool pwmEnabled;
    /* MIDI event latch override (M4ALatchMode: transport / on / off;
     * persisted).  Written by the main thread (GUI, state load), read and
     * applied by plugin_process at the top of each block and at each
     * in-block transport event. */
    uint8_t eventLatchMode;
    /* Polyphony-overflow debug: solo the sounds lost to the polyphony limit.
     * Session-only -- deliberately NOT saved to config or CLAP state, so a
     * project never reopens silently stuck in the debug listening mode. */
    bool polyDebugInvert;
    bool activated;
    bool transportWasPlaying; /* last seen CLAP_TRANSPORT_IS_PLAYING state */

    /* Voice editor: snapshot of original voices and per-voice override flags */
    ToneData originalVoices[VOICEGROUP_SIZE];
    bool voiceOverrides[VOICEGROUP_SIZE];

    /* GUI */
    const clap_host_t *host;
    M4AGuiState *gui;
    clap_id guiTimerId;

    /* Set when the plugin calls request_restart (e.g. after Reload).
     * The standalone polls this to perform the actual restart cycle. */
    bool restartRequested;
} M4APluginData;

#endif /* M4A_PLUGIN_H */
