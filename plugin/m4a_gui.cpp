/*
 * m4a_gui.cpp - Dear ImGui + Pugl GUI for the M4A plugin.
 *
 * Provides a simple settings panel where the user can change the project
 * root, voicegroup, reverb, and volume levels in real time from the DAW.
 *
 * Thread-safety: all functions must be called from the main thread.
 */

#include <pugl/pugl.h>
#include <pugl/gl.h>

#include "imgui.h"
#include "imgui_impl_pugl.h"
#include "imgui_impl_opengl3.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

/* Timer ID for the internal render timer 
    Bitwig does not give the plugin a timer, 
    so we use a timer from Pugl to drive GUI updates */
static const uintptr_t RENDER_TIMER_ID = 1;

/* ---- Debug logging ---- */
static const char *s_logPath = nullptr;

static void gui_log(const char *fmt, ...)
{
    if (!s_logPath) return;
    FILE *f = fopen(s_logPath, "a");
    if (!f) return;
    time_t t = time(nullptr);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&t));
    fprintf(f, "[%s] ", tbuf);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Our C interface */
#include "m4a_gui.h"
#include "m4a_engine.h"

/* CLAP GUI extension (for notifying host when floating window closes) */
#include <clap/ext/gui.h>

/* ---- Constants ---- */

static const int GUI_W = 540;
static const int GUI_H = 500;

/* ---- GUI state ---- */

struct M4AGuiState {
    PuglWorld     *world;
    PuglView      *view;
    ImGuiContext  *imguiCtx;
    const clap_host_t *host;

    bool           realized;   /* true after puglRealize succeeds */
    bool           glInited;   /* true after ImGui_ImplOpenGL3_Init */

    /* Cached size from PUGL_CONFIGURE */
    uint32_t       cachedWidth;
    uint32_t       cachedHeight;

    /* Currently displayed settings */
    M4AGuiSettings settings;

    /* Editable text buffers (not applied until "Reload" is clicked) */
    char projectRootBuf[512];
    char voicegroupBuf[256];

    /* Pending change flags (cleared by poll_changes) */
    bool settingsChanged;
    bool reloadRequested;

    /* True after set_parent() — host drives sizing and visibility */
    bool isEmbedded;

    /* True after the user closes the floating window */
    bool wasClosed;

    /* True when the internal pugl render timer is active */
    bool internalTimerActive;
    M4AGuiTimerCallback internalTimerCallback;
    void *internalTimerUserData;

    /* Voice editor state */
    ToneData *liveVoices;
    const ToneData *originalVoices;
    bool *voiceOverrides;
    const char (*voiceNames)[VG_VOICE_NAME_LEN]; /* per-voice display names */
    int selectedVoice;
    int pendingRestoreVoice;  /* -1 = none */
    bool voicesDirty;         /* set when any voice param is edited */

    /* Polyphony monitor: read-only engine view + per-track flash tracking.
     * prev* snapshots detect counter increases between frames so the table
     * row can flash the moment a track loses a sound. */
    M4AEngine *engine;
    uint32_t polyPrevDrop[MAX_TRACKS];
    uint32_t polyPrevSteal[MAX_TRACKS];
    uint32_t polyPrevTailCut[MAX_TRACKS];
    double   polyFlashTime[MAX_TRACKS]; /* ImGui::GetTime() of last increase */
    bool     polyPrevValid;             /* prev* snapshots initialized */
    bool     polyResetRequested;        /* Reset Counters clicked (cleared by poll) */
};

/* ---- Internal helpers ---- */

static void sync_buffers(M4AGuiState *gui)
{
    snprintf(gui->projectRootBuf, sizeof(gui->projectRootBuf),
             "%s", gui->settings.projectRoot);
    snprintf(gui->voicegroupBuf, sizeof(gui->voicegroupBuf),
             "%s", gui->settings.voicegroupName);
}

/* ---- Voice type helpers ---- */

static const char *voice_type_name(uint8_t type)
{
    uint8_t base = type & ~VOICE_TYPE_FIX;
    switch (base) {
    case 0x00: return "DirectSound";
    case 0x01: return "Square 1";
    case 0x02: return "Square 2";
    case 0x03: return "Prog Wave";
    case 0x04: return "Noise";
    case VOICE_CRY:          return "Cry";
    case VOICE_CRY_REVERSE:  return "Cry (Reverse)";
    case VOICE_KEYSPLIT:     return "Keysplit";
    case VOICE_KEYSPLIT_ALL: return "Drum Kit";
    default: return "Unknown";
    }
}

/* Edit ADSR for DirectSound voices (0-255 range). Returns true if changed. */
static bool edit_directsound_adsr(ToneData *voice)
{
    bool changed = false;
    int a = voice->attack, d = voice->decay, s = voice->sustain, r = voice->release;
    if (ImGui::SliderInt("Attack##ds", &a, 0, 255))  { voice->attack  = (uint8_t)a; changed = true; }
    if (ImGui::SliderInt("Decay##ds",  &d, 0, 255))  { voice->decay   = (uint8_t)d; changed = true; }
    if (ImGui::SliderInt("Sustain##ds",&s, 0, 255))   { voice->sustain = (uint8_t)s; changed = true; }
    if (ImGui::SliderInt("Release##ds",&r, 0, 255))   { voice->release = (uint8_t)r; changed = true; }
    return changed;
}

/* Edit ADSR for CGB voices (limited range). Returns true if changed. */
static bool edit_cgb_adsr(ToneData *voice)
{
    bool changed = false;
    int a = voice->attack, d = voice->decay, s = voice->sustain, r = voice->release;
    if (ImGui::SliderInt("Attack##cgb", &a, 0, 7))   { voice->attack  = (uint8_t)a; changed = true; }
    if (ImGui::SliderInt("Decay##cgb",  &d, 0, 7))   { voice->decay   = (uint8_t)d; changed = true; }
    if (ImGui::SliderInt("Sustain##cgb",&s, 0, 15))   { voice->sustain = (uint8_t)s; changed = true; }
    if (ImGui::SliderInt("Release##cgb",&r, 0, 7))    { voice->release = (uint8_t)r; changed = true; }
    return changed;
}

/* ---- Tab rendering ---- */

static void render_general_tab(M4AGuiState *gui)
{
    /* ---- Project Settings ---- */
    ImGui::SeparatorText("Project Settings");

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Project Root:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##root", gui->projectRootBuf, sizeof(gui->projectRootBuf));

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Voicegroup:  ");
    ImGui::SameLine();
    {
        float btnW = 80.0f;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - spacing);
    }
    ImGui::InputText("##vg", gui->voicegroupBuf, sizeof(gui->voicegroupBuf));
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(80, 0))) {
        snprintf(gui->settings.projectRoot,    sizeof(gui->settings.projectRoot),
                 "%s", gui->projectRootBuf);
        snprintf(gui->settings.voicegroupName, sizeof(gui->settings.voicegroupName),
                 "%s", gui->voicegroupBuf);
        gui->settingsChanged = true;
        gui->reloadRequested = true;
    }

    /* Voicegroup load status */
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Status:      ");
    ImGui::SameLine();
    if (gui->settings.voicegroupLoaded)
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Voicegroup loaded");
    else
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "Voicegroup not loaded");

    ImGui::Spacing();

    /* ---- Audio Settings ---- */
    ImGui::SeparatorText("Audio Settings");
    {
        int v = (int)gui->settings.songMasterVolume;
        if (ImGui::SliderInt("Song Volume (0-127)", &v, 0, 127)) {
            gui->settings.songMasterVolume = (uint8_t)v;
            gui->settingsChanged = true;
        }
    }
    {
        int v = (int)gui->settings.reverbAmount;
        if (ImGui::SliderInt("Reverb (0-127)", &v, 0, 127)) {
            gui->settings.reverbAmount = (uint8_t)v;
            gui->settingsChanged = true;
        }
    }
    {
        int v = (int)gui->settings.maxPcmChannels;
        if (ImGui::SliderInt("Polyphony (1-15)", &v, 1, MAX_PCM_CHANNELS)) {
            gui->settings.maxPcmChannels = (uint8_t)v;
            gui->settingsChanged = true;
        }
    }

    {
        /* DirectSound (PCM) mixing rate.  The m4a engine mixes PCM at a low-ish sample rate,
         * so high notes alias audibly; higher rates progressively reduce that. */
        static const char  *mixRateNames[]  = {
            "13379 Hz (m4a's default)", "21024 Hz", "31536 Hz", "42048 Hz",
            "Host rate (clean, no aliasing)"
        };
        static const float  mixRateValues[] = {
            13379.0f, 21024.0f, 31536.0f, 42048.0f, 0.0f
        };
        const int mixRateCount = (int)(sizeof(mixRateValues) / sizeof(mixRateValues[0]));

        int curIdx = -1;
        for (int k = 0; k < mixRateCount; k++) {
            if (gui->settings.pcmMixRate == mixRateValues[k]) { curIdx = k; break; }
        }
        const char *preview = (curIdx >= 0) ? mixRateNames[curIdx] : "Custom";
        if (ImGui::BeginCombo("PCM Mix Rate", preview)) {
            for (int k = 0; k < mixRateCount; k++) {
                bool selected = (k == curIdx);
                if (ImGui::Selectable(mixRateNames[k], selected)) {
                    gui->settings.pcmMixRate = mixRateValues[k];
                    gui->settingsChanged = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip(
            "Rate the DirectSound (PCM) channels are mixed at before upsampling.\n"
            "The m4a engine uses 13379 Hz by default, so high notes have aliasing.\n"
            "Higher rates sound better, but are less accurate compared to in-game.");
    }

    if (ImGui::Checkbox("GBA Analog Filter", &gui->settings.analogFilter))
        gui->settingsChanged = true;
}

static void render_voices_tab(M4AGuiState *gui)
{
    if (!gui->liveVoices) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "No voicegroup loaded");
        return;
    }

    /* Voice selector */
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    ImGui::SliderInt("##voiceSlider", &gui->selectedVoice, 0, 127);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##voiceInput", &gui->selectedVoice, 1, 10);
    if (gui->selectedVoice < 0) gui->selectedVoice = 0;
    if (gui->selectedVoice > 127) gui->selectedVoice = 127;

    int idx = gui->selectedVoice;
    ToneData *voice = &gui->liveVoices[idx];
    uint8_t type = voice->type;

    /* Type label */
    ImGui::Text("Type: %s (0x%02X)", voice_type_name(type), type);
    if (type == VOICE_DIRECTSOUND_NO_RESAMPLE) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "[Fixed]");
    }

    /* Modified indicator */
    if (gui->voiceOverrides[idx]) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "(modified)");
    }

    ImGui::Separator();

    bool changed = false;
    uint8_t baseType = type & ~VOICE_TYPE_FIX;

    /* Dispatch to per-type editor */
    if (baseType == 0x00) {
        /* DirectSound — key and panSweep are metadata only */
        ImGui::Text("Key: %d", voice->key);
        ImGui::Text("Pan/Sweep: %d (0x%02X)", voice->panSweep, voice->panSweep);
        changed |= edit_directsound_adsr(voice);

        /* Read-only sample info */
        if (voice->wav) {
            ImGui::Spacing();
            ImGui::SeparatorText("Sample Info");
            ImGui::Text("Size: %u samples", voice->wav->size);
            ImGui::Text("Frequency: %u Hz", voice->wav->freq);
            ImGui::Text("Loop: %s (start: %u)", (voice->wav->status & 0x4000) ? "Yes" : "No", voice->wav->loopStart);
        }
    } else if (baseType == 0x01) {
        /* Square 1 */
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        int sweep = voice->panSweep;
        if (ImGui::SliderInt("Sweep", &sweep, 0, 127)) { voice->panSweep = (uint8_t)sweep; changed = true; }
        int duty = (int)(uintptr_t)voice->wavePointer & 0x03;
        const char *dutyNames[] = { "12.5%", "25%", "50%", "75%" };
        if (ImGui::Combo("Duty Cycle", &duty, dutyNames, 4)) {
            voice->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
            changed = true;
        }
        changed |= edit_cgb_adsr(voice);
    } else if (baseType == 0x02) {
        /* Square 2 */
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        int duty = (int)(uintptr_t)voice->wavePointer & 0x03;
        const char *dutyNames[] = { "12.5%", "25%", "50%", "75%" };
        if (ImGui::Combo("Duty Cycle", &duty, dutyNames, 4)) {
            voice->wavePointer = (uint32_t *)(uintptr_t)(duty & 0x03);
            changed = true;
        }
        changed |= edit_cgb_adsr(voice);
    } else if (baseType == 0x03) {
        /* Programmable Wave */
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        changed |= edit_cgb_adsr(voice);
    } else if (baseType == 0x04) {
        /* Noise */
        int key = voice->key;
        if (ImGui::SliderInt("Key", &key, 0, 127)) { voice->key = (uint8_t)key; changed = true; }
        int period = (int)(uintptr_t)voice->wavePointer & 0x01;
        const char *periodNames[] = { "Normal (15-bit)", "Metallic (7-bit)" };
        if (ImGui::Combo("Period", &period, periodNames, 2)) {
            voice->wavePointer = (uint32_t *)(uintptr_t)(period & 0x01);
            changed = true;
        }
        changed |= edit_cgb_adsr(voice);
    } else if (baseType == VOICE_CRY || baseType == VOICE_CRY_REVERSE) {
        /* Cry — read-only display */
        ImGui::Text("Key: %d", voice->key);
        ImGui::Text("Attack: %d  Decay: %d  Sustain: %d  Release: %d",
                     voice->attack, voice->decay, voice->sustain, voice->release);
        ImGui::TextDisabled("(Cry voices are read-only)");
    } else if (baseType == VOICE_KEYSPLIT) {
        ImGui::TextDisabled("(Keysplit voice — sub-voice editing not supported)");
    } else if (baseType == VOICE_KEYSPLIT_ALL) {
        ImGui::TextDisabled("(Drum Kit voice — sub-voice editing not supported)");
    } else {
        ImGui::TextDisabled("(Unknown voice type)");
    }

    if (changed) {
        gui->voiceOverrides[idx] = true;
        gui->voicesDirty = true;
    }

    /* Restore button */
    if (gui->voiceOverrides[idx]) {
        ImGui::Spacing();
        if (ImGui::Button("Restore Original")) {
            gui->pendingRestoreVoice = idx;
        }
    }
}

/* ---- Polyphony monitor tab ---- */

static void midi_note_name(uint8_t key, char *buf, size_t n)
{
    static const char *names[12] = { "C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B" };
    snprintf(buf, n, "%s%d", names[key % 12], (int)(key / 12) - 1);
}

/* One fixed-size channel cell, colored by state:
 * 0 = free (dark), 1 = active (green), 2 = releasing (amber), 3 = lost sound
 * playing on a shadow channel (blue).  `id` disambiguates the ImGui ID. */
static void channel_cell(const char *id, const char *label, int state)
{
    static const ImVec4 colors[4] = {
        ImVec4(0.20f, 0.20f, 0.23f, 1.0f),
        ImVec4(0.15f, 0.55f, 0.22f, 1.0f),
        ImVec4(0.72f, 0.53f, 0.10f, 1.0f),
        ImVec4(0.16f, 0.38f, 0.75f, 1.0f),
    };
    char text[80];
    snprintf(text, sizeof(text), "%s###%s", label, id);
    ImGui::PushStyleColor(ImGuiCol_Button, colors[state & 3]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[state & 3]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[state & 3]);
    ImGui::Button(text, ImVec2(52, 40));
    ImGui::PopStyleColor(3);
}

/* Cell state/label for one PCM channel slot. */
static int pcm_cell_state(const M4APCMChannel *ch, bool shadow, char *label, size_t n)
{
    if (!(ch->status & CHN_ON)) {
        snprintf(label, n, "--");
        return 0;
    }
    char note[8];
    midi_note_name(ch->midiKey, note, sizeof(note));
    snprintf(label, n, "T%d\n%s", ch->trackIndex + 1, note);
    if (shadow)
        return 3;
    return (ch->status & (CHN_STOP | CHN_IEC)) ? 2 : 1;
}

static int cgb_cell_state(const M4ACGBChannel *ch, bool shadow, const char *name,
                          char *label, size_t n)
{
    if (!(ch->status & CHN_ON)) {
        snprintf(label, n, "%s\n--", name);
        return 0;
    }
    char note[8];
    midi_note_name(ch->midiKey, note, sizeof(note));
    snprintf(label, n, "%s\nT%d %s", name, ch->trackIndex + 1, note);
    if (shadow)
        return 3;
    return (ch->status & (CHN_STOP | CHN_IEC)) ? 2 : 1;
}

/* Resolve a display name for the instrument on a voicegroup program slot:
 * the loader-provided symbol name when available, else the voice type name,
 * else just the program number. */
static const char *poly_instrument_name(M4AGuiState *gui, uint8_t program,
                                        char *buf, size_t n)
{
    int idx = program & 0x7F;
    if (gui->voiceNames && gui->voiceNames[idx][0])
        return gui->voiceNames[idx];
    if (gui->liveVoices) {
        snprintf(buf, n, "%s", voice_type_name(gui->liveVoices[idx].type));
        return buf;
    }
    snprintf(buf, n, "prog %d", idx);
    return buf;
}

static void render_polyphony_tab(M4AGuiState *gui)
{
    M4AEngine *eng = gui->engine;
    if (!eng || eng->maxPcmChannels == 0) {
        ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "Engine not running");
        return;
    }

    static const char *cgbNames[MAX_CGB_CHANNELS] = { "Sq1", "Sq2", "Wave", "Noise" };
    const double now = ImGui::GetTime();

    /* ---- Detect per-track counter increases for the flash effect ---- */
    for (int t = 0; t < MAX_TRACKS; t++) {
        uint32_t d = eng->polyDropCount[t];
        uint32_t s = eng->polyStealCount[t];
        uint32_t c = eng->polyTailCutCount[t];
        if (gui->polyPrevValid
            && (d > gui->polyPrevDrop[t] || s > gui->polyPrevSteal[t]
                || c > gui->polyPrevTailCut[t]))
            gui->polyFlashTime[t] = now;
        gui->polyPrevDrop[t] = d;
        gui->polyPrevSteal[t] = s;
        gui->polyPrevTailCut[t] = c;
    }
    gui->polyPrevValid = true;

    /* ---- Solo-overflow (invert) toggle ---- */
    if (ImGui::Checkbox("Solo overflow (invert audio)", &gui->settings.polyDebugInvert))
        gui->settingsChanged = true;
    ImGui::SetItemTooltip(
        "Mutes normal playback and makes ONLY the sounds lost to the polyphony\n"
        "limit audible.");

    ImGui::Spacing();

    /* ---- Live channel usage ---- */
    if (ImGui::CollapsingHeader("Channel Usage", ImGuiTreeNodeFlags_DefaultOpen)) {
        char label[48];
        for (int i = 0; i < eng->maxPcmChannels; i++) {
            char id[16];
            snprintf(id, sizeof(id), "pcm%d", i);
            int state = pcm_cell_state(&eng->pcmChannels[i], false, label, sizeof(label));
            if (i > 0) ImGui::SameLine();
            channel_cell(id, label, state);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(" PCM");
        for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
            char id[16];
            snprintf(id, sizeof(id), "cgb%d", i);
            int state = cgb_cell_state(&eng->cgbChannels[i], false, cgbNames[i],
                                       label, sizeof(label));
            if (i > 0) ImGui::SameLine();
            channel_cell(id, label, state);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(" CGB");

        /* In invert mode, also show the shadow pool: the lost sounds being played. */
        if (gui->settings.polyDebugInvert) {
            ImGui::Spacing();
            ImGui::TextDisabled("Lost sounds currently playing (solo overflow):");
            for (int i = 0; i < MAX_PCM_CHANNELS; i++) {
                char id[16];
                snprintf(id, sizeof(id), "spcm%d", i);
                int state = pcm_cell_state(&eng->pcmChannels[MAX_PCM_CHANNELS + i], true,
                                           label, sizeof(label));
                if (i > 0) ImGui::SameLine();
                channel_cell(id, label, state);
            }
            for (int i = 0; i < MAX_CGB_CHANNELS; i++) {
                char id[16];
                snprintf(id, sizeof(id), "scgb%d", i);
                int state = cgb_cell_state(&eng->cgbChannels[MAX_CGB_CHANNELS + i], true,
                                           cgbNames[i], label, sizeof(label));
                if (i > 0) ImGui::SameLine();
                channel_cell(id, label, state);
            }
        }
        ImGui::Spacing();
    }

    /* ---- Per-track overflow counters ---- */
    if (ImGui::CollapsingHeader("Overflow by Track", ImGuiTreeNodeFlags_DefaultOpen)) {
    /* The reset itself is performed by the plugin (via m4a_gui_poll_poly_reset)
     * so the GUI only ever reads engine state, never mutates it.  The prev*
     * snapshots need no clearing here: they re-sync from the live counters
     * every frame, and the flash only triggers on increases. */
    if (ImGui::Button("Reset Counters")) {
        gui->polyResetRequested = true;
        for (int t = 0; t < MAX_TRACKS; t++)
            gui->polyFlashTime[t] = 0.0;
    }

    bool anyRows = false;
    if (ImGui::BeginTable("##polyTable", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        /* Submit the header row cell-by-cell (instead of TableHeadersRow) so
         * each column label can carry an explanatory hover tooltip. */
        static const char *columns[4] = { "Track", "Dropped", "Cut Off", "Tail Cut" };
        static const char *columnHelp[4] = {
            "Track (MIDI channel) whose sound was lost.",
            "Notes that never played at all: every channel was in use and\n"
            "none had low enough priority to steal.  The most audible kind\n"
            "of overflow because the note is simply missing.",
            "Notes that were still sounding when a newer note stole their\n"
            "channel, cutting them off abruptly before their note-off.",
            "Notes that were already fading out (released) when a newer note\n"
            "reused their channel.  This shortens the fade-out tail and is\n"
            "usually the least audible kind of overflow.",
        };
        for (int c = 0; c < 4; c++)
            ImGui::TableSetupColumn(columns[c]);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < 4; c++) {
            ImGui::TableSetColumnIndex(c);
            ImGui::TableHeader(columns[c]);
            ImGui::SetItemTooltip("%s", columnHelp[c]);
        }
        for (int t = 0; t < MAX_TRACKS; t++) {
            uint32_t d = gui->polyPrevDrop[t];
            uint32_t s = gui->polyPrevSteal[t];
            uint32_t c = gui->polyPrevTailCut[t];
            if (d == 0 && s == 0 && c == 0)
                continue;
            anyRows = true;
            ImGui::TableNextRow();
            /* Flash the row red for ~1s after a track loses a sound. */
            float flash = (float)(now - gui->polyFlashTime[t]);
            if (gui->polyFlashTime[t] > 0.0 && flash < 1.0f) {
                float alpha = 0.55f * (1.0f - flash);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImVec4(0.85f, 0.15f, 0.15f, alpha)));
            }
            ImGui::TableNextColumn(); ImGui::Text("%d", t + 1);
            ImGui::TableNextColumn(); ImGui::Text("%u", d);
            ImGui::TableNextColumn(); ImGui::Text("%u", s);
            ImGui::TableNextColumn(); ImGui::Text("%u", c);
        }
        ImGui::EndTable();
    }
    if (!anyRows)
        ImGui::TextDisabled("No overflow recorded");
    ImGui::Spacing();
    }

    /* ---- Recent events, newest first ---- */
    if (ImGui::CollapsingHeader("Recent Events", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginChild("##polyEvents", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            uint32_t total = eng->polyEventTotal;
            uint32_t shown = total < M4A_POLY_EVENT_CAPACITY ? total : M4A_POLY_EVENT_CAPACITY;
            if (shown == 0)
                ImGui::TextDisabled("No overflow events yet");
            for (uint32_t k = 0; k < shown; k++) {
                uint32_t idx = total - 1 - k;
                const M4APolyEvent *ev = &eng->polyEvents[idx % M4A_POLY_EVENT_CAPACITY];
                char note[8], nameBuf[32];
                midi_note_name(ev->midiKey, note, sizeof(note));
                const char *inst = poly_instrument_name(gui, ev->program,
                                                        nameBuf, sizeof(nameBuf));
                switch (ev->type) {
                case M4A_POLY_DROPPED:
                    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
                        "#%u  Trk %d  %-4s %s: dropped (no channel available)",
                        idx + 1, ev->trackIndex + 1, note, inst);
                    break;
                case M4A_POLY_STOLEN:
                    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                        "#%u  Trk %d  %-4s %s: cut off by Trk %d",
                        idx + 1, ev->trackIndex + 1, note, inst, ev->byTrack + 1);
                    break;
                case M4A_POLY_TAIL_CUT:
                    ImGui::TextDisabled(
                        "#%u  Trk %d  %-4s %s: release tail cut by Trk %d",
                        idx + 1, ev->trackIndex + 1, note, inst, ev->byTrack + 1);
                    break;
                }
            }
        }
        ImGui::EndChild();
    }
}

/* A toggleable menu item with a checkmark and a hover tooltip.  Flips *value
 * and flags the settings dirty when clicked.  The tooltip shows on hover even
 * while the menu popup is open (ImGui shows item tooltips inside menus). */
static void toggle_menu_item(M4AGuiState *gui, const char *label,
                             bool *value, const char *help)
{
    if (ImGui::MenuItem(label, nullptr, *value)) {
        *value = !*value;
        gui->settingsChanged = true;
    }
    if (help && help[0])
        ImGui::SetItemTooltip("%s", help);
}

/* Top menu bar.  "Options" holds the opt-in effect features as toggleable
 * (checkmarked) items, each with hover help describing what it does. */
static void render_menu_bar(M4AGuiState *gui)
{
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("Options")) {
        toggle_menu_item(gui, "Respect Base MIDI Key", &gui->settings.respectBaseMidiKey,
            "For PCM (DirectSound) instruments, treat the voice's key field as the\n"
            "sample's base MIDI note and transpose accordingly, so a pressed note\n"
            "plays at its intended pitch. Off matches stock pokeemerald behavior.");
        toggle_menu_item(gui, "Portamento", &gui->settings.portamentoEnabled,
            "Enable the portamento glide effect (CC 5 = glide time in ticks).\n"
            "Notes slide smoothly from the previous note's pitch to the new one.");
        toggle_menu_item(gui, "Pulse-Width Modulation", &gui->settings.pwmEnabled,
            "Enable pulse-width modulation on the CGB square channels\n"
            "(CC 0x17 = duty-cycle pattern, CC 0x19 = modulation speed).");
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

/* Render a single ImGui frame — called from PUGL_EXPOSE. */
static void render_frame(M4AGuiState *gui)
{
    ImGui::SetCurrentContext(gui->imguiCtx);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplPugl_NewFrame();
    ImGui::NewFrame();

    uint32_t fbW = gui->cachedWidth;
    uint32_t fbH = gui->cachedHeight;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)fbW, (float)fbH));

    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoTitleBar      |
        ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoCollapse      |
        ImGuiWindowFlags_MenuBar         |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##Main", nullptr, wflags);

    render_menu_bar(gui);

    /* ---- Plugin title ---- */
    ImGui::TextColored(ImVec4(0.3f, 0.75f, 1.0f, 1.0f), "poryaaaa");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 160.0f);
    ImGui::TextDisabled("pokeemerald");
    ImGui::Separator();
    ImGui::Spacing();

    /* ---- Tabbed content ---- */
    if (ImGui::BeginTabBar("##Tabs")) {
        if (ImGui::BeginTabItem("General")) {
            render_general_tab(gui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Voices")) {
            render_voices_tab(gui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Polyphony")) {
            render_polyphony_tab(gui);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();

    /* ---- Render ---- */
    ImGui::Render();
    glViewport(0, 0, (int)fbW, (int)fbH);
    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    /* Pugl handles buffer swap */
}

/* ---- Pugl event handler ---- */

static PuglStatus pugl_event_handler(PuglView *view, const PuglEvent *event)
{
    M4AGuiState *gui = (M4AGuiState *)puglGetHandle(view);
    if (!gui)
        return PUGL_SUCCESS;

    ImGui::SetCurrentContext(gui->imguiCtx);

    switch (event->type)
    {
    case PUGL_REALIZE:
        /* GL context is now current — initialize OpenGL ImGui backend */
        if (!gui->glInited) {
            ImGui_ImplOpenGL3_Init("#version 330 core");
            gui->glInited = true;
        }
        break;

    case PUGL_UNREALIZE:
        /* GL context is current — shut down OpenGL backend if still active.
         * NOTE: puglFreeView() on Windows does NOT dispatch PUGL_UNREALIZE,
         * so the explicit shutdown in m4a_gui_destroy() handles the normal
         * teardown path.  This case handles any other unrealize scenario. */
        if (gui->glInited) {
            ImGui_ImplOpenGL3_Shutdown();
            gui->glInited = false;
            gui_log("pugl_event_handler: PUGL_UNREALIZE, ImGui_ImplOpenGL3_Shutdown done");
        }
        break;

    case PUGL_CONFIGURE:
        gui->cachedWidth  = event->configure.width;
        gui->cachedHeight = event->configure.height;
        break;

    case PUGL_UPDATE:
        /* Request a redraw on every update so we render continuously */
        puglObscureView(view);
        break;

    case PUGL_EXPOSE:
        /* GL context active and drawing is allowed */
        if (gui->glInited)
            render_frame(gui);
        break;

    case PUGL_TIMER:
        /* The internal Pugl timer drives rendering when the host has no
         * timer_support. The callback (set in gui_create) routes back into
         * the plugin's normal timer pump. */
        if (event->timer.id == RENDER_TIMER_ID && gui->internalTimerCallback)
            gui->internalTimerCallback(gui->internalTimerUserData);
        break;

    case PUGL_CLOSE:
        gui->wasClosed = true;
        m4a_gui_stop_internal_timer(gui);
        if (gui->host) {
            const clap_host_gui_t *hostGui =
                (const clap_host_gui_t *)gui->host->get_extension(gui->host, CLAP_EXT_GUI);
            if (hostGui)
                hostGui->closed(gui->host, false /* was_destroyed */);
        }
        break;

    case PUGL_BUTTON_PRESS:
        /* Claim keyboard focus so that subsequent key/text events are routed
         * to our child window.  In embedded mode the host's message pump does
         * not automatically give the child focus on click. */
        puglGrabFocus(view);
        ImGui_ImplPugl_ProcessEvent(event);
        break;

    case PUGL_BUTTON_RELEASE:
        ImGui_ImplPugl_ProcessEvent(event);
        break;

    case PUGL_KEY_PRESS:
    case PUGL_KEY_RELEASE:
    {
        ImGuiIO &io = ImGui::GetIO();

        /* Tells Pugl to not handle spacebar presses *unless* ImGui wants text input
        See third_party/pugl/mac.m > key_down handler for more details */
        const PuglMods mods = event->key.state;
        const bool plainSpace =
            event->key.key == PUGL_KEY_SPACE &&
            (mods & (PUGL_MOD_SHIFT | PUGL_MOD_CTRL | PUGL_MOD_ALT | PUGL_MOD_SUPER)) == 0;

        if (gui->isEmbedded && plainSpace && !io.WantTextInput) // in case ur focusing text input
            return PUGL_UNSUPPORTED;

        ImGui_ImplPugl_ProcessEvent(event);
        break;
    }

    default:
        /* Forward all other input events to ImGui */
        ImGui_ImplPugl_ProcessEvent(event);
        break;
    }

    return PUGL_SUCCESS;
}

/* ---- Public C interface ---- */

extern "C" {

M4AGuiState *m4a_gui_create(const clap_host_t *host, const M4AGuiSettings *initial,
                             const char *log_path)
{
    s_logPath = log_path;

    M4AGuiState *gui = new M4AGuiState();
    memset(gui, 0, sizeof(*gui));
    gui->host         = host;
    gui->cachedWidth  = (uint32_t)GUI_W;
    gui->cachedHeight = (uint32_t)GUI_H;
    gui->selectedVoice       = 0;
    gui->pendingRestoreVoice = -1;

    if (initial) {
        gui->settings = *initial;
    } else {
        memset(&gui->settings, 0, sizeof(gui->settings));
        gui->settings.masterVolume     = 15;
        gui->settings.songMasterVolume = 127;
        gui->settings.pcmMixRate       = 13379.0f;
    }
    sync_buffers(gui);

    /* Create Pugl world and view */
    gui->world = puglNewWorld(PUGL_MODULE, 0);
    if (!gui->world) {
        gui_log("m4a_gui_create: puglNewWorld failed");
        delete gui;
        return nullptr;
    }
    puglSetWorldString(gui->world, PUGL_CLASS_NAME, "poryaaaa");

    gui->view = puglNewView(gui->world);
    if (!gui->view) {
        gui_log("m4a_gui_create: puglNewView failed");
        puglFreeWorld(gui->world);
        delete gui;
        return nullptr;
    }

    /* Configure the view */
    puglSetBackend(gui->view, puglGlBackend());
    puglSetViewHint(gui->view, PUGL_CONTEXT_API,           PUGL_OPENGL_API);
    puglSetViewHint(gui->view, PUGL_CONTEXT_VERSION_MAJOR, 3);
    puglSetViewHint(gui->view, PUGL_CONTEXT_VERSION_MINOR, 3);
    puglSetViewHint(gui->view, PUGL_CONTEXT_PROFILE,       PUGL_OPENGL_CORE_PROFILE);
    puglSetViewHint(gui->view, PUGL_DOUBLE_BUFFER,         1);
    puglSetViewHint(gui->view, PUGL_RESIZABLE,             1);
    puglSetSizeHint(gui->view, PUGL_DEFAULT_SIZE, (PuglSpan)GUI_W, (PuglSpan)GUI_H);
    puglSetSizeHint(gui->view, PUGL_MIN_SIZE,     (PuglSpan)200,   (PuglSpan)150);
    puglSetViewString(gui->view, PUGL_WINDOW_TITLE, "poryaaaa");

    puglSetHandle(gui->view, gui);
    puglSetEventFunc(gui->view, pugl_event_handler);

    /* Create ImGui context per instance */
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontGlobalScale = 1.2f;

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding    = ImVec2(12, 12);
    style.ItemSpacing      = ImVec2(8, 6);
    style.FramePadding     = ImVec2(6, 4);
    style.GrabMinSize      = 10.0f;
    style.WindowRounding   = 4.0f;
    style.FrameRounding    = 3.0f;
    style.GrabRounding     = 3.0f;

    ImGui_ImplPugl_Init(gui->view);

    gui->imguiCtx = ctx;

    /* Do NOT realize yet — that happens in set_parent() or show() */
    gui_log("m4a_gui_create: success");
    return gui;
}

void m4a_gui_destroy(M4AGuiState *gui)
{
    if (!gui)
        return;


    /* Stop the internal render timer before tearing down GL/ImGui */
    m4a_gui_stop_internal_timer(gui);

    ImGui::SetCurrentContext(gui->imguiCtx);

    /* puglFreeView() on Windows calls puglFreeViewInternals() which destroys
     * the GL context WITHOUT dispatching PUGL_UNREALIZE first.  Explicitly
     * enter the GL context and shut down the OpenGL backend before the view
     * (and its context) are freed. */
    if (gui->view && gui->glInited) {
        puglEnterContext(gui->view);
        ImGui_ImplOpenGL3_Shutdown();
        gui->glInited = false;
        puglLeaveContext(gui->view);
    }

    ImGui_ImplPugl_Shutdown();

    if (gui->view) {
        puglFreeView(gui->view);
        gui->view = nullptr;
    }

    ImGui::DestroyContext(gui->imguiCtx);
    gui->imguiCtx = nullptr;

    if (gui->world) {
        puglFreeWorld(gui->world);
        gui->world = nullptr;
    }

    delete gui;
    gui_log("m4a_gui_destroy: done");
}

bool m4a_gui_set_parent(M4AGuiState *gui, uintptr_t native_parent)
{
    gui_log("m4a_gui_set_parent: parent=0x%zx", (size_t)native_parent);
    if (!gui || !gui->view) return false;
    if (gui->realized) {
        gui_log("m4a_gui_set_parent: already realized");
        return false;
    }

    puglSetParent(gui->view, (PuglNativeView)native_parent);

    PuglStatus st = puglRealize(gui->view);
    if (st != PUGL_SUCCESS) {
        gui_log("m4a_gui_set_parent: puglRealize failed (%d)", (int)st);
        return false;
    }
    gui->realized   = true;
    gui->isEmbedded = true;
    gui_log("m4a_gui_set_parent: success");
    return true;
}

bool m4a_gui_show(M4AGuiState *gui)
{
    gui_log("m4a_gui_show called");
    if (!gui || !gui->view) return false;

    if (!gui->realized) {
        /* Floating mode: realize now (no parent) */
        PuglStatus st = puglRealize(gui->view);
        if (st != PUGL_SUCCESS) {
            gui_log("m4a_gui_show: puglRealize failed (%d)", (int)st);
            return false;
        }
        gui->realized = true;
        gui_log("m4a_gui_show: realized as floating");
    }

    /* Embedded views must not manipulate the host's window (orderFront etc.),
     * so use PUGL_SHOW_PASSIVE.  Floating windows should raise normally. */
    puglShow(gui->view, gui->isEmbedded ? PUGL_SHOW_PASSIVE : PUGL_SHOW_RAISE);
    return true;
}

bool m4a_gui_hide(M4AGuiState *gui)
{
    if (!gui || !gui->view) return false;
    m4a_gui_stop_internal_timer(gui);
    puglHide(gui->view);
    return true;
}

void m4a_gui_get_size(M4AGuiState *gui, uint32_t *width, uint32_t *height)
{
    if (!gui) {
        *width  = (uint32_t)GUI_W;
        *height = (uint32_t)GUI_H;
        return;
    }
    /* cachedWidth/Height are in backing pixels (from Pugl CONFIGURE).
     * The CLAP host expects logical pixels (points on macOS), so divide
     * by the backing scale factor. */
    double scale = gui->view ? puglGetScaleFactor(gui->view) : 1.0;
    if (scale < 1.0) scale = 1.0;
    *width  = (uint32_t)(gui->cachedWidth  / scale);
    *height = (uint32_t)(gui->cachedHeight / scale);
}

bool m4a_gui_set_size(M4AGuiState *gui, uint32_t width, uint32_t height)
{
    if (!gui || !gui->view) return false;
    /* The host sends logical pixels; Pugl's PUGL_CURRENT_SIZE expects
     * backing pixels, so scale up. */
    double scale = puglGetScaleFactor(gui->view);
    if (scale < 1.0) scale = 1.0;
    PuglSpan pw = (PuglSpan)(width  * scale);
    PuglSpan ph = (PuglSpan)(height * scale);
    puglSetSizeHint(gui->view, PUGL_CURRENT_SIZE, pw, ph);
    return true;
}

bool m4a_gui_can_resize(M4AGuiState *gui)
{
    return gui && gui->isEmbedded;
}

void m4a_gui_update_settings(M4AGuiState *gui, const M4AGuiSettings *settings)
{
    if (!gui || !settings) return;
    gui->settings = *settings;
    sync_buffers(gui);
}

bool m4a_gui_poll_changes(M4AGuiState *gui, M4AGuiSettings *out, bool *reload_voicegroup)
{
    if (!gui || !gui->settingsChanged)
        return false;

    *out               = gui->settings;
    *reload_voicegroup = gui->reloadRequested;
    gui->settingsChanged  = false;
    gui->reloadRequested  = false;
    return true;
}

bool m4a_gui_was_closed(M4AGuiState *gui)
{
    return gui && gui->wasClosed;
}

void m4a_gui_tick(M4AGuiState *gui)
{
    if (!gui || !gui->world)
        return;

    /* Schedule a redraw, then process events (non-blocking) */
    if (gui->view && gui->realized)
        puglObscureView(gui->view);

    puglUpdate(gui->world, 0.0);
}

void m4a_gui_set_internal_timer_callback(M4AGuiState *gui,
                                          M4AGuiTimerCallback callback,
                                          void *user_data)
{
    if (!gui)
        return;
    gui->internalTimerCallback = callback;
    gui->internalTimerUserData = user_data;
}

void m4a_gui_set_engine(M4AGuiState *gui, M4AEngine *engine)
{
    if (!gui) return;
    gui->engine = engine;
    gui->polyPrevValid = false;
}

void m4a_gui_set_voice_data(M4AGuiState *gui,
                             ToneData *liveVoices,
                             const ToneData *originalVoices,
                             bool *overrides,
                             const char (*voiceNames)[VG_VOICE_NAME_LEN])
{
    if (!gui) return;
    gui->liveVoices     = liveVoices;
    gui->originalVoices = originalVoices;
    gui->voiceOverrides = overrides;
    gui->voiceNames     = voiceNames;
    if (!liveVoices)
        gui->pendingRestoreVoice = -1;
}

bool m4a_gui_poll_voice_restore(M4AGuiState *gui, int *voiceIndex)
{
    if (!gui || gui->pendingRestoreVoice < 0)
        return false;
    *voiceIndex = gui->pendingRestoreVoice;
    gui->pendingRestoreVoice = -1;
    return true;
}

bool m4a_gui_poll_poly_reset(M4AGuiState *gui)
{
    if (!gui || !gui->polyResetRequested)
        return false;
    gui->polyResetRequested = false;
    return true;
}

bool m4a_gui_poll_voices_dirty(M4AGuiState *gui)
{
    if (!gui || !gui->voicesDirty)
        return false;
    gui->voicesDirty = false;
    return true;
}

void m4a_gui_start_internal_timer(M4AGuiState *gui)
{
    if (!gui || !gui->view || !gui->realized)
        return;
    if (gui->internalTimerActive)
        return;
    PuglStatus st = puglStartTimer(gui->view, RENDER_TIMER_ID, 1.0 / 60.0);
    if (st == PUGL_SUCCESS)
        gui->internalTimerActive = true;
}

void m4a_gui_stop_internal_timer(M4AGuiState *gui)
{
    if (!gui || !gui->view || !gui->internalTimerActive)
        return;

    puglStopTimer(gui->view, RENDER_TIMER_ID);
    gui->internalTimerActive = false;
}
} /* extern "C" */
