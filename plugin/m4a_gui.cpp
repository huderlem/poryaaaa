/*
 * m4a_gui.cpp - Dear ImGui + GLFW floating window GUI for the M4A plugin.
 *
 * Provides a simple settings panel where the user can change the project
 * root, voicegroup, reverb, and volume levels in real time from the DAW.
 *
 * Thread-safety: all functions must be called from the main thread.
 */

/* GLFW (includes <GL/gl.h> on Linux/Windows, <OpenGL/gl.h> on macOS) */
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#  define GLFW_EXPOSE_NATIVE_WIN32
#  include <GLFW/glfw3native.h>
#endif

/* Dear ImGui core */
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

/* C standard library */
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ---- Debug logging ---- */
/*
 * Set M4A_GUI_LOG to a writable file path to enable diagnostic logging.
 * On Windows: "C:/Users/Public/m4a_plugin.log" or similar.
 * Leave as nullptr to disable.
 */
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

/* CLAP GUI extension (for notifying host when floating window closes) */
#include <clap/ext/gui.h>

/* ---- Constants ---- */

static const int GUI_W = 540;
static const int GUI_H = 350;

/* ---- GLFW global reference count ---- */
/*
 * glfwInit/glfwTerminate are called once globally. We ref-count them so that
 * multiple plugin instances each calling create/destroy works correctly.
 * Only ever accessed from the main thread (CLAP guarantee), so no locking needed.
 */
static int s_glfwRefCount = 0;

static bool glfw_acquire()
{
    if (s_glfwRefCount == 0) {
        glfwSetErrorCallback([](int code, const char *msg) {
            (void)code;
            fprintf(stderr, "[m4a_plugin] GLFW error %d: %s\n", code, msg);
        });
        if (!glfwInit())
            return false;
    }
    s_glfwRefCount++;
    return true;
}

static void glfw_release()
{
    if (s_glfwRefCount > 0 && --s_glfwRefCount == 0)
        glfwTerminate();
}

/* ---- GUI state ---- */

struct M4AGuiState {
    GLFWwindow    *window;
    ImGuiContext  *imguiCtx;
    const clap_host_t *host;

    /* Currently displayed settings */
    M4AGuiSettings settings;

    /* Editable text buffers (not applied until "Reload" is clicked) */
    char projectRootBuf[512];
    char voicegroupBuf[256];

    /* Pending change flags (cleared by poll_changes) */
    bool settingsChanged;
    bool reloadRequested;

    /* True after set_parent_win32() — host drives sizing and visibility */
    bool isEmbedded;
};

/* ---- Internal helpers ---- */

/* Sync text buffers from settings (called on create and update_settings) */
static void sync_buffers(M4AGuiState *gui)
{
    snprintf(gui->projectRootBuf, sizeof(gui->projectRootBuf),
             "%s", gui->settings.projectRoot);
    snprintf(gui->voicegroupBuf, sizeof(gui->voicegroupBuf),
             "%s", gui->settings.voicegroupName);
}

/* ---- Public C interface ---- */

extern "C" {

M4AGuiState *m4a_gui_create(const clap_host_t *host, const M4AGuiSettings *initial,
                             const char *log_path)
{
    s_logPath = log_path;
    gui_log("m4a_gui_create: begin");

    if (!glfw_acquire()) {
        gui_log("m4a_gui_create: glfwInit() failed");
        return nullptr;
    }
    gui_log("m4a_gui_create: glfwInit() OK (refcount=%d)", s_glfwRefCount);

    /* Request an OpenGL 3.3 core context */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   /* hidden until show() */
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);

    GLFWwindow *window = glfwCreateWindow(
        GUI_W, GUI_H, "M4A GBA Sound Engine", nullptr, nullptr);
    if (!window) {
        gui_log("m4a_gui_create: glfwCreateWindow() failed");
        glfw_release();
        return nullptr;
    }
    gui_log("m4a_gui_create: glfwCreateWindow() OK");

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); /* vsync */

    /* Set up Dear ImGui with a fresh context per instance */
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;                      /* no .ini persistence */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontGlobalScale = 1.2f;                     /* slightly larger text */

    ImGui::StyleColorsDark();

    /* Tweak style for a cleaner look */
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding    = ImVec2(12, 12);
    style.ItemSpacing      = ImVec2(8, 6);
    style.FramePadding     = ImVec2(6, 4);
    style.GrabMinSize      = 10.0f;
    style.WindowRounding   = 4.0f;
    style.FrameRounding    = 3.0f;
    style.GrabRounding     = 3.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    /* Allocate and populate state */
    M4AGuiState *gui = new M4AGuiState();
    gui->window       = window;
    gui->imguiCtx     = ctx;
    gui->host         = host;
    gui->settingsChanged  = false;
    gui->reloadRequested  = false;
    gui->isEmbedded       = false;

    if (initial) {
        gui->settings = *initial;
    } else {
        memset(&gui->settings, 0, sizeof(gui->settings));
        gui->settings.masterVolume     = 15;
        gui->settings.songMasterVolume = 127;
    }
    sync_buffers(gui);

    /* Store back-pointer for the close callback */
    glfwSetWindowUserPointer(window, gui);

    /* Intercept window close: hide instead of destroying, notify host */
    glfwSetWindowCloseCallback(window, [](GLFWwindow *w) {
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        glfwHideWindow(w);
        auto *g = static_cast<M4AGuiState *>(glfwGetWindowUserPointer(w));
        if (g && g->host) {
            auto *hostGui = static_cast<const clap_host_gui_t *>(
                g->host->get_extension(g->host, CLAP_EXT_GUI));
            if (hostGui)
                hostGui->closed(g->host, false /* was_destroyed */);
        }
    });

    gui_log("m4a_gui_create: success");
    return gui;
}

void m4a_gui_destroy(M4AGuiState *gui)
{
    if (!gui)
        return;

    glfwMakeContextCurrent(gui->window);
    ImGui::SetCurrentContext(gui->imguiCtx);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(gui->imguiCtx);

    glfwDestroyWindow(gui->window);
    glfw_release();

    delete gui;
}

bool m4a_gui_show(M4AGuiState *gui)
{
    gui_log("m4a_gui_show called (gui=%p)", (void*)gui);
    if (!gui) return false;
    glfwShowWindow(gui->window);
    glfwFocusWindow(gui->window);
    gui_log("m4a_gui_show: window shown");
    return true;
}

bool m4a_gui_hide(M4AGuiState *gui)
{
    gui_log("m4a_gui_hide called");
    if (!gui) return false;
    glfwHideWindow(gui->window);
    return true;
}

void m4a_gui_get_size(M4AGuiState *gui, uint32_t *width, uint32_t *height)
{
    if (!gui) {
        *width  = (uint32_t)GUI_W;
        *height = (uint32_t)GUI_H;
        return;
    }
    int w, h;
    glfwGetWindowSize(gui->window, &w, &h);
    *width  = (uint32_t)w;
    *height = (uint32_t)h;
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

    *out              = gui->settings;
    *reload_voicegroup = gui->reloadRequested;
    gui->settingsChanged  = false;
    gui->reloadRequested  = false;
    return true;
}

void m4a_gui_tick(M4AGuiState *gui)
{
    if (!gui || !gui->window)
        return;

    /* Always pump GLFW events so OS messages are handled */
    glfwPollEvents();

    /* Skip rendering when window is hidden */
    if (!glfwGetWindowAttrib(gui->window, GLFW_VISIBLE))
        return;

    glfwMakeContextCurrent(gui->window);
    ImGui::SetCurrentContext(gui->imguiCtx);

    /* ---- New frame ---- */
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    /* Fill the entire GLFW window with a single ImGui window */
    int fbW, fbH;
    glfwGetFramebufferSize(gui->window, &fbW, &fbH);

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
    ImGui::TextColored(ImVec4(0.3f, 0.75f, 1.0f, 1.0f), "M4A GBA Sound Engine");
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

    ImGui::End();

    /* ---- Render ---- */
    ImGui::Render();
    glViewport(0, 0, fbW, fbH);
    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(gui->window);
}

bool m4a_gui_can_resize(M4AGuiState *gui)
{
    return gui && gui->isEmbedded;
}

bool m4a_gui_set_size(M4AGuiState *gui, uint32_t width, uint32_t height)
{
    if (!gui) return false;
    gui_log("m4a_gui_set_size: %ux%u", width, height);
#if defined(_WIN32)
    if (gui->isEmbedded) {
        HWND hwnd = glfwGetWin32Window(gui->window);
        if (hwnd) {
            SetWindowPos(hwnd, nullptr, 0, 0, (int)width, (int)height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
            return true;
        }
    }
#endif
    glfwSetWindowSize(gui->window, (int)width, (int)height);
    return true;
}

#if defined(_WIN32)
bool m4a_gui_set_parent_win32(M4AGuiState *gui, void *parentHwnd)
{
    if (!gui || !parentHwnd) return false;

    HWND hwnd = glfwGetWin32Window(gui->window);
    if (!hwnd) {
        gui_log("m4a_gui_set_parent: glfwGetWin32Window returned NULL");
        return false;
    }
    gui_log("m4a_gui_set_parent: hwnd=%p parent=%p", (void *)hwnd, parentHwnd);

    /* Convert from a top-level window to a child window */
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_POPUP);
    style |= WS_CHILD;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_APPWINDOW | WS_EX_OVERLAPPEDWINDOW | WS_EX_TOPMOST);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

    /* Re-parent and position at origin of the host's client area */
    SetParent(hwnd, (HWND)parentHwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, GUI_W, GUI_H,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    gui->isEmbedded = true;
    gui_log("m4a_gui_set_parent: success (embedded)");
    return true;
}
#endif

} /* extern "C" */
