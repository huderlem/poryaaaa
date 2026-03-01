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
    bool analogFilter;
    uint8_t maxPcmChannels;
    bool activated;

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
