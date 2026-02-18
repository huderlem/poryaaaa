#ifndef M4A_PLUGIN_H
#define M4A_PLUGIN_H

#include "m4a_engine.h"
#include "voicegroup_loader.h"

typedef struct {
    M4AEngine engine;
    LoadedVoiceGroup *loadedVg;
    char projectRoot[512];
    char voicegroupName[256];
    uint8_t reverbAmount;
    uint8_t masterVolume;
    bool activated;
} M4APluginData;

#endif /* M4A_PLUGIN_H */
