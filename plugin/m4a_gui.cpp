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
 * On Windows: "C:/Users/Public/poryaaaa_plugin.log" or similar.
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
#include "m4a_engine.h"

/* CLAP GUI extension (for notifying host when floating window closes) */
#include <clap/ext/gui.h>
#include <clap/ext/draft/undo.h>

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
            fprintf(stderr, "[poryaaaa] GLFW error %d: %s\n", code, msg);
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

#if defined(_WIN32)
    HHOOK  msgHook;         /* WH_GETMESSAGE hook handle, or NULL */
    bool   pendingPaste;    /* Ctrl+V intercepted, inject on next tick */
    bool   pendingCopy;     /* Ctrl+C */
    bool   pendingCut;      /* Ctrl+X */
    bool   pendingSelectAll;/* Ctrl+A */
    bool   pendingUndo;          /* Ctrl+Z intercepted */
    bool   pendingRedo;          /* Ctrl+Y or Ctrl+Shift+Z intercepted */
    bool   wantTextInput;        /* WantTextInput from previous frame */
#endif
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

#if defined(_WIN32)
static LRESULT CALLBACK m4a_get_msg_hook(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == PM_REMOVE)
    {
        MSG *msg = reinterpret_cast<MSG *>(lParam);
        if (msg->message == WM_KEYDOWN)
        {
            M4AGuiState *gui =
                static_cast<M4AGuiState *>(::GetPropA(msg->hwnd, "M4A_GUI_STATE"));
            if (gui && (::GetKeyState(VK_CONTROL) & 0x8000))
            {
                bool consumed = true;
                switch (msg->wParam)
                {
                case 'V': gui->pendingPaste      = true; break;
                case 'C': gui->pendingCopy       = true; break;
                case 'X': gui->pendingCut        = true; break;
                case 'A': gui->pendingSelectAll  = true; break;
                case 'Z':
                    if (gui->wantTextInput) {
                        if (::GetKeyState(VK_SHIFT) & 0x8000)
                            gui->pendingRedo = true;
                        else
                            gui->pendingUndo = true;
                    } else {
                        consumed = false;
                    }
                    break;
                case 'Y':
                    if (gui->wantTextInput)
                        gui->pendingRedo = true;
                    else
                        consumed = false;
                    break;
                default:  consumed = false;              break;
                }
                if (consumed)
                    msg->message = WM_NULL;  /* prevents TranslateAcceleratorW match */
            }
        }
    }
    return ::CallNextHookEx(NULL, nCode, wParam, lParam);
}
#endif

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
        GUI_W, GUI_H, "poryaaaa", nullptr, nullptr);
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

#if defined(_WIN32)
    gui->msgHook          = NULL;
    gui->pendingPaste     = false;
    gui->pendingCopy      = false;
    gui->pendingCut       = false;
    gui->pendingSelectAll = false;
    gui->pendingUndo      = false;
    gui->pendingRedo      = false;
    gui->wantTextInput    = false;
#endif

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

#if defined(_WIN32)
    {
        HWND hwnd = glfwGetWin32Window(gui->window);
        if (gui->msgHook) {
            ::UnhookWindowsHookEx(gui->msgHook);
            gui->msgHook = NULL;
        }
        if (hwnd)
            ::RemovePropA(hwnd, "M4A_GUI_STATE");
    }
#endif

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

#if defined(_WIN32)
    if (gui->isEmbedded) {
        ImGuiIO &io = ImGui::GetIO();
        const bool ctrlNowHeld = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

        auto inject_ctrl_key = [&](ImGuiKey key) {
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(key, true);
            io.AddKeyEvent(key, false);
            if (!ctrlNowHeld)
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
        };

        if (gui->pendingPaste)     { inject_ctrl_key(ImGuiKey_V); gui->pendingPaste     = false; }
        if (gui->pendingCopy)      { inject_ctrl_key(ImGuiKey_C); gui->pendingCopy      = false; }
        if (gui->pendingCut)       { inject_ctrl_key(ImGuiKey_X); gui->pendingCut       = false; }
        if (gui->pendingSelectAll) { inject_ctrl_key(ImGuiKey_A); gui->pendingSelectAll = false; }

        /* Cache WantTextInput for the hook to use next frame.
           The hook only intercepts Z/Y when this is true, so if pendingUndo/Redo
           is set, a text field was active — always inject into ImGui. */
        gui->wantTextInput = ImGui::GetIO().WantTextInput;

        if (gui->pendingUndo) { inject_ctrl_key(ImGuiKey_Z); gui->pendingUndo = false; }
        if (gui->pendingRedo) { inject_ctrl_key(ImGuiKey_Y); gui->pendingRedo = false; }
    }
#endif

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

    ::SetPropA(hwnd, "M4A_GUI_STATE", gui);

    gui->msgHook = ::SetWindowsHookExW(
        WH_GETMESSAGE, m4a_get_msg_hook,
        NULL, ::GetCurrentThreadId());
    if (!gui->msgHook)
        gui_log("m4a_gui_set_parent: SetWindowsHookExW failed (err=%lu)", ::GetLastError());
    else
        gui_log("m4a_gui_set_parent: WH_GETMESSAGE hook installed");

    gui->isEmbedded = true;
    gui_log("m4a_gui_set_parent: success (embedded)");
    return true;
}
#endif

} /* extern "C" */
