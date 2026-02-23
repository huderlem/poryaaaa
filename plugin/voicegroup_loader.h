#ifndef VOICEGROUP_LOADER_H
#define VOICEGROUP_LOADER_H

#include "m4a_engine.h"

#define VOICEGROUP_SIZE 128
#define VG_MAX_PATH_LEN 512

/*
 * Optional configuration for the voicegroup loader.
 * All paths are relative to the project root directory.
 * Zero-initialized config means "auto-discover everything".
 */
typedef struct {
    char soundDataPaths[8][VG_MAX_PATH_LEN];    /* extra .inc files with sample symbol definitions */
    int soundDataPathCount;
    char voicegroupPaths[8][VG_MAX_PATH_LEN];   /* extra voicegroup directories or files */
    int voicegroupPathCount;
    char sampleDirs[8][VG_MAX_PATH_LEN];        /* extra directories with .wav sample files */
    int sampleDirCount;
} VoicegroupLoaderConfig;

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
 * Load a voicegroup from a project.
 *
 * projectRoot: path to the project root directory
 * voicegroupName: name of the voicegroup (e.g. "petalburg", "voicegroup000")
 * config: optional loader configuration (NULL for pure auto-discovery)
 *
 * The loader auto-discovers project structure (pokeemerald, pokefirered,
 * and forks with custom sound directories). Config overrides can
 * specify additional search paths.
 *
 * Returns a LoadedVoiceGroup on success, or NULL on failure.
 * The caller must free the result with voicegroup_free().
 */
LoadedVoiceGroup *voicegroup_load(const char *projectRoot, const char *voicegroupName,
                                   const VoicegroupLoaderConfig *config);

/*
 * Free all resources associated with a loaded voicegroup.
 */
void voicegroup_free(LoadedVoiceGroup *vg);

/*
 * Set an optional file path for diagnostic logging inside the voicegroup loader.
 * Pass NULL to disable. The same path used by the plugin's "log=" config key works.
 * Call before voicegroup_load() for the output to be useful.
 */
void voicegroup_loader_set_log_path(const char *path);

#endif /* VOICEGROUP_LOADER_H */
