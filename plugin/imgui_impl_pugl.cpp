// imgui_impl_pugl.cpp - Dear ImGui platform backend for Pugl
// Replaces imgui_impl_glfw with a Pugl-based implementation.

#include "imgui_impl_pugl.h"
#include "imgui.h"

#include <pugl/pugl.h>
#include <pugl/gl.h>

#include <string>
#include <string.h>
#include <float.h>

struct ImGui_ImplPugl_Data {
    PuglView*   View;
    double      Time;
    std::string ClipboardText;
};

static ImGui_ImplPugl_Data* ImGui_ImplPugl_GetBackendData()
{
    return ImGui::GetCurrentContext() ?
        (ImGui_ImplPugl_Data*)ImGui::GetIO().BackendPlatformUserData : nullptr;
}

static const char* ImGui_ImplPugl_GetClipboardText(void*)
{
    ImGui_ImplPugl_Data* bd = ImGui_ImplPugl_GetBackendData();
    if (!bd) return "";
    // Request an async paste; the actual data comes via PUGL_DATA event.
    puglPaste(bd->View);
    return bd->ClipboardText.c_str();
}

static void ImGui_ImplPugl_SetClipboardText(void*, const char* text)
{
    ImGui_ImplPugl_Data* bd = ImGui_ImplPugl_GetBackendData();
    if (!bd) return;
    puglSetClipboard(bd->View, "text/plain", text, strlen(text) + 1);
}

static ImGuiKey PuglKeyToImGuiKey(uint32_t key)
{
    switch (key)
    {
    case PUGL_KEY_TAB:          return ImGuiKey_Tab;
    case PUGL_KEY_LEFT:         return ImGuiKey_LeftArrow;
    case PUGL_KEY_RIGHT:        return ImGuiKey_RightArrow;
    case PUGL_KEY_UP:           return ImGuiKey_UpArrow;
    case PUGL_KEY_DOWN:         return ImGuiKey_DownArrow;
    case PUGL_KEY_PAGE_UP:      return ImGuiKey_PageUp;
    case PUGL_KEY_PAGE_DOWN:    return ImGuiKey_PageDown;
    case PUGL_KEY_HOME:         return ImGuiKey_Home;
    case PUGL_KEY_END:          return ImGuiKey_End;
    case PUGL_KEY_INSERT:       return ImGuiKey_Insert;
    case PUGL_KEY_DELETE:       return ImGuiKey_Delete;
    case PUGL_KEY_BACKSPACE:    return ImGuiKey_Backspace;
    case PUGL_KEY_SPACE:        return ImGuiKey_Space;
    case PUGL_KEY_ENTER:        return ImGuiKey_Enter;
    case PUGL_KEY_ESCAPE:       return ImGuiKey_Escape;
    case PUGL_KEY_PAD_ENTER:    return ImGuiKey_KeypadEnter;
    case PUGL_KEY_F1:           return ImGuiKey_F1;
    case PUGL_KEY_F2:           return ImGuiKey_F2;
    case PUGL_KEY_F3:           return ImGuiKey_F3;
    case PUGL_KEY_F4:           return ImGuiKey_F4;
    case PUGL_KEY_F5:           return ImGuiKey_F5;
    case PUGL_KEY_F6:           return ImGuiKey_F6;
    case PUGL_KEY_F7:           return ImGuiKey_F7;
    case PUGL_KEY_F8:           return ImGuiKey_F8;
    case PUGL_KEY_F9:           return ImGuiKey_F9;
    case PUGL_KEY_F10:          return ImGuiKey_F10;
    case PUGL_KEY_F11:          return ImGuiKey_F11;
    case PUGL_KEY_F12:          return ImGuiKey_F12;
    // Modifier keys
    case PUGL_KEY_SHIFT_L:      return ImGuiKey_LeftShift;
    case PUGL_KEY_SHIFT_R:      return ImGuiKey_RightShift;
    case PUGL_KEY_CTRL_L:       return ImGuiKey_LeftCtrl;
    case PUGL_KEY_CTRL_R:       return ImGuiKey_RightCtrl;
    case PUGL_KEY_ALT_L:        return ImGuiKey_LeftAlt;
    case PUGL_KEY_ALT_R:        return ImGuiKey_RightAlt;
    case PUGL_KEY_SUPER_L:      return ImGuiKey_LeftSuper;
    case PUGL_KEY_SUPER_R:      return ImGuiKey_RightSuper;
    case PUGL_KEY_MENU:         return ImGuiKey_Menu;
    case PUGL_KEY_CAPS_LOCK:    return ImGuiKey_CapsLock;
    case PUGL_KEY_SCROLL_LOCK:  return ImGuiKey_ScrollLock;
    case PUGL_KEY_NUM_LOCK:     return ImGuiKey_NumLock;
    case PUGL_KEY_PRINT_SCREEN: return ImGuiKey_PrintScreen;
    case PUGL_KEY_PAUSE:        return ImGuiKey_Pause;
    // Keypad
    case PUGL_KEY_PAD_0:        return ImGuiKey_Keypad0;
    case PUGL_KEY_PAD_1:        return ImGuiKey_Keypad1;
    case PUGL_KEY_PAD_2:        return ImGuiKey_Keypad2;
    case PUGL_KEY_PAD_3:        return ImGuiKey_Keypad3;
    case PUGL_KEY_PAD_4:        return ImGuiKey_Keypad4;
    case PUGL_KEY_PAD_5:        return ImGuiKey_Keypad5;
    case PUGL_KEY_PAD_6:        return ImGuiKey_Keypad6;
    case PUGL_KEY_PAD_7:        return ImGuiKey_Keypad7;
    case PUGL_KEY_PAD_8:        return ImGuiKey_Keypad8;
    case PUGL_KEY_PAD_9:        return ImGuiKey_Keypad9;
    case PUGL_KEY_PAD_DECIMAL:  return ImGuiKey_KeypadDecimal;
    case PUGL_KEY_PAD_DIVIDE:   return ImGuiKey_KeypadDivide;
    case PUGL_KEY_PAD_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case PUGL_KEY_PAD_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case PUGL_KEY_PAD_ADD:      return ImGuiKey_KeypadAdd;
    case PUGL_KEY_PAD_EQUAL:    return ImGuiKey_KeypadEqual;
    default: break;
    }

    // ASCII printable characters (a-z, 0-9, punctuation)
    if (key >= 'a' && key <= 'z')
        return (ImGuiKey)(ImGuiKey_A + (key - 'a'));
    if (key >= 'A' && key <= 'Z')
        return (ImGuiKey)(ImGuiKey_A + (key - 'A'));
    if (key >= '0' && key <= '9')
        return (ImGuiKey)(ImGuiKey_0 + (key - '0'));

    switch (key)
    {
    case '\'': return ImGuiKey_Apostrophe;
    case ',':  return ImGuiKey_Comma;
    case '-':  return ImGuiKey_Minus;
    case '.':  return ImGuiKey_Period;
    case '/':  return ImGuiKey_Slash;
    case ';':  return ImGuiKey_Semicolon;
    case '=':  return ImGuiKey_Equal;
    case '[':  return ImGuiKey_LeftBracket;
    case '\\': return ImGuiKey_Backslash;
    case ']':  return ImGuiKey_RightBracket;
    case '`':  return ImGuiKey_GraveAccent;
    default:   return ImGuiKey_None;
    }
}

static void UpdateModifiers(ImGuiIO& io, PuglMods mods)
{
    io.AddKeyEvent(ImGuiMod_Ctrl,  (mods & PUGL_MOD_CTRL)  != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (mods & PUGL_MOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt,   (mods & PUGL_MOD_ALT)   != 0);
    io.AddKeyEvent(ImGuiMod_Super, (mods & PUGL_MOD_SUPER) != 0);
}

bool ImGui_ImplPugl_Init(PuglView* view)
{
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendPlatformUserData == nullptr && "Already initialized!");

    ImGui_ImplPugl_Data* bd = IM_NEW(ImGui_ImplPugl_Data)();
    bd->View = view;
    bd->Time = 0.0;

    io.BackendPlatformUserData = (void*)bd;
    io.BackendPlatformName = "imgui_impl_pugl";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

    io.SetClipboardTextFn = ImGui_ImplPugl_SetClipboardText;
    io.GetClipboardTextFn = ImGui_ImplPugl_GetClipboardText;

    return true;
}

void ImGui_ImplPugl_Shutdown()
{
    ImGui_ImplPugl_Data* bd = ImGui_ImplPugl_GetBackendData();
    IM_ASSERT(bd != nullptr && "Not initialized!");

    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformUserData = nullptr;
    io.BackendPlatformName = nullptr;

    IM_DELETE(bd);
}

void ImGui_ImplPugl_NewFrame()
{
    ImGui_ImplPugl_Data* bd = ImGui_ImplPugl_GetBackendData();
    IM_ASSERT(bd != nullptr && "Not initialized!");

    ImGuiIO& io = ImGui::GetIO();

    // Update display size from the current view frame
    PuglArea size = puglGetSizeHint(bd->View, PUGL_CURRENT_SIZE);
    io.DisplaySize = ImVec2((float)size.width, (float)size.height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // Update time
    double currentTime = puglGetTime(puglGetWorld(bd->View));
    if (currentTime <= bd->Time)
        currentTime = bd->Time + 0.00001;
    io.DeltaTime = bd->Time > 0.0 ? (float)(currentTime - bd->Time) : (float)(1.0 / 60.0);
    bd->Time = currentTime;

    // Update cursor shape
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
        return;

    ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
    PuglCursor pugl_cursor = PUGL_CURSOR_ARROW;
    switch (imgui_cursor)
    {
    case ImGuiMouseCursor_TextInput:         pugl_cursor = PUGL_CURSOR_CARET; break;
    case ImGuiMouseCursor_ResizeAll:         pugl_cursor = PUGL_CURSOR_ALL_SCROLL; break;
    case ImGuiMouseCursor_ResizeNS:          pugl_cursor = PUGL_CURSOR_UP_DOWN; break;
    case ImGuiMouseCursor_ResizeEW:          pugl_cursor = PUGL_CURSOR_LEFT_RIGHT; break;
    case ImGuiMouseCursor_ResizeNESW:        pugl_cursor = PUGL_CURSOR_UP_RIGHT_DOWN_LEFT; break;
    case ImGuiMouseCursor_ResizeNWSE:        pugl_cursor = PUGL_CURSOR_UP_LEFT_DOWN_RIGHT; break;
    case ImGuiMouseCursor_Hand:              pugl_cursor = PUGL_CURSOR_HAND; break;
    case ImGuiMouseCursor_NotAllowed:        pugl_cursor = PUGL_CURSOR_NO; break;
    default:                                 pugl_cursor = PUGL_CURSOR_ARROW; break;
    }
    puglSetCursor(bd->View, pugl_cursor);
}

void ImGui_ImplPugl_ProcessEvent(const PuglEvent* event)
{
    ImGui_ImplPugl_Data* bd = ImGui_ImplPugl_GetBackendData();
    if (!bd) return;

    ImGuiIO& io = ImGui::GetIO();

    switch (event->type)
    {
    case PUGL_FOCUS_IN:
        io.AddFocusEvent(true);
        break;

    case PUGL_FOCUS_OUT:
        io.AddFocusEvent(false);
        break;

    case PUGL_KEY_PRESS:
    case PUGL_KEY_RELEASE:
    {
        const bool pressed = (event->type == PUGL_KEY_PRESS);
        UpdateModifiers(io, event->key.state);
        ImGuiKey key = PuglKeyToImGuiKey(event->key.key);
        if (key != ImGuiKey_None)
            io.AddKeyEvent(key, pressed);
        break;
    }

    case PUGL_TEXT:
        // AddInputCharacter takes a uint32_t (Unicode codepoint)
        io.AddInputCharacter(event->text.character);
        break;

    case PUGL_BUTTON_PRESS:
    case PUGL_BUTTON_RELEASE:
    {
        const bool pressed = (event->type == PUGL_BUTTON_PRESS);
        UpdateModifiers(io, event->button.state);
        // Pugl buttons are 0-indexed, same as ImGui
        if (event->button.button < 5)
            io.AddMouseButtonEvent((int)event->button.button, pressed);
        break;
    }

    case PUGL_MOTION:
        UpdateModifiers(io, event->motion.state);
        io.AddMousePosEvent((float)event->motion.x, (float)event->motion.y);
        break;

    case PUGL_SCROLL:
        UpdateModifiers(io, event->scroll.state);
        io.AddMouseWheelEvent((float)event->scroll.dx, (float)event->scroll.dy);
        break;

    case PUGL_POINTER_OUT:
        /* When embedded in a DAW host on X11, the host may grab the pointer on
           click, causing a LeaveNotify even though the cursor is still over our
           window.  Pugl reports this as PUGL_CROSSING_GRAB.  Only invalidate
           the mouse position for real leaves (PUGL_CROSSING_NORMAL). */
        if (event->crossing.mode == PUGL_CROSSING_NORMAL)
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        break;

    case PUGL_DATA_OFFER:
    {
        // Accept the first text/plain type offered
        uint32_t numTypes = puglGetNumClipboardTypes(bd->View);
        for (uint32_t i = 0; i < numTypes; i++)
        {
            const char* type = puglGetClipboardType(bd->View, i);
            if (type && strcmp(type, "text/plain") == 0)
            {
                puglAcceptOffer(bd->View, &event->offer, i);
                break;
            }
        }
        break;
    }

    case PUGL_DATA:
    {
        // Clipboard data is ready
        size_t len = 0;
        const void* data = puglGetClipboard(bd->View, event->data.typeIndex, &len);
        if (data && len > 0)
        {
            // Ensure null termination
            bd->ClipboardText.assign((const char*)data, len);
            // Strip null terminator from std::string if present
            if (!bd->ClipboardText.empty() && bd->ClipboardText.back() == '\0')
                bd->ClipboardText.pop_back();
        }
        break;
    }

    default:
        break;
    }
}
