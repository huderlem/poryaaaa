/*
 * Minimal Win32 standalone entry point for MinGW cross-compilation.
 *
 * clap-wrapper's native Windows standalone (windows_standalone.cpp) requires
 * WinRT and WIL, which are unavailable under MinGW.  This file provides a
 * lightweight alternative that:
 *
 *   1. Uses clap-wrapper's mainCreatePlugin / mainStartAudio for audio & MIDI.
 *   2. Creates the plugin's CLAP GUI (Pugl/ImGui) as a floating window.
 *   3. Drives GUI rendering via a Win32 timer that calls on_timer directly.
 *   4. Runs a standard Windows message pump until the Pugl window is closed.
 */

#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>

#include <clap/clap.h>
#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_host.h"
#include "clap_proxy.h"

extern "C" bool m4a_plugin_gui_was_closed(const clap_plugin_t *plugin);
extern "C" bool m4a_plugin_take_restart_request(const clap_plugin_t *plugin);

static std::shared_ptr<Clap::Plugin> g_plugin;
static HWND g_msgWindow = nullptr;
static const UINT_PTR RENDER_TIMER_ID = 1;

/* ------------------------------------------------------------------ */
/*  Hidden message-only window for WM_TIMER delivery                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK msgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TIMER && wParam == RENDER_TIMER_ID)
    {
        /* Drive the plugin's GUI rendering by calling on_timer.
         * The timer_id argument doesn't matter for our plugin since
         * we're calling on_timer unconditionally, and our plugin's
         * timer_on_timer just renders a frame regardless of the id
         * when the GUI exists. We pass 0 as a dummy. */
        if (g_plugin && g_plugin->_ext._timer)
        {
            g_plugin->_ext._timer->on_timer(g_plugin->_plugin, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool createMessageWindow()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = msgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PoryaaaaStandaloneMsg";

    if (!RegisterClassW(&wc))
        return false;

    g_msgWindow = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    return g_msgWindow != nullptr;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

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
    g_plugin = plugin;

    /* --- 2. Start audio & MIDI --- */
    freeaudio::clap_wrapper::standalone::mainStartAudio();

    /* --- 3. Create the hidden window and start a ~60 Hz render timer --- */
    if (!createMessageWindow())
    {
        fprintf(stderr, "Failed to create message window\n");
        return 1;
    }

    /* --- 4. Create and show the plugin GUI as a floating window --- */
    if (plugin->_ext._gui)
    {
        plugin->_ext._gui->create(plugin->_plugin, CLAP_WINDOW_API_WIN32, true);

        uint32_t w = 0, h = 0;
        plugin->_ext._gui->get_size(plugin->_plugin, &w, &h);
        plugin->_ext._gui->show(plugin->_plugin);
    }

    /* Start the render timer after GUI creation so on_timer has a window to render */
    SetTimer(g_msgWindow, RENDER_TIMER_ID, 16, nullptr);

    /* --- 5. Message pump – runs until the standalone host signals stop --- */
    auto *sah = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    MSG msg;
    while (sah->running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        /* Fire any pending main-thread callbacks */
        if (sah->callbackRequested.exchange(false))
        {
            plugin->_plugin->on_main_thread(plugin->_plugin);
        }

        /* Restart audio/plugin when the user clicks Reload */
        if (m4a_plugin_take_restart_request(plugin->_plugin))
        {
            sah->stopAudioThread();
            /* stopAudioThread() sets running=false; reset it so the message
             * loop keeps running and startAudioThread() processes correctly. */
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

        /* Yield a bit so we don't spin at 100 % CPU between timer ticks */
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
    }

    /* --- 6. Tear down --- */
    KillTimer(g_msgWindow, RENDER_TIMER_ID);

    if (plugin->_ext._gui)
    {
        plugin->_ext._gui->destroy(plugin->_plugin);
    }
    g_plugin.reset();
    plugin.reset();
    freeaudio::clap_wrapper::standalone::mainFinish();

    if (g_msgWindow)
        DestroyWindow(g_msgWindow);

    return 0;
}
