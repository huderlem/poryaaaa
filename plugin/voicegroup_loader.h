#ifndef VOICEGROUP_LOADER_H
#define VOICEGROUP_LOADER_H

#include "m4a_engine.h"

#define VOICEGROUP_SIZE 128

/*
 * Loaded voicegroup data - holds all allocated resources.
 * Must be freed with voicegroup_free() when done.
 */
typedef struct {
    ToneData voices[VOICEGROUP_SIZE];

    /* Loaded wave data (samples) */
    WaveData **waveDatas;
    int waveDataCount;
    int waveDataCapacity;

    /* Loaded programmable wave data */
    uint32_t **progWaves;
    int progWaveCount;
    int progWaveCapacity;

    /* Sub-voicegroups (keysplits, drumsets) */
    ToneData **subGroups;
    int subGroupCount;
    int subGroupCapacity;

    /* Keysplit tables */
    uint8_t **keySplitTables;
    int keySplitTableCount;
    int keySplitTableCapacity;
} LoadedVoiceGroup;

/*
 * Load a voicegroup from a pokeemerald project.
 *
 * projectRoot: path to the pokeemerald project root directory
 * voicegroupName: name of the voicegroup (e.g. "petalburg")
 *                 The loader will look for sound/voicegroups/<name>.inc
 *
 * Returns a LoadedVoiceGroup on success, or NULL on failure.
 * The caller must free the result with voicegroup_free().
 */
LoadedVoiceGroup *voicegroup_load(const char *projectRoot, const char *voicegroupName);

/*
 * Free all resources associated with a loaded voicegroup.
 */
void voicegroup_free(LoadedVoiceGroup *vg);

#endif /* VOICEGROUP_LOADER_H */
