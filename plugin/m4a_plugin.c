#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <clap/clap.h>
#include "m4a_plugin.h"
#include "m4a_engine.h"
#include "m4a_channel.h"
#include "m4a_reverb.h"
#include "voicegroup_loader.h"

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
    .id = "com.pokeemerald.m4a-plugin",
    .name = "M4A GBA Sound Engine",
    .vendor = "pokeemerald",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0",
    .description = "GBA m4a sound engine plugin for pokeemerald music preview",
    .features = s_features,
};

/* ---- Plugin lifecycle ---- */

static bool plugin_init(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    data->masterVolume = 15;
    data->reverbAmount = 0;
    data->projectRoot[0] = '\0';
    data->voicegroupName[0] = '\0';
    data->loadedVg = NULL;
    data->activated = false;
    return true;
}

static void plugin_destroy(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
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
    m4a_reverb_set_amount(&data->engine.reverb, data->reverbAmount);

    /* If voicegroup is configured, load it */
    if (data->projectRoot[0] && data->voicegroupName[0]) {
        if (data->loadedVg) {
            voicegroup_free(data->loadedVg);
            data->loadedVg = NULL;
        }
        data->loadedVg = voicegroup_load(data->projectRoot, data->voicegroupName);
        if (data->loadedVg) {
            m4a_engine_set_voicegroup(&data->engine, data->loadedVg->voices);
        }
    }

    data->activated = true;
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
}

static void plugin_reset(const clap_plugin_t *plugin)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;
    m4a_engine_all_sound_off(&data->engine);
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

    return true;
}

static bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    M4APluginData *data = (M4APluginData *)plugin->plugin_data;

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

    /* If activated, reload voicegroup */
    if (data->activated && data->projectRoot[0] && data->voicegroupName[0]) {
        if (data->loadedVg) {
            voicegroup_free(data->loadedVg);
            data->loadedVg = NULL;
        }
        data->loadedVg = voicegroup_load(data->projectRoot, data->voicegroupName);
        if (data->loadedVg) {
            m4a_engine_set_voicegroup(&data->engine, data->loadedVg->voices);
        }
        data->engine.masterVolume = data->masterVolume;
        m4a_reverb_set_amount(&data->engine.reverb, data->reverbAmount);
    }

    return true;
}

static const clap_plugin_state_t s_state = {
    .save = state_save,
    .load = state_load,
};

/* Extension dispatcher */
static const void *plugin_get_extension(const clap_plugin_t *plugin, const char *id)
{
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &s_audio_ports;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &s_note_ports;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &s_state;
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
