#ifndef M4A_GUI_H
#define M4A_GUI_H

#include <stdint.h>
#include <stdbool.h>
#include <clap/clap.h>
#include "m4a_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque GUI state handle */
typedef struct M4AGuiState M4AGuiState;

/* Settings shown/edited in the GUI */
typedef struct {
    char projectRoot[512];
    char voicegroupName[256];
    uint8_t reverbAmount;
    uint8_t masterVolume;
    uint8_t songMasterVolume;
    bool analogFilter;
    uint8_t maxPcmChannels;
    bool voicegroupLoaded;
} M4AGuiSettings;

/*
 * Create GUI resources. Returns NULL on failure (e.g. no display available).
 * Must be called from the main thread.
 * log_path: optional file path for diagnostic logging; NULL to disable.
 */
M4AGuiState *m4a_gui_create(const clap_host_t *host, const M4AGuiSettings *initial,
                             const char *log_path);

/*
 * Free all GUI resources. Must be called from the main thread.
 */
void m4a_gui_destroy(M4AGuiState *gui);

/* Show/hide the GUI window. Return true on success. */
bool m4a_gui_show(M4AGuiState *gui);
bool m4a_gui_hide(M4AGuiState *gui);

/* Get current window pixel dimensions. */
void m4a_gui_get_size(M4AGuiState *gui, uint32_t *width, uint32_t *height);

/* Resize the window (used by the host in embedded mode). */
bool m4a_gui_set_size(M4AGuiState *gui, uint32_t width, uint32_t height);

/* Returns true if the host may resize us (i.e. we are in embedded mode). */
bool m4a_gui_can_resize(M4AGuiState *gui);

/*
 * Embed the plugin window as a child of the host's native view.
 * native_parent is a platform-native handle (HWND on Win32, NSView* on macOS,
 * Window (XID) on X11). Must be called after m4a_gui_create() and before
 * m4a_gui_show().
 */
bool m4a_gui_set_parent(M4AGuiState *gui, uintptr_t native_parent);

/*
 * Poll events and render one frame. Call from the CLAP timer callback (~60 Hz).
 * Must be called from the main thread.
 */
void m4a_gui_tick(M4AGuiState *gui);

/*
 * Returns true if the GUI window has been closed by the user.
 */
bool m4a_gui_was_closed(M4AGuiState *gui);

/*
 * Push new settings into the GUI (e.g. after voicegroup reload).
 * Only the displayed values are updated; text input buffers are refreshed.
 */
void m4a_gui_update_settings(M4AGuiState *gui, const M4AGuiSettings *settings);

/*
 * Poll for user-initiated changes. Returns true if any setting changed.
 * If true, *out is filled with the new settings.
 * *reload_voicegroup is set to true if the user pressed "Reload".
 */
bool m4a_gui_poll_changes(M4AGuiState *gui, M4AGuiSettings *out, bool *reload_voicegroup);

/*
 * Provide the GUI with direct pointers to voice data for the voice editor tab.
 * Pass NULL for all pointers to clear (e.g. when voicegroup is unloaded).
 */
void m4a_gui_set_voice_data(M4AGuiState *gui,
                             ToneData *liveVoices,
                             const ToneData *originalVoices,
                             bool *overrides);

/*
 * Poll for a voice restore request. Returns true if the user clicked
 * "Restore Original" on a voice. *voiceIndex receives the voice index.
 */
bool m4a_gui_poll_voice_restore(M4AGuiState *gui, int *voiceIndex);

/*
 * Returns true (and clears) if any voice was edited since the last poll.
 * The plugin should call m4a_engine_refresh_voices() to propagate changes.
 */
bool m4a_gui_poll_voices_dirty(M4AGuiState *gui);

#ifdef __cplusplus
}
#endif

#endif /* M4A_GUI_H */
