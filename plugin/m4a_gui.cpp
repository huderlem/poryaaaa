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
static const int GUI_H = 350;

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
};

/* ---- Internal helpers ---- */

static void sync_buffers(M4AGuiState *gui)
{
    snprintf(gui->projectRootBuf, sizeof(gui->projectRootBuf),
             "%s", gui->settings.projectRoot);
    snprintf(gui->voicegroupBuf, sizeof(gui->voicegroupBuf),
             "%s", gui->settings.voicegroupName);
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
        ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoCollapse      |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("##Main", nullptr, wflags);

    /* ---- Plugin title ---- */
    ImGui::TextColored(ImVec4(0.3f, 0.75f, 1.0f, 1.0f), "poryaaaa");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 160.0f);
    ImGui::TextDisabled("pokeemerald");
    ImGui::Separator();
    ImGui::Spacing();

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
        if (ImGui::SliderInt("Polyphony (1-12)", &v, 1, MAX_PCM_CHANNELS)) {
            gui->settings.maxPcmChannels = (uint8_t)v;
            gui->settingsChanged = true;
        }
    }
    if (ImGui::Checkbox("GBA Analog Filter", &gui->settings.analogFilter))
        gui->settingsChanged = true;

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
            gui_log("pugl_event_handler: PUGL_REALIZE, ImGui_ImplOpenGL3_Init done");
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

    case PUGL_CLOSE:
        gui->wasClosed = true;
        gui_log("pugl_event_handler: PUGL_CLOSE");
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
    gui_log("m4a_gui_create: begin");

    M4AGuiState *gui = new M4AGuiState();
    memset(gui, 0, sizeof(*gui));
    gui->host         = host;
    gui->cachedWidth  = (uint32_t)GUI_W;
    gui->cachedHeight = (uint32_t)GUI_H;

    if (initial) {
        gui->settings = *initial;
    } else {
        memset(&gui->settings, 0, sizeof(gui->settings));
        gui->settings.masterVolume     = 15;
        gui->settings.songMasterVolume = 127;
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

    gui_log("m4a_gui_destroy: begin");

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
        gui_log("m4a_gui_destroy: ImGui_ImplOpenGL3_Shutdown done");
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

    puglShow(gui->view, PUGL_SHOW_RAISE);
    gui_log("m4a_gui_show: shown");
    return true;
}

bool m4a_gui_hide(M4AGuiState *gui)
{
    gui_log("m4a_gui_hide called");
    if (!gui || !gui->view) return false;
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
    *width  = gui->cachedWidth;
    *height = gui->cachedHeight;
}

bool m4a_gui_set_size(M4AGuiState *gui, uint32_t width, uint32_t height)
{
    if (!gui || !gui->view) return false;
    gui_log("m4a_gui_set_size: %ux%u", width, height);
    puglSetSizeHint(gui->view, PUGL_CURRENT_SIZE, (PuglSpan)width, (PuglSpan)height);
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

} /* extern "C" */
