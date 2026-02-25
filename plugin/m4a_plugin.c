#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/draft/undo.h>
#include "m4a_plugin.h"
#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_reverb.h"
#include "voicegroup_loader.h"
#include "m4a_gui.h"

/*
 * M4A VSTi Plugin - CLAP implementation
 *
 * A CLAP instrument plugin that uses the GBA m4a sound engine to render audio.
 * Receives MIDI input from the DAW and produces stereo audio output.
 */

/* Plugin descriptor */
static const char *s_features[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_SAMPLER,
    CLAP_PLUGIN_FEATURE_STEREO,
    NULL
};

static const clap_plugin_descriptor_t s_descriptor = {
    .clap_version = CLAP_VERSION,
    .id = "com.huderlem.poryaaaa",
    .name = "poryaaaa",
    .vendor = "pokeemerald",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0",
    .description = "GBA M4A sound engine plugin for pokeemerald music preview",
    .features = s_features,
};

/* ---- Config file ---- */

/*
 * Directory of the loaded .clap file, set during entry_init.
 * Used to find poryaaaa.cfg in the same directory as the plugin.
 */
static char s_pluginDir[512] = {0};

/* Optional diagnostic log path, set from config key "log=<path>" */
static const char *s_pluginLogPath = NULL;

/*
 * Load settings from poryaaaa.cfg placed next to the .clap file.
 *
 * The config file uses simple key=value lines, one per line.
 * Lines starting with '#' are comments and are ignored.
 *
 * Supported keys:
 *   project_root   - Path to the pokeemerald project directory
 *   voicegroup     - Voicegroup name (e.g. petalburg, littleroot_town)
 *   reverb         - Reverb amount (0-127)
 *   master_volume  - Master volume (0-15)
 *   analog_filter  - GBA analog output low-pass filter (0=off, 1=on)
 */
static void load_config_file(M4APluginData *data)
{
    if (s_pluginDir[0] == '\0')
        return;

    char configPath[600];
    snprintf(configPath, sizeof(configPath), "%s/poryaaaa.cfg", s_pluginDir);

    FILE *f = fopen(configPath, "r");
    if (!f)
        return;

    char line[600];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline and carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = line;
        const char *value = eq + 1;

        if (strcmp(key, "log") == 0) {
            s_pluginLogPath = strdup(value); /* leak is fine for a dev diagnostic */
        } else if (strcmp(key, "project_root") == 0) {
            snprintf(data->projectRoot, sizeof(data->projectRoot), "%s", value);
        } else if (strcmp(key, "voicegroup") == 0) {
            snprintf(data->voicegroupName, sizeof(data->voicegroupName), "%s", value);
        } else if (strcmp(key, "reverb") == 0) {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            data->reverbAmount = (uint8_t)v;
        } else if (strcmp(key, "master_volume") == 0) {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > 15) v = 15;
            data->masterVolume = (uint8_t)v;
        } else if (strcmp(key, "song_master_volume") == 0) {
            int v = atoi(value);
            if (v < 0) v = 0;
            if (v > MAX_SONG_VOLUME) v = MAX_SONG_VOLUME;
            data->songMasterVolume = (uint8_t)v;
        } else if (strcmp(key, "analog_filter") == 0) {
            data->analogFilter = (atoi(value) != 0);
        } else if (strcmp(key, "max_channels") == 0) {
            int v = atoi(value);
            if (v < 1) v = 1;
            if (v > MAX_PCM_CHANNELS) v = MAX_PCM_CHANNELS;
            data->maxPcmChannels = (uint8_t)v;
        } else if (strcmp(key, "sound_data_paths") == 0) {
            /* Semicolon-separated list of extra .inc files, relative to project_root */
            char tmp[600];
            strncpy(tmp, value, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *tok = strtok(tmp, ";");
            while (tok && data->loaderConfig.soundDataPathCount < 8) {
                while (*tok == ' ') tok++;
                int idx = data->loaderConfig.soundDataPathCount++;
                snprintf(data->loaderConfig.soundDataPaths[idx],
                         sizeof(data->loaderConfig.soundDataPaths[idx]), "%s", tok);
                tok = strtok(NULL, ";");
            }
        } else if (strcmp(key, "voicegroup_paths") == 0) {
            char tmp[600];
            strncpy(tmp, value, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *tok = strtok(tmp, ";");
            while (tok && data->loaderConfig.voicegroupPathCount < 8) {
                while (*tok == ' ') tok++;
                int idx = data->loaderConfig.voicegroupPathCount++;
                snprintf(data->loaderConfig.voicegroupPaths[idx],
                         sizeof(data->loaderConfig.voicegroupPaths[idx]), "%s", tok);
                tok = strtok(NULL, ";");
            }
        } else if (strcmp(key, "sample_dirs") == 0) {
            char tmp[600];
            strncpy(tmp, value, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *tok = strtok(tmp, ";");
            while (tok && data->loaderConfig.sampleDirCount < 8) {
                while (*tok == ' ') tok++;
                int idx = data->loaderConfig.sampleDirCount++;
                snprintf(data->loaderConfig.sampleDirs[idx],
                         sizeof(data->loaderConfig.sampleDirs[idx]), "%s", tok);
                tok = strtok(NULL, ";");
            }
        }
    }

    fclose(f);
}

/* ---- Plugin lifecycle ---- */

static bool plugin_init(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    data->masterVolume = 15;
    data->songMasterVolume = MAX_SONG_VOLUME;
    data->reverbAmount = 0;
    data->analogFilter = false;
    data->maxPcmChannels = 5;
    data->projectRoot[0] = '\0';
    data->voicegroupName[0] = '\0';
    data->loadedVg = NULL;
    data->activated = false;
    data->gui = NULL;
    data->guiTimerId = CLAP_INVALID_ID;
    /* Load defaults from config file placed next to the .clap */
    load_config_file(data);
    /* Forward the log path into the voicegroup loader so it can emit diagnostics */
    voicegroup_loader_set_log_path(s_pluginLogPath);
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    /* GUI must already be destroyed by the host (gui->destroy before plugin->destroy) */
    if (data->loadedVg) {
        voicegroup_free(data->loadedVg);
        data->loadedVg = NULL;
    }
    m4a_engine_destroy(&data->engine);
    free(data);
    free((void *)plugin);
}

static bool plugin_activate(const clap_plugin_t *plugin, double sample_rate,
                            uint32_t min_frames, uint32_t max_frames)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_init(&data->engine, (float)sample_rate);
    data->engine.masterVolume = data->masterVolume;
    data->engine.songMasterVolume = data->songMasterVolume;
    data->engine.analogFilter = data->analogFilter;
    data->engine.maxPcmChannels = data->maxPcmChannels;
    m4a_reverb_set_amount(&data->engine.reverb, data->reverbAmount);

    /* If voicegroup is configured, load it */
    if (data->projectRoot[0] && data->voicegroupName[0]) {
        if (data->loadedVg) {
            voicegroup_free(data->loadedVg);
            data->loadedVg = NULL;
        }
        data->loadedVg = voicegroup_load(data->projectRoot, data->voicegroupName,
                                         &data->loaderConfig);
        if (data->loadedVg) {
            m4a_engine_set_voicegroup(&data->engine, data->loadedVg->voices);
        }
    }

    data->activated = true;

    /* Notify GUI of current voicegroup status */
    if (data->gui) {
        M4AGuiSettings gs;
        memset(&gs, 0, sizeof(gs));
        snprintf(gs.projectRoot,    sizeof(gs.projectRoot),    "%s", data->projectRoot);
        snprintf(gs.voicegroupName, sizeof(gs.voicegroupName), "%s", data->voicegroupName);
        gs.reverbAmount      = data->reverbAmount;
        gs.masterVolume      = data->masterVolume;
        gs.songMasterVolume  = data->songMasterVolume;
        gs.analogFilter      = data->analogFilter;
        gs.maxPcmChannels    = data->maxPcmChannels;
        gs.voicegroupLoaded  = (data->loadedVg != NULL);
        m4a_gui_update_settings(data->gui, &gs);
    }

    return true;
}

static void plugin_deactivate(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_destroy(&data->engine);
    data->activated = false;
}

static bool plugin_start_processing(const clap_plugin_t *plugin)
{
    return true;
}

static void plugin_stop_processing(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_all_sound_off(&data->engine);
    m4a_reverb_reset(&data->engine.reverb);
    data->engine.lowPassLeft  = 0.0f;
    data->engine.lowPassRight = 0.0f;
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_all_sound_off(&data->engine);
    m4a_reverb_reset(&data->engine.reverb);
    data->engine.lowPassLeft  = 0.0f;
    data->engine.lowPassRight = 0.0f;
}

/* ---- MIDI event processing ---- */

static void process_midi_event(M4APluginData *data, const uint8_t *msg)
{
    uint8_t status = msg[0] & 0xF0;
    uint8_t channel = msg[0] & 0x0F;

    switch (status) {
    case 0x90: /* Note On */
        if (msg[2] > 0) {
            m4a_engine_note_on(&data->engine, channel, msg[1], msg[2]);
        } else {
            /* velocity 0 = note off */
            m4a_engine_note_off(&data->engine, channel, msg[1]);
        }
        break;
    case 0x80: /* Note Off */
        m4a_engine_note_off(&data->engine, channel, msg[1]);
        break;
    case 0xC0: /* Program Change */
        m4a_engine_program_change(&data->engine, channel, msg[1]);
        break;
    case 0xB0: /* Control Change */
        m4a_engine_cc(&data->engine, channel, msg[1], msg[2]);
        break;
    case 0xE0: /* Pitch Bend */
    {
        int16_t bend = ((int16_t)msg[2] << 7 | msg[1]) - 8192;
        m4a_engine_pitch_bend(&data->engine, channel, bend);
        break;
    }
    }
}

static void process_clap_note_event(M4APluginData *data, const clap_event_note_t *ev)
{
    int channel = ev->channel >= 0 ? ev->channel : 0;
    if (channel >= MAX_TRACKS) channel = 0;

    if (ev->header.type == CLAP_EVENT_NOTE_ON) {
        uint8_t velocity = (uint8_t)(ev->velocity * 127.0 + 0.5);
        if (velocity == 0) velocity = 1;
        m4a_engine_note_on(&data->engine, channel, (uint8_t)ev->key, velocity);
    } else if (ev->header.type == CLAP_EVENT_NOTE_OFF) {
        m4a_engine_note_off(&data->engine, channel, (uint8_t)ev->key);
    } else if (ev->header.type == CLAP_EVENT_NOTE_CHOKE) {
        m4a_engine_note_off(&data->engine, channel, (uint8_t)ev->key);
    }
}

/* ---- Audio processing ---- */

static clap_process_status plugin_process(const clap_plugin_t *plugin,
                                           const clap_process_t *process)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    if (!data->activated)
        return CLAP_PROCESS_ERROR;

    /* Read tempo from host transport (MIDI meta event tempo) */
    if (process->transport
        && (process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO)) {
        m4a_engine_set_tempo_bpm(&data->engine, process->transport->tempo);
    }

    const uint32_t numFrames = process->frames_count;
    const uint32_t numEvents = process->in_events->size(process->in_events);

    /* Get output buffers */
    float *outL = process->audio_outputs[0].data32[0];
    float *outR = process->audio_outputs[0].data32[1];

    /* Process with sample-accurate event handling */
    uint32_t eventIdx = 0;
    uint32_t framePos = 0;

    while (framePos < numFrames) {
        /* Process all events at current position */
        while (eventIdx < numEvents) {
            const clap_event_header_t *hdr = process->in_events->get(process->in_events, eventIdx);
            if (hdr->time > framePos)
                break;

            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID) {
                switch (hdr->type) {
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                case CLAP_EVENT_NOTE_CHOKE:
                    process_clap_note_event(data, (const clap_event_note_t *)hdr);
                    break;
                case CLAP_EVENT_MIDI:
                {
                    const clap_event_midi_t *midiEv = (const clap_event_midi_t *)hdr;
                    process_midi_event(data, midiEv->data);
                    break;
                }
                }
            }
            eventIdx++;
        }

        /* Determine how many frames to render before next event */
        uint32_t nextEventTime = numFrames;
        if (eventIdx < numEvents) {
            const clap_event_header_t *hdr = process->in_events->get(process->in_events, eventIdx);
            if (hdr->time < nextEventTime)
                nextEventTime = hdr->time;
        }

        uint32_t framesToRender = nextEventTime - framePos;
        if (framesToRender > 0) {
            m4a_engine_process(&data->engine, outL + framePos, outR + framePos,
                              (int)framesToRender);
        }

        framePos = nextEventTime;
    }

    return CLAP_PROCESS_CONTINUE;
}

/* ---- Extensions ---- */

/* Audio ports extension */
static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    return is_input ? 0 : 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                            clap_audio_port_info_t *info)
{
    if (is_input || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "Audio Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t s_audio_ports = {
    .count = audio_ports_count,
    .get = audio_ports_get,
};

/* Note ports extension */
static uint32_t note_ports_count(const clap_plugin_t *plugin, bool is_input)
{
    return is_input ? 1 : 0;
}

static bool note_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                           clap_note_port_info_t *info)
{
    if (!is_input || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "MIDI Input");
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    return true;
}

static const clap_plugin_note_ports_t s_note_ports = {
    .count = note_ports_count,
    .get = note_ports_get,
};

/* State extension - save/load voicegroup configuration */
static bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    /* Write a simple format: lengths + strings + parameters */
    uint32_t rootLen = (uint32_t)strlen(data->projectRoot);
    uint32_t nameLen = (uint32_t)strlen(data->voicegroupName);

    if (stream->write(stream, &rootLen, sizeof(rootLen)) != sizeof(rootLen)) return false;
    if (rootLen > 0 && stream->write(stream, data->projectRoot, rootLen) != (int64_t)rootLen) return false;
    if (stream->write(stream, &nameLen, sizeof(nameLen)) != sizeof(nameLen)) return false;
    if (nameLen > 0 && stream->write(stream, data->voicegroupName, nameLen) != (int64_t)nameLen) return false;
    if (stream->write(stream, &data->reverbAmount, 1) != 1) return false;
    if (stream->write(stream, &data->masterVolume, 1) != 1) return false;
    if (stream->write(stream, &data->songMasterVolume, 1) != 1) return false;
    uint8_t analogFilterByte = data->analogFilter ? 1 : 0;
    if (stream->write(stream, &analogFilterByte, 1) != 1) return false;
    if (stream->write(stream, &data->maxPcmChannels, 1) != 1) return false;

    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    /* Snapshot current voicegroup identity to detect changes after load */
    char prevRoot[sizeof(data->projectRoot)];
    char prevName[sizeof(data->voicegroupName)];
    memcpy(prevRoot, data->projectRoot,    sizeof(prevRoot));
    memcpy(prevName, data->voicegroupName, sizeof(prevName));

    uint32_t rootLen, nameLen;

    if (stream->read(stream, &rootLen, sizeof(rootLen)) != sizeof(rootLen)) return false;
    if (rootLen >= sizeof(data->projectRoot)) return false;
    if (rootLen > 0 && stream->read(stream, data->projectRoot, rootLen) != (int64_t)rootLen) return false;
    data->projectRoot[rootLen] = '\0';

    if (stream->read(stream, &nameLen, sizeof(nameLen)) != sizeof(nameLen)) return false;
    if (nameLen >= sizeof(data->voicegroupName)) return false;
    if (nameLen > 0 && stream->read(stream, data->voicegroupName, nameLen) != (int64_t)nameLen) return false;
    data->voicegroupName[nameLen] = '\0';

    if (stream->read(stream, &data->reverbAmount, 1) != 1) return false;
    if (stream->read(stream, &data->masterVolume, 1) != 1) return false;
    if (stream->read(stream, &data->songMasterVolume, 1) != 1) return false;
    /* analogFilter byte is optional (not present in older saves); default to enabled */
    uint8_t analogFilterByte = 1;
    stream->read(stream, &analogFilterByte, 1);
    data->analogFilter = (analogFilterByte != 0);
    /* maxPcmChannels byte is optional (not present in older saves); default to 5 */
    uint8_t maxChannelsByte = 5;
    stream->read(stream, &maxChannelsByte, 1);
    if (maxChannelsByte < 1) maxChannelsByte = 1;
    if (maxChannelsByte > MAX_PCM_CHANNELS) maxChannelsByte = MAX_PCM_CHANNELS;
    data->maxPcmChannels = maxChannelsByte;

    if (data->activated) {
        /* Only reload voicegroup if the project root or name actually changed */
        bool vgChanged = strcmp(data->projectRoot,    prevRoot) != 0 ||
                         strcmp(data->voicegroupName, prevName) != 0;
        if (vgChanged && data->projectRoot[0] && data->voicegroupName[0]) {
            if (data->loadedVg) {
                voicegroup_free(data->loadedVg);
                data->loadedVg = NULL;
            }
            data->loadedVg = voicegroup_load(data->projectRoot, data->voicegroupName,
                                             &data->loaderConfig);
            if (data->loadedVg)
                m4a_engine_set_voicegroup(&data->engine, data->loadedVg->voices);
        }
        data->engine.masterVolume = data->masterVolume;
        data->engine.songMasterVolume = data->songMasterVolume;
        data->engine.analogFilter = data->analogFilter;
        data->engine.maxPcmChannels = data->maxPcmChannels;
        m4a_reverb_set_amount(&data->engine.reverb, data->reverbAmount);
    }

    /* Push restored values into the GUI so it reflects the loaded state */
    if (data->gui) {
        M4AGuiSettings gs;
        memset(&gs, 0, sizeof(gs));
        snprintf(gs.projectRoot,    sizeof(gs.projectRoot),    "%s", data->projectRoot);
        snprintf(gs.voicegroupName, sizeof(gs.voicegroupName), "%s", data->voicegroupName);
        gs.reverbAmount     = data->reverbAmount;
        gs.masterVolume     = data->masterVolume;
        gs.songMasterVolume = data->songMasterVolume;
        gs.analogFilter     = data->analogFilter;
        gs.maxPcmChannels   = data->maxPcmChannels;
        gs.voicegroupLoaded = (data->loadedVg != NULL);
        m4a_gui_update_settings(data->gui, &gs);
    }

    return true;
}

static const clap_plugin_state_t s_state = {
    .save = state_save,
    .load = state_load,
};

/* ---- GUI extension ---- */

static void plugin_log(const char *fmt, ...)
{
    if (!s_pluginLogPath) return;
    FILE *f = fopen(s_pluginLogPath, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static bool gui_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    (void)plugin;
#if defined(_WIN32)
    /* Support Win32 embedded (preferred by Reaper) and all floating */
    bool supported = is_floating ||
                     (!is_floating && api && strcmp(api, CLAP_WINDOW_API_WIN32) == 0);
#else
    bool supported = is_floating;
#endif
    plugin_log("gui_is_api_supported: api=%s floating=%d -> %d",
               api ? api : "(null)", is_floating, supported);
    return supported;
}

static bool gui_get_preferred_api(const clap_plugin_t *plugin,
                                  const char **api, bool *is_floating)
{
    (void)plugin;
#if defined(_WIN32)
    /* Prefer embedded on Windows so hosts like Reaper show us in-line */
    *api = CLAP_WINDOW_API_WIN32;
    *is_floating = false;
#elif defined(__APPLE__)
    *api = CLAP_WINDOW_API_COCOA;
    *is_floating = true;
#else
    *api = CLAP_WINDOW_API_X11;
    *is_floating = true;
#endif
    return true;
}

static bool gui_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    plugin_log("gui_create: api=%s floating=%d", api ? api : "(null)", is_floating);
    (void)api;
#if !defined(_WIN32)
    /* On non-Windows platforms we only support floating windows */
    if (!is_floating) {
        plugin_log("gui_create: rejecting (not floating, non-Windows)");
        return false;
    }
#endif

    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

    M4AGuiSettings gs;
    memset(&gs, 0, sizeof(gs));
    snprintf(gs.projectRoot,    sizeof(gs.projectRoot),    "%s", data->projectRoot);
    snprintf(gs.voicegroupName, sizeof(gs.voicegroupName), "%s", data->voicegroupName);
    gs.reverbAmount     = data->reverbAmount;
    gs.masterVolume     = data->masterVolume;
    gs.songMasterVolume = data->songMasterVolume;
    gs.analogFilter     = data->analogFilter;
    gs.maxPcmChannels   = data->maxPcmChannels;
    gs.voicegroupLoaded = (data->loadedVg != NULL);

    data->gui = m4a_gui_create(data->host, &gs, s_pluginLogPath);
    if (!data->gui) {
        plugin_log("gui_create: m4a_gui_create() returned NULL");
        return false;
    }
    plugin_log("gui_create: success");

    /* Register a ~60 Hz timer to drive GUI rendering */
    const clap_host_timer_support_t *timerExt =
        (const clap_host_timer_support_t *)data->host->get_extension(
            data->host, CLAP_EXT_TIMER_SUPPORT);
    if (timerExt)
        timerExt->register_timer(data->host, 16 /* ms */, &data->guiTimerId);

    return true;
}

static void gui_destroy(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    if (!data->gui)
        return;

    /* Unregister the render timer */
    if (data->guiTimerId != CLAP_INVALID_ID) {
        const clap_host_timer_support_t *timerExt =
            (const clap_host_timer_support_t *)data->host->get_extension(
                data->host, CLAP_EXT_TIMER_SUPPORT);
        if (timerExt)
            timerExt->unregister_timer(data->host, data->guiTimerId);
        data->guiTimerId = CLAP_INVALID_ID;
    }

    m4a_gui_destroy(data->gui);
    data->gui = NULL;
}

static bool gui_set_scale(const clap_plugin_t *plugin, double scale)
{
    (void)plugin;
    (void)scale;
    return false; /* GLFW handles DPI internally */
}

static bool gui_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_gui_get_size(data->gui, width, height);
    return true;
}

static bool gui_can_resize(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return m4a_gui_can_resize(data->gui);
}

static bool gui_get_resize_hints(const clap_plugin_t *plugin,
                                 clap_gui_resize_hints_t *hints)
{
    (void)plugin;
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically   = true;
    hints->preserve_aspect_ratio   = false;
    return true;
}

static bool gui_adjust_size(const clap_plugin_t *plugin,
                            uint32_t *width, uint32_t *height)
{
    /* Accept any size the host offers */
    (void)plugin;
    (void)width;
    (void)height;
    return true;
}

static bool gui_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return m4a_gui_set_size(data->gui, width, height);
}

static bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
    plugin_log("gui_set_parent called");
#if defined(_WIN32)
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return m4a_gui_set_parent_win32(data->gui, window->win32);
#else
    (void)plugin;
    (void)window;
    return false;
#endif
}

static bool gui_set_transient(const clap_plugin_t *plugin, const clap_window_t *window)
{
    (void)plugin;
    (void)window;
    return true;
}

static void gui_suggest_title(const clap_plugin_t *plugin, const char *title)
{
    (void)plugin;
    (void)title;
}

static bool gui_show(const clap_plugin_t *plugin)
{
    plugin_log("gui_show called");
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return m4a_gui_show(data->gui);
}

static bool gui_hide(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    return m4a_gui_hide(data->gui);
}

static const clap_plugin_gui_t s_gui = {
    .is_api_supported  = gui_is_api_supported,
    .get_preferred_api = gui_get_preferred_api,
    .create            = gui_create,
    .destroy           = gui_destroy,
    .set_scale         = gui_set_scale,
    .get_size          = gui_get_size,
    .can_resize        = gui_can_resize,
    .get_resize_hints  = gui_get_resize_hints,
    .adjust_size       = gui_adjust_size,
    .set_size          = gui_set_size,
    .set_parent        = gui_set_parent,
    .set_transient     = gui_set_transient,
    .suggest_title     = gui_suggest_title,
    .show              = gui_show,
    .hide              = gui_hide,
};

/* ---- Timer support extension ---- */

static void timer_on_timer(const clap_plugin_t *plugin, clap_id timer_id)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    if (!data->gui || timer_id != data->guiTimerId)
        return;

    /* Render one GUI frame */
    m4a_gui_tick(data->gui);

    /* Apply any settings the user changed */
    M4AGuiSettings gs;
    bool reloadVoicegroup = false;
    if (!m4a_gui_poll_changes(data->gui, &gs, &reloadVoicegroup))
        return;

    /* Immediate audio settings - safe to write since they're byte-sized */
    data->reverbAmount     = gs.reverbAmount;
    data->masterVolume     = gs.masterVolume;
    data->songMasterVolume = gs.songMasterVolume;
    data->analogFilter     = gs.analogFilter;
    data->maxPcmChannels   = gs.maxPcmChannels;

    if (data->activated) {
        data->engine.masterVolume = gs.masterVolume;
        m4a_engine_set_song_volume(&data->engine, gs.songMasterVolume);
        m4a_reverb_set_amount(&data->engine.reverb, gs.reverbAmount);
        data->engine.analogFilter = gs.analogFilter;
        data->engine.maxPcmChannels = gs.maxPcmChannels;
    }

    if (reloadVoicegroup) {
        /* Update paths, then ask the host to deactivate/reactivate so the new
         * voicegroup is loaded cleanly from the audio thread's perspective. */
        snprintf(data->projectRoot,    sizeof(data->projectRoot),
                 "%s", gs.projectRoot);
        snprintf(data->voicegroupName, sizeof(data->voicegroupName),
                 "%s", gs.voicegroupName);
        data->host->request_restart(data->host);
    }

    /* Register this change with the host's undo stack.
     *
     * Prefer the CLAP undo draft extension when available: pass no delta so
     * the host snapshots state via state->save()/state->load().
     *
     * Fall back to mark_dirty() for hosts (e.g. Reaper) that don't implement
     * the draft extension. Per the CLAP spec, mark_dirty() creates an implicit
     * undo step as long as the plugin hasn't opted into CLAP_EXT_UNDO. */
    const clap_host_undo_t *hostUndo =
        (const clap_host_undo_t *)data->host->get_extension(data->host, CLAP_EXT_UNDO);
    if (hostUndo && hostUndo->change_made) {
        const char *name = reloadVoicegroup ? "M4A: Reload Voicegroup"
                                            : "M4A: Settings Change";
        hostUndo->change_made(data->host, name, NULL, 0, false);
    } else {
        const clap_host_state_t *hostState =
            (const clap_host_state_t *)data->host->get_extension(data->host, CLAP_EXT_STATE);
        if (hostState && hostState->mark_dirty)
            hostState->mark_dirty(data->host);
    }

    /* Reflect updated status back into the GUI (voicegroupLoaded may change
     * after request_restart completes, but update the rest immediately). */
    gs.voicegroupLoaded = (data->loadedVg != NULL);
    m4a_gui_update_settings(data->gui, &gs);
}

static const clap_plugin_timer_support_t s_timer_support = {
    .on_timer = timer_on_timer,
};

/* Extension dispatcher */
static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)   return &s_audio_ports;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)    return &s_note_ports;
    if (strcmp(id, CLAP_EXT_STATE) == 0)          return &s_state;
    if (strcmp(id, CLAP_EXT_GUI) == 0)            return &s_gui;
    if (strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)  return &s_timer_support;
    return NULL;
}

static void plugin_on_main_thread(const clap_plugin_t *plugin)
{
}

/* ---- Factory ---- */

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t *factory)
{
    return 1;
}

static const clap_plugin_descriptor_t *factory_get_plugin_descriptor(
    const clap_plugin_factory_t *factory, uint32_t index)
{
    if (index == 0) return &s_descriptor;
    return NULL;
}

static const clap_plugin_t *factory_create_plugin(
    const clap_plugin_factory_t *factory, const clap_host_t *host, const char *plugin_id)
{
    if (strcmp(plugin_id, s_descriptor.id) != 0)
        return NULL;

    M4APluginData *data = calloc(1, sizeof(M4APluginData));
    if (!data) return NULL;

    data->host = host;

    clap_plugin_t *plugin = calloc(1, sizeof(clap_plugin_t));
    if (!plugin) {
        free(data);
        return NULL;
    }

    plugin->desc = &s_descriptor;
    plugin->plugin_data = data;
    plugin->init = plugin_init;
    plugin->destroy = plugin_destroy;
    plugin->activate = plugin_activate;
    plugin->deactivate = plugin_deactivate;
    plugin->start_processing = plugin_start_processing;
    plugin->stop_processing = plugin_stop_processing;
    plugin->reset = plugin_reset;
    plugin->process = plugin_process;
    plugin->get_extension = plugin_get_extension;
    plugin->on_main_thread = plugin_on_main_thread;

    return plugin;
}

static const clap_plugin_factory_t s_factory = {
    .get_plugin_count = factory_get_plugin_count,
    .get_plugin_descriptor = factory_get_plugin_descriptor,
    .create_plugin = factory_create_plugin,
};

/* ---- Entry point ---- */

static bool entry_init(const char *plugin_path)
{
    if (plugin_path && plugin_path[0]) {
        /* Extract directory portion: everything up to the last separator */
        const char *end = plugin_path + strlen(plugin_path);
        while (end > plugin_path && *end != '/' && *end != '\\')
            end--;
        if (end > plugin_path) {
            size_t dirLen = (size_t)(end - plugin_path);
            if (dirLen >= sizeof(s_pluginDir))
                dirLen = sizeof(s_pluginDir) - 1;
            memcpy(s_pluginDir, plugin_path, dirLen);
            s_pluginDir[dirLen] = '\0';
        }

#ifdef __APPLE__
        /* On macOS the binary lives at <bundle>.clap/Contents/MacOS/<binary>.
         * The cfg file should sit next to the bundle, not inside it, so
         * navigate up two levels: Contents/MacOS -> Contents -> bundle root,
         * then one more to the directory that contains the bundle. */
        {
            char *p = s_pluginDir;
            size_t len = strlen(p);
            /* Check suffix .../Contents/MacOS (case-sensitive on macOS) */
            const char *suffix = "/Contents/MacOS";
            size_t slen = strlen(suffix);
            if (len > slen && strcmp(p + len - slen, suffix) == 0) {
                /* Strip /Contents/MacOS to get the bundle root */
                p[len - slen] = '\0';
                /* Strip the bundle name (.clap dir) to get the install dir */
                char *last = p + strlen(p);
                while (last > p && *last != '/')
                    last--;
                if (last > p)
                    *last = '\0';
            }
        }
#endif
    }
    return true;
}

static void entry_deinit(void)
{
}

static const void *entry_get_factory(const char *factory_id)
{
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &s_factory;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
