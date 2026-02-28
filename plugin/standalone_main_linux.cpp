/*
 * Lightweight Linux standalone entry point.
 *
 * clap-wrapper's generic wrapasstandalone.cpp falls back to a headless
 * mainWait() loop on Linux (GTK3 is not available in this build), so it
 * never shows the Pugl/ImGui GUI.  This file provides a lightweight
 * alternative that:
 *
 *   1. Uses clap-wrapper's mainCreatePlugin / mainStartAudio for audio & MIDI.
 *   2. Creates the plugin's CLAP GUI (Pugl/ImGui) as a floating X11 window.
 *   3. Drives GUI rendering via a nanosleep-based ~60 Hz loop that calls
 *      on_timer directly (which in turn calls m4a_gui_tick / puglUpdate).
 */

#include <time.h>   // nanosleep
#include <cstdio>
#include <memory>
#include <string>

#include <clap/clap.h>
#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_host.h"
#include "clap_proxy.h"

extern "C" bool m4a_plugin_gui_was_closed(const clap_plugin_t *plugin);
extern "C" bool m4a_plugin_take_restart_request(const clap_plugin_t *plugin);

int main(int argc, char **argv)
{
    /* --- 1. Create plugin via clap-wrapper infrastructure --- */
    extern const clap_plugin_entry clap_entry;
    const clap_plugin_entry *entry = &clap_entry;
    std::string pid{PLUGIN_ID};

    auto plugin = freeaudio::clap_wrapper::standalone::mainCreatePlugin(
        entry, pid, 0, argc, argv);

    if (!plugin)
    {
        fprintf(stderr, "Failed to create plugin\n");
        return 1;
    }

    /* --- 2. Start audio & MIDI --- */
    freeaudio::clap_wrapper::standalone::mainStartAudio();

    /* --- 3. Create and show the plugin GUI as a floating X11 window --- */
    if (plugin->_ext._gui)
    {
        plugin->_ext._gui->create(plugin->_plugin, CLAP_WINDOW_API_X11, true);
        plugin->_ext._gui->show(plugin->_plugin);
    }

    /* --- 4. ~60 Hz event loop: nanosleep then drive GUI via on_timer --- */
    auto *sah = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    while (sah->running)
    {
        struct timespec ts = {0, 16000000}; /* 16 ms ≈ 60 Hz */
        nanosleep(&ts, nullptr);

        /* Render GUI frame and process X11 events */
        if (plugin->_ext._timer)
            plugin->_ext._timer->on_timer(plugin->_plugin, 0);

        /* Fire any pending main-thread callbacks */
        if (sah->callbackRequested.exchange(false))
            plugin->_plugin->on_main_thread(plugin->_plugin);

        /* Restart audio/plugin when the user clicks Reload */
        if (m4a_plugin_take_restart_request(plugin->_plugin))
        {
            sah->stopAudioThread();
            /* stopAudioThread() sets running=false; reset it so the loop
             * keeps running and startAudioThread() processes correctly. */
            sah->running = true;
            sah->finishedRunning = false;
            sah->startAudioThread();
        }

        /* Exit when the user closes the GUI window */
        if (m4a_plugin_gui_was_closed(plugin->_plugin))
        {
            sah->running = false;
            break;
        }
    }

    /* --- 5. Tear down --- */
    if (plugin->_ext._gui)
        plugin->_ext._gui->destroy(plugin->_plugin);

    plugin.reset();
    freeaudio::clap_wrapper::standalone::mainFinish();
    return 0;
}
